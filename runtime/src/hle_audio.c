/* HLE audio boundary: host replacements for the XACT engine and DirectSound entry points that
 * title code (and BINK) calls in the statically linked XACTENG/DSOUND libraries.
 *
 * Boundary (derived from disassembly of this XBE): the title
 * only calls flat stdcall wrappers (IXACTEngine_X(pThis, ...), IDirectSoundBuffer_X(pThis, ...)),
 * never object vtables, so returned objects are opaque host allocations. Argument bytes come
 * from each wrapper's own "ret N". Names ending in '?' are inferred from the wrapper's
 * arguments and internal callees, not from symbols.
 *
 * Behavior: DirectSound buffers are mixer voices (src/audio.c) whose play cursor advances as the
 * mixer consumes them; XACT wave banks (in memory or streamed from the title's file handle) and
 * the sound bank are parsed (src/xact.c), cues play their waves as voices, and the title's
 * registered notifications are queued when cues are prepared or stop. 3D sound entries are
 * attenuated by distance and panned from their sound source's position relative to the listener
 * (DirectSound 3D model). The title's parameter controls ("V"
 * volume, "F" fade) set through Prepare/PlayEx and SetParameterControl move each cue's volume
 * along the sound bank's curves. Doppler, cones, pitch, effects
 * and category volumes are not applied. */
#define _GNU_SOURCE
#include "hle_audio.h"
#include "audio.h"
#include "xact.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <strings.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

#define S_OK 0u
#define E_FAIL 0x80004005u
#define E_INVALIDARG 0x80070057u
#define DSBPLAY_LOOPING 1u
#define DSBSTATUS_PLAYING 1u
#define DSBSTATUS_LOOPING 4u
#define DSBLOCK_FROMWRITECURSOR 1u
#define DSBLOCK_ENTIREBUFFER 2u

/* Stack frame built by hle_entry_common (hle_thunk.S). */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t stub_ret, index, ret_addr;
    uint32_t args[];
} HleFrame;

enum { OBJ_ENGINE = 1, OBJ_SOUNDBANK, OBJ_WAVEBANK, OBJ_SOURCE, OBJ_CUE, OBJ_EFFECTS, OBJ_DSOUND, OBJ_DSBUFFER };
enum { CUE_PREPARED = 1, CUE_PLAYING, CUE_STOPPED };

/* One parameter control of a cue: the value the title set, an XACT ramp (value + step * rate,
 * one step per XACTEngineDoWork up to steps) and the summed output of its volume curves. */
typedef struct CueCtl {
    XsbControl c;
    float value, rate;
    uint32_t step, steps;
    int ramping;
    int32_t volume;             /* hundredths of a dB (0 until the title sets the control) */
} CueCtl;
#define CUE_MAX_CTL 4           /* this title's sounds have 2 ("V" and "P" or "F") */

/* Opaque object handed to the title. The first 64 bytes stay zero (a stray dereference reads
 * NULL instead of host state); bookkeeping follows. Objects are never freed: the title may
 * still hold them after a Release or a stop. */
typedef struct HleObj {
    uint8_t guest_view[64];
    uint32_t magic, type, refs;
    /* DirectSound buffer */
    AudioVoice *voice;
    uint8_t *data;              /* buffer storage (own allocation or title memory via SetBufferData) */
    uint32_t bytes, cursor, play_flags, playing, frame_bytes, rate;
    int channels, bits;
    int32_t volume;             /* hundredths of a dB */
    float mix[2][2];
    /* XACT wave bank / sound bank */
    XwbBank wb;
    int wb_ok;
    XsbBank sb;
    int sb_ok;
    /* XACT cue */
    struct HleObj *bank, *next;
    uint32_t index, source, state, ready_pending;
    int nvoices;
    AudioVoice *vv[XSB_MAX_WAVES];
    WaveReader *rd[XSB_MAX_WAVES];
    int chans[XSB_MAX_WAVES];
    float gain;                 /* entry volume plus parameter controls (linear) */
    int32_t entry_volume;       /* hundredths of a dB */
    int nctl;
    CueCtl ctl[CUE_MAX_CTL];    /* the sound's parameter controls */
    int is3d;
    float min_dist, max_dist;
    /* XACT sound source */
    float pos[3];
    int has_pos;
} HleObj;
#define HLE_MAGIC 0x4F454C48u   /* "HLEO" */


#define G(p) ((void *)(uintptr_t)(p))
#define GA(p) ((uint32_t)(uintptr_t)(p))

static pthread_mutex_t hle_lock = PTHREAD_MUTEX_INITIALIZER;
int hle_audio_log_limit = 32;
int (*hle_audio_handle_fd)(uint32_t handle);

static void xlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void xlog(const char *fmt, ...) {
    if (hle_audio_log_limit <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&hle_lock);
    printf("[xact] ");
    vprintf(fmt, ap);
    printf("\n");
    pthread_mutex_unlock(&hle_lock);
    va_end(ap);
}

static HleObj *obj_new(uint32_t type) {
    HleObj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->magic = HLE_MAGIC;
    o->type = type;
    o->refs = 1;
    return o;
}
static HleObj *obj_get(uint32_t p, uint32_t type) {
    HleObj *o = G(p);
    return (o && o->magic == HLE_MAGIC && o->type == type) ? o : NULL;
}
/* Writes a new object to a guest out-pointer (if any) and returns the HRESULT. */
static uint32_t obj_out(uint32_t out, uint32_t type, HleObj **made) {
    if (made) *made = NULL;
    if (!out) return S_OK;   /* optional out-parameter (e.g. PlayEx without ppCue) */
    HleObj *o = obj_new(type);
    if (!o) return 0x8007000Eu;   /* E_OUTOFMEMORY */
    *(uint32_t *)G(out) = GA(o);
    if (made) *made = o;
    return S_OK;
}
static void zero_out(uint32_t p, size_t n) { if (p) memset(G(p), 0, n); }
static uint32_t rd32g(uint32_t p, int i) { return ((const uint32_t *)G(p))[i]; }

/* ---- XACT notifications ----
 * A notification description (title code and the XBE's matcher at 0x3cef58): u16 type, u16
 * flags, sound bank, cue index (-1 = any), cue (compared only with flag 4), then context.
 * GetNotification copies 10 dwords. Types the title uses: 1 with flag 4 (registered per cue
 * after PlayEx: the cue stopped) and 3 with flags 0x8002 (registered once per sound bank,
 * logged by the title as "StreamDataAvailable"; posted when a cue is prepared). */
#define MAX_REG 256
#define MAX_QUEUE 256
typedef struct { uint32_t d[6]; } NotifyDesc;
static NotifyDesc regs[MAX_REG], queue[MAX_QUEUE];
static int nregs, nqueue;
static HleObj *cues;            /* prepared and playing cues */
static HleObj *wavebanks[512];
static int nwavebanks;
static uint32_t xact_rng = 1;
/* Listener (IXACTEngine_SetListenerPosition/Orientation): DirectSound's default pose. */
static float lis_pos[3], lis_front[3] = {0, 0, 1}, lis_top[3] = {0, 1, 0};

static int desc_match(const uint32_t *desc, const uint32_t *rec) {
    uint16_t dt = desc[0] & 0xFFFF, rt = rec[0] & 0xFFFF;
    if (dt != rt && dt != 0xFFFF) return 0;
    if ((desc[0] >> 16 & 7) != (rec[0] >> 16 & 0xFFF7)) return 0;
    if (desc[1] != rec[1]) return 0;
    if (desc[0] >> 16 & 4) return desc[3] == rec[3];
    return desc[2] == rec[2] || desc[2] == 0xFFFFFFFFu;
}

/* Queues a notification of the given type for every registration that matches the cue. */
static void notify(uint32_t type, HleObj *cue) {
    for (int i = 0; i < nregs; i++) {
        uint32_t *r = regs[i].d;
        if ((r[0] & 0xFFFF) != type || r[1] != GA(cue->bank)) continue;
        if (r[2] != 0xFFFFFFFFu && r[2] != cue->index) continue;
        if ((r[0] >> 16 & 4) && r[3] != GA(cue)) continue;
        if (nqueue == MAX_QUEUE) { memmove(queue, queue + 1, sizeof queue - sizeof *queue); nqueue--; }
        NotifyDesc *q = &queue[nqueue++];
        *q = regs[i];
        q->d[0] = type | (r[0] >> 16 & 7) << 16;
        q->d[2] = cue->index;
        q->d[3] = GA(cue);
        xlog("notify type %u cue %x (index %x)", type, GA(cue), cue->index);
        if (!(r[0] >> 16 & 0x8000)) { regs[i] = regs[--nregs]; i--; }   /* one-shot registration */
    }
}

/* ---- XACT cues ---- */
static HleObj *find_wavebank(const char *name) {
    for (int i = 0; i < nwavebanks; i++)
        if (wavebanks[i]->wb_ok && !strcasecmp(wavebanks[i]->wb.name, name)) return wavebanks[i];
    return NULL;
}

static void cue_release_voices(HleObj *c) {
    for (int i = 0; i < c->nvoices; i++) {
        voice_free(c->vv[i]);
        free(c->rd[i]);
        c->vv[i] = NULL;
        c->rd[i] = NULL;
    }
    c->nvoices = 0;
}

static void cue_unlink(HleObj *c) {
    for (HleObj **p = &cues; *p; p = &(*p)->next)
        if (*p == c) { *p = c->next; break; }
    c->next = NULL;
}

static void cue_stop(HleObj *c) {
    if (c->state != CUE_PREPARED && c->state != CUE_PLAYING) return;
    audio_lock();
    cue_release_voices(c);
    c->state = CUE_STOPPED;
    cue_unlink(c);
    notify(1, c);
    audio_unlock();
}

/* 3D sounds: distance attenuation and stereo pan from the cue's sound source and the listener
 * (DirectSound 3D model, src/audio.c). A 3D cue without a positioned source is heard at the
 * listener. 2D cues keep the entry volume set when they were built. */
static float sfx3d_gain = 1.0f;   /* --sfx-3d-volume */

int hle_audio_parse_db(const char *s, float *db) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end || !(v >= HLE_AUDIO_SFX3D_MIN_DB && v <= HLE_AUDIO_SFX3D_MAX_DB)) return -1;
    *db = (float)v;
    return 0;
}

void hle_audio_set_sfx_3d_volume(float db) { sfx3d_gain = powf(10.0f, db / 20.0f); }
float hle_audio_sfx_3d_gain(void) { return sfx3d_gain; }

static void cue_apply_3d(HleObj *c) {
    if (!c->is3d) return;
    HleObj *src = obj_get(c->source, OBJ_SOURCE);
    float g = c->gain * sfx3d_gain, lr[2] = {0.70710678f, 0.70710678f};   /* centered: constant power */
    if (src && src->has_pos) {
        float dx = src->pos[0] - lis_pos[0], dy = src->pos[1] - lis_pos[1], dz = src->pos[2] - lis_pos[2];
        g *= audio_ds3d_distance_gain(sqrtf(dx * dx + dy * dy + dz * dz), c->min_dist, c->max_dist, 1.0f);
        audio_ds3d_pan(lis_pos, lis_front, lis_top, src->pos, c->min_dist, lr);
    }
    for (int i = 0; i < c->nvoices; i++) {
        if (c->chans[i] == 1) voice_set_gains(c->vv[i], (const float[2][2]){{lr[0], lr[1]}, {0, 0}});
        voice_set_volume(c->vv[i], g);   /* stereo waves: distance only (DS3D voices are mono) */
    }
}

/* ---- XACT parameter controls ---- */
static int xact_controls = 1;   /* --no-xact-controls */
static uint32_t ctl_sets, ctl_unmatched, ctl_global;
void hle_audio_set_xact_controls(int on) { xact_controls = on != 0; }
int hle_audio_xact_controls(void) { return xact_controls; }

/* Entry volume plus the controls' volume curves, clamped to <= 0 as 0x3d77b7 does before it adds
 * the category volumes (all 0 dB in this title, not applied). */
static void cue_update_gain(HleObj *c) {
    int32_t v = c->entry_volume;
    if (xact_controls)
        for (int i = 0; i < c->nctl; i++) v += c->ctl[i].volume;
    c->gain = audio_db100_to_gain(v > 0 ? 0 : v);
}

/* The control's volume at its current ramp step (0x3cfd20, 0x3d8970): the outputs of its volume
 * curves add up. Every curve in this title's bank is on track 0 and every sound has one track,
 * so the volume applies to all of the cue's voices. Pitch curves ("P") are not applied. */
static void ctl_eval(HleObj *c, CueCtl *k) {
    float v = (float)k->step * k->rate + k->value;
    int32_t sum = 0;
    XsbCurve cv;
    for (int i = 0; i < k->c.ncurves; i++)
        if (!xsb_curve(&c->bank->sb, &k->c, i, &cv) && cv.target == XSB_TARGET_VOLUME) sum += xsb_curve_eval(&cv, v);
    k->volume = sum;
}

static void cue_refresh(HleObj *c) {
    cue_update_gain(c);
    if (c->is3d) cue_apply_3d(c);
    else
        for (int i = 0; i < c->nvoices; i++) voice_set_volume(c->vv[i], c->gain);
}

/* Sets a control by name (the first of the sound's controls with that name, as 0x3d9224 matches);
 * names the sound does not have are ignored. Applied at once, also when the title asks for the
 * next DoWork (flags bit 0 clear): at most one title frame earlier than the console. */
static void cue_set_control(HleObj *c, const char *name, float value, float rate, uint32_t steps) {
    for (int i = 0; i < c->nctl; i++) {
        CueCtl *k = &c->ctl[i];
        if (strncmp(k->c.name, name, 16)) continue;
        k->value = value;
        k->rate = rate;
        k->steps = steps;
        k->step = 0;
        ctl_eval(c, k);
        k->step = 1;
        k->ramping = steps > 0;
        cue_refresh(c);
        ctl_sets++;
        xlog("cue %x index %x: control %s = %.4f -> %d (cue volume %.1f dB)", GA(c), c->index, k->c.name, value, k->volume,
             20.0 * log10(c->gain > 1e-5f ? c->gain : 1e-5f));
        return;
    }
    ctl_unmatched++;
}

/* XACT ramps: one step per XACTEngineDoWork (0x3d00b2), steps 1..steps after the first apply. */
static void cue_ramp(HleObj *c) {
    int changed = 0;
    for (int i = 0; i < c->nctl; i++) {
        CueCtl *k = &c->ctl[i];
        if (!k->ramping) continue;
        ctl_eval(c, k);
        if (++k->step > k->steps) k->ramping = 0;
        changed = 1;
    }
    if (changed) cue_refresh(c);
}

/* Control descriptions {count, entries} (Prepare/PlayEx desc+0xc, SetParameterControl): 0x18-byte
 * entries {u16 type, u16 flags, u16, u16 steps, f32 rate, f32 value, u32 cue, name pointer}
 * (0x6dea0, 0x3cfe65). Type 1 sets a control of one cue: the cue being prepared or played, else
 * the entry's cue handle. Type 0 ("MasterVolume", a category volume) is reported and skipped:
 * this title never sends it. A name pointer with a zero high word is a numeric id (unused). */
static void apply_controls(uint32_t pdesc, HleObj *cue) {
    if (!pdesc || !xact_controls) return;
    uint32_t n = rd32g(pdesc, 0), ents = rd32g(pdesc, 1);
    if (!ents || n > 64) return;
    audio_lock();
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = G(ents + i * 0x18);
        uint16_t type, steps;
        float rate, value;
        uint32_t handle, np;
        memcpy(&type, e, 2);
        memcpy(&steps, e + 6, 2);
        memcpy(&rate, e + 8, 4);
        memcpy(&value, e + 12, 4);
        memcpy(&handle, e + 16, 4);
        memcpy(&np, e + 20, 4);
        char name[17] = {0};
        if (np >> 16)
            for (int j = 0; j < 16 && ((const char *)G(np))[j]; j++) name[j] = ((const char *)G(np))[j];
        if (!(type & 1)) {
            if (!ctl_global++) printf("[audio] XACT global parameter control '%s' (type %u) not applied\n", name, type);
            continue;
        }
        HleObj *c = cue ? cue : obj_get(handle, OBJ_CUE);
        if (!name[0] || !c || c->state == CUE_STOPPED) { ctl_unmatched++; continue; }
        cue_set_control(c, name, value, rate, steps);
    }
    audio_unlock();
}

/* Stops cues whose voices all reached their end; with work=1 (XACTEngineDoWork) also posts the
 * "data available" notification of newly prepared cues (asynchronous on the Xbox, so never
 * inside Prepare itself). */
static void xact_reap(int work) {
    audio_lock();
    for (HleObj *c = cues, *n; c; c = n) {
        n = c->next;
        if (work && c->ready_pending) { c->ready_pending = 0; notify(3, c); }
        if (work) cue_ramp(c);
        if (c->state != CUE_PLAYING) continue;
        if (work) cue_apply_3d(c);   /* once per title frame: listener and sources move */
        int live = 0;
        for (int i = 0; i < c->nvoices; i++) live |= voice_playing(c->vv[i]);
        if (!live) cue_stop(c);
    }
    audio_unlock();
}

/* Builds the voices of a cue (stopped). Unknown cues and missing wave banks give a silent cue
 * that ends at once, so the title still sees it play and stop. */
static void cue_build(HleObj *c) {
    HleObj *sbo = c->bank;
    XsbSound s;
    if (!sbo || !sbo->sb_ok || xsb_cue_sound(&sbo->sb, c->index, &xact_rng, &s)) {
        xlog("cue %x: index %x not in the sound bank", GA(c), c->index);
        return;
    }
    c->entry_volume = s.volume;
    int first;
    c->nctl = xsb_sound_controls(&sbo->sb, s.entry, &first);
    if (c->nctl > CUE_MAX_CTL) c->nctl = CUE_MAX_CTL;
    for (int i = 0; i < c->nctl; i++) xsb_control(&sbo->sb, first + i, &c->ctl[i].c);
    cue_update_gain(c);
    c->is3d = s.is3d;
    c->min_dist = s.min_dist;
    c->max_dist = s.max_dist;
    for (int i = 0; i < s.nwaves; i++) {
        char name[17];
        xsb_bank_name(&sbo->sb, s.w[i].bank, name);
        HleObj *wbo = find_wavebank(name);
        if (!wbo || s.w[i].wave >= wbo->wb.count) {
            xlog("cue index %x: wave %d of bank %s not available", c->index, s.w[i].wave, name);
            continue;
        }
        const XwbEntry *e = &wbo->wb.entries[s.w[i].wave];
        WaveReader *r = malloc(sizeof *r);
        if (!r || e->codec < 0 || e->codec > XWB_XADPCM) { free(r); continue; }
        wave_reader_init(r, &wbo->wb, e, s.w[i].loops);
        AudioVoice *v = voice_new(e->channels, e->rate, wave_reader_fill, r);
        if (!v) { free(r); continue; }
        voice_set_volume(v, c->gain);
        c->chans[c->nvoices] = e->channels;
        c->vv[c->nvoices] = v;
        c->rd[c->nvoices++] = r;
        xlog("cue index %x: entry %d wave %s/%d %s %dch %u Hz %u bytes loops %d vol %d%s", c->index, s.entry, name,
             s.w[i].wave, e->codec == XWB_XADPCM ? "adpcm" : "pcm", e->channels, e->rate, e->length, s.w[i].loops, s.volume,
             s.is3d ? " 3D" : "");
    }
}

static HleObj *cue_new(HleObj *bank, uint32_t index, uint32_t source) {
    HleObj *c = obj_new(OBJ_CUE);
    if (!c) return NULL;
    c->bank = bank;
    c->index = index;
    c->source = source;
    c->state = CUE_PREPARED;
    audio_lock();
    cue_build(c);
    c->next = cues;
    cues = c;
    audio_unlock();
    return c;
}

static void cue_play(HleObj *c) {
    audio_lock();
    c->state = CUE_PLAYING;
    cue_apply_3d(c);
    for (int i = 0; i < c->nvoices; i++) voice_play(c->vv[i], 1);
    audio_unlock();
}

/* ---- XACT entry points ---- */
static uint32_t xact_set_control(const uint32_t *a) { apply_controls(a[1], NULL); return S_OK; }   /* (eng, pDesc) */
static uint32_t ok(const uint32_t *a) { (void)a; return S_OK; }
static uint32_t xact_engine_create(const uint32_t *a) { return obj_out(a[1], OBJ_ENGINE, NULL); }   /* (pParams, ppEngine) */
static uint32_t xact_do_work(const uint32_t *a) { (void)a; xact_reap(1); return S_OK; }

/* (eng, pv, size, ppBank): the sound bank stays in title memory while registered. */
static uint32_t xact_create_bank(const uint32_t *a) {
    HleObj *o;
    uint32_t r = obj_out(a[3], OBJ_SOUNDBANK, &o);
    char err[96];
    if (o && a[1]) {
        o->sb_ok = !xsb_parse(G(a[1]), a[2], &o->sb, err, sizeof err);
        if (o->sb_ok) xlog("sound bank %x: %d cues, %d entries, %d wave bank names", GA(o), o->sb.cues, o->sb.entries, o->sb.banks);
        else printf("[audio] sound bank at %08x: %s\n", a[1], err);
    }
    return r;
}

static void add_wavebank(HleObj *o) {
    if (nwavebanks < (int)(sizeof wavebanks / sizeof *wavebanks)) wavebanks[nwavebanks++] = o;
    xlog("wave bank %x: %s, %d entries, %s", GA(o), o->wb.name, o->wb.count, o->wb.fd >= 0 ? "streamed" : "in memory");
}

/* (eng, pv, size, ppWB): in-memory wave bank. */
static uint32_t xact_register_wb(const uint32_t *a) {
    HleObj *o;
    uint32_t r = obj_out(a[3], OBJ_WAVEBANK, &o);
    char err[96];
    if (o && a[1]) {
        audio_lock();
        o->wb_ok = !xwb_parse(G(a[1]), a[2], &o->wb, NULL, err, sizeof err);
        if (o->wb_ok) { o->wb.mem = (const uint8_t *)G(a[1]) + rd32g(a[1], 8); add_wavebank(o); }
        else printf("[audio] wave bank at %08x: %s\n", a[1], err);
        audio_unlock();
    }
    return r;
}

/* (eng, pParams{hFile, dwOffset, dwPacketSize...}, ppWB): streamed from the title's file
 * handle; the host reads it with its own descriptor (dup), so closing the handle after
 * UnRegisterWaveBank does not disturb a voice still being reaped. */
static uint32_t xact_register_swb(const uint32_t *a) {
    HleObj *o;
    uint32_t r = obj_out(a[2], OBJ_WAVEBANK, &o);
    char err[96];
    if (o && a[1]) {
        int fd = hle_audio_handle_fd ? hle_audio_handle_fd(rd32g(a[1], 0)) : -1;
        int own = fd >= 0 ? dup(fd) : -1;
        audio_lock();
        o->wb_ok = own >= 0 && !xwb_open_stream(own, rd32g(a[1], 1), &o->wb, err, sizeof err);
        if (o->wb_ok) add_wavebank(o);
        else {
            printf("[audio] streamed wave bank (handle %x): %s\n", rd32g(a[1], 0), own < 0 ? "no host file" : err);
            if (own >= 0) close(own);
        }
        audio_unlock();
    }
    return r;
}

static uint32_t xact_unregister_wb(const uint32_t *a) {   /* (eng, pWB) */
    HleObj *o = obj_get(a[1], OBJ_WAVEBANK);
    if (!o || !o->wb_ok) return S_OK;
    audio_lock();
    for (HleObj *c = cues, *n; c; c = n) {   /* stop cues still reading from this bank */
        n = c->next;
        for (int i = 0; i < c->nvoices; i++)
            if (c->rd[i] && c->rd[i]->wb == &o->wb) { cue_stop(c); break; }
    }
    for (int i = 0; i < nwavebanks; i++)
        if (wavebanks[i] == o) { wavebanks[i] = wavebanks[--nwavebanks]; break; }
    o->wb_ok = 0;
    if (o->wb.fd >= 0) close(o->wb.fd);
    xwb_free(&o->wb);
    audio_unlock();
    return S_OK;
}

static uint32_t xact_create_source(const uint32_t *a) { return obj_out(a[2], OBJ_SOURCE, NULL); }    /* (eng, flags, ppSrc) */

/* Prepare?(bank, pParams{dwFlags, dwCueIndex, pSoundSource, ...}, ppCue) */
static uint32_t xact_prepare(const uint32_t *a) {
    HleObj *bank = obj_get(a[0], OBJ_SOUNDBANK);
    if (!a[1]) return E_INVALIDARG;
    HleObj *c = cue_new(bank, rd32g(a[1], 1), rd32g(a[1], 2));
    if (!c) return 0x8007000Eu;
    apply_controls(rd32g(a[1], 3), c);   /* the description's initial control values */
    if (a[2]) *(uint32_t *)G(a[2]) = GA(c);
    c->ready_pending = 1;
    return S_OK;
}

/* PlayEx(bank, pParams{dwFlags, dwCueIndex, pSoundSource, ...}, ppCue): flag 4 plays the cue
 * prepared in *ppCue (XACT_CSoundBank_Play at 0x3d16c9) on pSoundSource. The title prepares
 * with no source and polls the source's GetStatus afterwards: a cue left without its source read
 * as idle, so the title stopped every prepared cue (voices, crew commands, most effects) within
 * a frame. */
static uint32_t xact_play(const uint32_t *a) {
    HleObj *bank = obj_get(a[0], OBJ_SOUNDBANK);
    if (!a[1]) return E_INVALIDARG;
    uint32_t flags = rd32g(a[1], 0);
    HleObj *c = (flags & 4) && a[2] ? obj_get(*(uint32_t *)G(a[2]), OBJ_CUE) : NULL;
    if (c && c->state == CUE_STOPPED) c = NULL;
    if (!c) {
        c = cue_new(bank, rd32g(a[1], 1), rd32g(a[1], 2));
        if (!c) return 0x8007000Eu;
    } else if (rd32g(a[1], 2)) {   /* the title prepares with no source and names it here */
        c->source = rd32g(a[1], 2);
    }
    apply_controls(rd32g(a[1], 3), c);
    cue_play(c);
    if (a[2]) *(uint32_t *)G(a[2]) = GA(c);
    xact_reap(0);   /* a cue without playable waves stops at once */
    return S_OK;
}

/* Stop?(bank, dwCueIndex (-1 = all), dwFlags, pCue) */
static uint32_t xact_stop(const uint32_t *a) {
    HleObj *bank = obj_get(a[0], OBJ_SOUNDBANK), *cue = obj_get(a[3], OBJ_CUE);
    audio_lock();
    if (cue) cue_stop(cue);
    else if (bank)
        for (HleObj *c = cues, *n; c; c = n) {
            n = c->next;
            if (c->bank == bank && (a[1] == 0xFFFFFFFFu || c->index == a[1])) cue_stop(c);
        }
    audio_unlock();
    return S_OK;
}

static uint32_t xact_register_notification(const uint32_t *a) {   /* (eng, pDesc) */
    if (!a[1]) return E_INVALIDARG;
    audio_lock();
    if (nregs < MAX_REG) memcpy(regs[nregs++].d, G(a[1]), sizeof regs[0].d);
    audio_unlock();
    return S_OK;
}
static uint32_t xact_unregister_notification(const uint32_t *a) {   /* (eng, pDesc): the registered description */
    if (!a[1]) return E_INVALIDARG;
    audio_lock();
    for (int i = 0; i < nregs; i++)
        if (!memcmp(G(a[1]), regs[i].d, 16)) { regs[i] = regs[--nregs]; i--; }
    audio_unlock();
    return S_OK;
}
static uint32_t xact_flush_notification(const uint32_t *a) {   /* (eng, pDesc) */
    if (!a[1]) return E_INVALIDARG;
    audio_lock();
    for (int i = 0; i < nqueue; i++)
        if (desc_match(G(a[1]), queue[i].d)) { memmove(queue + i, queue + i + 1, (nqueue - i - 1) * sizeof *queue); nqueue--; i--; }
    audio_unlock();
    return S_OK;
}
/* (eng, pDesc, pNotification): E_FAIL when nothing matching is queued; the title treats only
 * 0 as "notification received". */
static uint32_t xact_get_notification(const uint32_t *a) {
    if (!a[1]) return E_FAIL;
    xact_reap(0);
    uint32_t r = E_FAIL;
    audio_lock();
    for (int i = 0; i < nqueue; i++)
        if (desc_match(G(a[1]), queue[i].d)) {
            if (a[2]) { memset(G(a[2]), 0, 40); memcpy(G(a[2]), queue[i].d, sizeof queue[i].d); }
            memmove(queue + i, queue + i + 1, (nqueue - i - 1) * sizeof *queue);
            nqueue--;
            r = S_OK;
            break;
        }
    audio_unlock();
    return r;
}
/* (eng, pOut): internal code fills DSCAPS at +0x40, output levels and 3 bytes at +0x54. */
static uint32_t xact_get_levels(const uint32_t *a) { zero_out(a[1], 0x58); return S_OK; }
/* (bank, index, pProps): the 0x64-byte block of the XBE's own code (0x3d12a7, see
 * xsb_cue_props): the title queries it before preparing and while playing each cue; it used to
 * get zeros (duration, loop count, 3D flag and parameters). */
static uint32_t wave_ms(void *ctx, int bank, int wave) {
    char name[17];
    HleObj *wbo = find_wavebank(xsb_bank_name(ctx, bank, name));
    return wbo && wave >= 0 && wave < wbo->wb.count ? xwb_entry_ms(&wbo->wb.entries[wave]) : XSB_NO_MS;
}
static uint32_t xact_cue_props(const uint32_t *a) {
    HleObj *bank = obj_get(a[0], OBJ_SOUNDBANK);
    if (!a[2]) return S_OK;
    uint32_t props[25];
    audio_lock();
    if (!bank || !bank->sb_ok || xsb_cue_props(&bank->sb, a[1], wave_ms, &bank->sb, props)) memset(props, 0, sizeof props);
    audio_unlock();
    memcpy(G(a[2]), props, sizeof props);   /* zero-filled for an unknown bank or cue */
    return S_OK;
}
/* (src, pdwStatus): bit 0 = a cue is playing on the source; the title polls it to free voices. */
static uint32_t xact_source_status(const uint32_t *a) {
    uint32_t st = 0;
    xact_reap(0);
    audio_lock();
    for (HleObj *c = cues; c; c = c->next)
        if (c->state == CUE_PLAYING && c->source && c->source == a[0]) st = 1;
    audio_unlock();
    if (a[1]) *(uint32_t *)G(a[1]) = st;
    return S_OK;
}
static uint32_t xact_source_stop(const uint32_t *a) {   /* (src) */
    audio_lock();
    for (HleObj *c = cues, *n; c; c = n) {
        n = c->next;
        if (c->source && c->source == a[0]) cue_stop(c);
    }
    audio_unlock();
    return S_OK;
}
/* Listener and source poses: float arguments, then dwApply (the title passes 1 =
 * DS3D_DEFERRED and calls CommitDeferredSettings every frame; the values are applied to the
 * playing cues at the next XACTEngineDoWork, i.e. once per title frame either way). */
static void get_vec(const uint32_t *a, float v[3]) { memcpy(v, a, 3 * sizeof(float)); }
static uint32_t xact_listener_pos(const uint32_t *a) {   /* (eng, x, y, z, apply) */
    audio_lock();
    get_vec(a + 1, lis_pos);
    audio_unlock();
    return S_OK;
}
static uint32_t xact_listener_orient(const uint32_t *a) {   /* (eng, xFront, yFront, zFront, xTop, yTop, zTop, apply) */
    audio_lock();
    get_vec(a + 1, lis_front);
    get_vec(a + 4, lis_top);
    audio_unlock();
    return S_OK;
}
static uint32_t xact_source_pos(const uint32_t *a) {   /* (src, x, y, z, apply) */
    HleObj *s = obj_get(a[0], OBJ_SOURCE);
    if (!s) return S_OK;
    audio_lock();
    get_vec(a + 1, s->pos);
    s->has_pos = 1;
    audio_unlock();
    return S_OK;
}
static uint32_t xaudio_effects_image(const uint32_t *a) { return obj_out(a[3], OBJ_EFFECTS, NULL); } /* (name, pLoc, flags, ppDesc) */

/* ---- DirectSound ----
 * Buffers are mixer voices that read the buffer memory at a play cursor the mixer advances, so
 * GetCurrentPosition follows what was actually mixed (BINK streams its movie audio this way). */
static uint32_t ds_create(const uint32_t *a) { return obj_out(a[1], OBJ_DSOUND, NULL); }   /* (pGuid, ppDS, pUnkOuter) */
static uint32_t ds_release(const uint32_t *a) {
    HleObj *o = G(a[0]);
    if (!o || o->magic != HLE_MAGIC) return 0;
    if (o->refs && !--o->refs && o->voice) voice_play(o->voice, 0);
    return o->refs;
}

/* Mixer source: frames at the play cursor; a one-shot buffer ends at its last byte. */
static int dsb_fill(void *ctx, int16_t *dst, int max) {
    HleObj *b = ctx;
    uint32_t fb = b->frame_bytes, end = fb ? b->bytes / fb * fb : 0;
    int done = 0;
    if (!b->data || !end) return 0;
    while (done < max) {
        if (b->cursor >= end) {
            if (!(b->play_flags & DSBPLAY_LOOPING)) break;
            b->cursor = 0;
        }
        uint32_t n = (end - b->cursor) / fb;
        if (n > (uint32_t)(max - done)) n = (uint32_t)(max - done);
        const uint8_t *p = b->data + b->cursor;
        for (uint32_t i = 0; i < n * b->channels; i++)
            dst[done * b->channels + i] = b->bits == 16 ? (int16_t)(p[2 * i] | p[2 * i + 1] << 8) : (int16_t)((p[i] - 128) << 8);
        b->cursor += n * fb;
        done += (int)n;
    }
    return done;
}

/* Xbox mix bins: 0 front left, 1 front right, 2 center, 4/5 back left/right. */
static void dsb_set_mixbins(HleObj *b, uint32_t pmixbins) {
    float g[2][2] = {{0}};
    uint32_t n = pmixbins ? rd32g(pmixbins, 0) : 0, pairs = n ? rd32g(pmixbins, 1) : 0;
    if (!n || !pairs) return;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint32_t bin = rd32g(pairs, 2 * i);
        float v = audio_db100_to_gain((int32_t)rd32g(pairs, 2 * i + 1));
        int src = b->channels == 2 ? (int)(i % 2) : 0;
        if (bin == 0 || bin == 4) g[src][0] += v;
        else if (bin == 1 || bin == 5) g[src][1] += v;
        else if (bin == 2) { g[src][0] += v * 0.7071f; g[src][1] += v * 0.7071f; }
    }
    memcpy(b->mix, g, sizeof g);
    if (b->voice) voice_set_gains(b->voice, b->mix);
}

/* (pDS, pDSBufferDesc{dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, ...}, ppBuffer, pUnkOuter) */
static uint32_t ds_create_buffer(const uint32_t *a) {
    if (!a[2]) return E_INVALIDARG;
    HleObj *b;
    uint32_t r = obj_out(a[2], OBJ_DSBUFFER, &b);
    if (!b) return r;
    uint32_t tag = 1, mixbins = 0;
    b->channels = 2;
    b->bits = 16;
    b->rate = 48000;
    if (a[1]) {
        b->bytes = rd32g(a[1], 2);
        uint32_t wfx = rd32g(a[1], 3);
        mixbins = rd32g(a[1], 4);
        if (wfx) {
            const uint8_t *w = G(wfx);
            tag = w[0] | w[1] << 8;
            b->channels = w[2] | w[3] << 8;
            b->rate = rd32g(wfx, 1);
            b->bits = w[14] | w[15] << 8;
        }
        if (b->bytes) b->data = calloc(1, b->bytes);
    }
    if (tag != 1 || b->channels < 1 || b->channels > 2 || (b->bits != 8 && b->bits != 16)) {
        printf("[audio] DirectSound buffer format tag %u, %d channels, %d bits not supported (silent)\n", tag, b->channels, b->bits);
        b->frame_bytes = 0;
    } else b->frame_bytes = (uint32_t)b->channels * b->bits / 8;
    if (b->channels < 1 || b->channels > 2) b->channels = 2;
    xlog("DirectSound buffer %x: tag %u, %d ch, %u Hz, %d bits, %u bytes", GA(b), tag, b->channels, b->rate, b->bits, b->bytes);
    b->voice = voice_new(b->channels, b->rate, dsb_fill, b);
    if (b->voice) {
        if (b->channels == 2) { b->mix[0][0] = 1; b->mix[1][1] = 1; }
        else { b->mix[0][0] = 1; b->mix[0][1] = 1; }
        dsb_set_mixbins(b, mixbins);
    }
    return r;
}
/* The play cursor; a one-shot buffer the mixer finished reads as stopped at 0. */
static uint32_t buf_pos(HleObj *b) {
    if (b->playing && b->voice && !voice_playing(b->voice)) { b->playing = 0; b->cursor = 0; }
    return b->cursor;
}
static uint32_t dsb_play(const uint32_t *a) {   /* (pBuf, dwReserved1, dwReserved2, dwFlags) */
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    if (!b || !b->voice) return S_OK;
    audio_lock();
    b->play_flags = a[3];
    b->playing = 1;
    voice_play(b->voice, 1);
    audio_unlock();
    return S_OK;
}
static uint32_t dsb_stop(const uint32_t *a) {
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    if (!b || !b->voice) return S_OK;
    audio_lock();
    buf_pos(b);
    b->playing = 0;
    voice_play(b->voice, 0);
    audio_unlock();
    return S_OK;
}
static uint32_t dsb_status(const uint32_t *a) {   /* (pBuf, pdwStatus) */
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    uint32_t st = 0;
    if (b) {
        audio_lock();
        buf_pos(b);
        if (b->playing) st = DSBSTATUS_PLAYING | (b->play_flags & DSBPLAY_LOOPING ? DSBSTATUS_LOOPING : 0);
        audio_unlock();
    }
    if (a[1]) *(uint32_t *)G(a[1]) = st;
    return S_OK;
}
static uint32_t dsb_get_pos(const uint32_t *a) {   /* (pBuf, pdwPlay, pdwWrite) */
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    uint32_t p = 0, w = 0;
    if (b) {
        audio_lock();
        p = buf_pos(b);
        w = b->bytes ? (p + 1024) % b->bytes : 0;
        audio_unlock();
    }
    if (a[1]) *(uint32_t *)G(a[1]) = p;
    if (a[2]) *(uint32_t *)G(a[2]) = w;
    return S_OK;
}
static uint32_t dsb_set_pos(const uint32_t *a) {   /* (pBuf, dwPlayCursor) */
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    if (!b) return S_OK;
    audio_lock();
    b->cursor = b->bytes && b->frame_bytes ? a[1] % b->bytes / b->frame_bytes * b->frame_bytes : 0;
    if (b->voice) voice_reset(b->voice);
    audio_unlock();
    return S_OK;
}
/* (pBuf, dwOffset, dwBytes, ppv1, pdw1, ppv2, pdw2, dwFlags) */
static uint32_t dsb_lock(const uint32_t *a) {
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    uint32_t *pv1 = G(a[3]), *pn1 = G(a[4]), *pv2 = G(a[5]), *pn2 = G(a[6]);
    if (pv2) *pv2 = 0;
    if (pn2) *pn2 = 0;
    if (!b || !b->data || !b->bytes || !pv1 || !pn1) return E_INVALIDARG;
    audio_lock();
    uint32_t off = a[7] & DSBLOCK_FROMWRITECURSOR ? (buf_pos(b) + 1024) % b->bytes : a[1] % b->bytes;
    audio_unlock();
    uint32_t len = a[7] & DSBLOCK_ENTIREBUFFER ? b->bytes : (a[2] < b->bytes ? a[2] : b->bytes);
    if (a[7] & DSBLOCK_ENTIREBUFFER) off = 0;
    uint32_t first = off + len > b->bytes ? b->bytes - off : len;
    *pv1 = GA(b->data + off);
    *pn1 = first;
    if (first < len && pv2 && pn2) { *pv2 = GA(b->data); *pn2 = len - first; }
    return S_OK;
}
static uint32_t dsb_set_data(const uint32_t *a) {   /* (pBuf, pvBufferData, dwBufferBytes): caller-owned memory */
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    if (!b) return S_OK;
    audio_lock();
    b->data = G(a[1]);
    b->bytes = a[1] ? a[2] : 0;
    b->cursor = 0;
    if (b->voice) voice_reset(b->voice);
    audio_unlock();
    return S_OK;
}
static uint32_t dsb_set_volume(const uint32_t *a) {   /* (pBuf, lVolume in hundredths of a dB) */
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    if (b && b->voice) { b->volume = (int32_t)a[1]; voice_set_volume(b->voice, audio_db100_to_gain(b->volume)); }
    return S_OK;
}
static uint32_t dsb_set_frequency(const uint32_t *a) {   /* (pBuf, dwFrequency; 0 = the format's rate) */
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    if (b && b->voice) voice_set_rate(b->voice, a[1] ? a[1] : b->rate);
    return S_OK;
}
static uint32_t dsb_set_mixbins_entry(const uint32_t *a) {   /* (pBuf, pMixBins) */
    HleObj *b = obj_get(a[0], OBJ_DSBUFFER);
    if (b && b->voice) dsb_set_mixbins(b, a[1]);
    return S_OK;
}

typedef uint32_t (*HleFn)(const uint32_t *a);
typedef struct {
    const char *name;   /* symbol suffix after LIB__ when XbSymbolDatabase knows it */
    const char *lib;
    uint32_t addr;
    int argbytes;       /* from the wrapper's own "ret N" (0 = plain ret, no arguments) */
    HleFn fn;
    uint8_t sig[HLE_SIG_LEN];
    uint32_t calls;
} HleEntry;

#define X(name, addr, n, fn, ...) {name, "XACTENG", addr, n, fn, {__VA_ARGS__}, 0}
#define D(name, addr, n, fn, ...) {name, "DSOUND", addr, n, fn, {__VA_ARGS__}, 0}
static HleEntry entries[] = {
    X("XACTEngineCreate", 0x3d01ec, 8, xact_engine_create, 0x53, 0x56, 0x33, 0xf6, 0xe8),
    X("XACTEngineDoWork", 0x3d01ba, 0, xact_do_work, 0x56, 0x57, 0xe8, 0x88, 0xd8),
    X("XACTSetFileIoCallbacks?", 0x3cf9a0, 4, ok, 0xe8, 0xa4, 0xe0, 0xff, 0xff),
    X("IXACTEngine_CreateSoundSource", 0x3cdb32, 12, xact_create_source, 0x56, 0x57, 0xe8, 0x10, 0xff),
    X("IXACTEngine_CreateSoundBank", 0x3cdb71, 16, xact_create_bank, 0x56, 0x57, 0xe8, 0xd1, 0xfe),
    X("IXACTEngine_RegisterWaveBank", 0x3cdbb4, 16, xact_register_wb, 0x56, 0x57, 0xe8, 0x8e, 0xfe),
    X("IXACTEngine_RegisterStreamedWaveBank", 0x3cdbf7, 12, xact_register_swb, 0x56, 0x57, 0xe8, 0x4b, 0xfe),
    X("IXACTEngine_UnRegisterWaveBank", 0x3cdc36, 8, xact_unregister_wb, 0x56, 0x57, 0xe8, 0x0c, 0xfe),
    X("IXACTEngine_SetCategoryVolume?", 0x3cdc71, 12, ok, 0x56, 0x57, 0xe8, 0xd1, 0xfd),
    X("IXACTEngine_SetI3DL2Listener?", 0x3cdcb0, 12, ok, 0x8b, 0x44, 0x24, 0x04, 0xff),
    X("IXACTEngine_SetListenerOrientation?", 0x3cdcd0, 32, xact_listener_orient, 0x55, 0x8b, 0xec, 0x56, 0x57),
    X("IXACTEngine_SetListenerPosition?", 0x3cdd39, 20, xact_listener_pos, 0x55, 0x8b, 0xec, 0x56, 0x57),
    X("IXACTEngine_SetListenerVelocity?", 0x3cdd8d, 20, ok, 0x55, 0x8b, 0xec, 0xff, 0x75),
    X("IXACTEngine_SetCategoryParam?", 0x3cddde, 12, ok, 0x56, 0x57, 0xe8, 0x64, 0xfc),
    X("IXACTEngine_RegisterNotification", 0x3cde1e, 8, xact_register_notification, 0x56, 0x57, 0xe8, 0x24, 0xfc),
    X("IXACTEngine_UnRegisterNotification?", 0x3cde59, 8, xact_unregister_notification, 0x56, 0x57, 0xe8, 0xe9, 0xfb),
    X("IXACTEngine_GetNotification", 0x3cde94, 12, xact_get_notification, 0x56, 0x57, 0xe8, 0xae, 0xfb),
    X("IXACTEngine_FlushNotification?", 0x3cded3, 8, xact_flush_notification, 0x56, 0x57, 0xe8, 0x6f, 0xfb),
    X("IXACTEngine_GetOutputLevels?", 0x3cdf90, 8, xact_get_levels, 0x56, 0x57, 0xe8, 0xb2, 0xfa),
    X("IXACTEngine_CommitDeferredSettings?", 0x3ce00b, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x8b),
    X("IXACTEngine_EnableHeadphones?", 0x3ce023, 8, ok, 0x8b, 0x44, 0x24, 0x04, 0xff),
    X("IXACTEngine_SetParameterControl?", 0x3ce0ab, 8, xact_set_control, 0x56, 0x57, 0xe8, 0x97, 0xf9),
    X("IXACTSoundBank_Prepare?", 0x3ce171, 12, xact_prepare, 0x56, 0x57, 0xe8, 0xd1, 0xf8),
    X("IXACTSoundBank_PlayEx", 0x3ce1a4, 12, xact_play, 0x56, 0x57, 0xe8, 0x9e, 0xf8),
    X("IXACTSoundBank_Stop?", 0x3ce1d7, 16, xact_stop, 0x56, 0x57, 0xe8, 0x6b, 0xf8),
    X("IXACTSoundBank_GetSoundCueProperties?", 0x3ce278, 12, xact_cue_props, 0x56, 0x57, 0xe8, 0xca, 0xf7),
    X("IXACTSoundSource_SetPosition?", 0x3ce323, 20, xact_source_pos, 0x55, 0x8b, 0xec, 0xff, 0x75),
    X("IXACTSoundSource_SetOrientation?", 0x3ce34c, 20, ok, 0x55, 0x8b, 0xec, 0xff, 0x75),
    X("IXACTSoundSource_SetVelocity?", 0x3ce37a, 20, ok, 0x55, 0x8b, 0xec, 0xff, 0x75),
    X("IXACTSoundSource_SetMixBinVolumes?", 0x3ce3a3, 8, ok, 0xe9, 0xf1, 0x48, 0x00, 0x00),
    X("IXACTSoundSource_Stop?", 0x3ce3b7, 4, xact_source_stop, 0xe9, 0x11, 0x47, 0x00, 0x00),
    X("IXACTSoundSource_GetStatus?", 0x3ce3c1, 8, xact_source_status, 0xe9, 0x3c, 0x47, 0x00, 0x00),
    D("DirectSoundUseFullHRTF", 0x3a7a66, 0, ok, 0x56, 0xe8, 0x04, 0xfe, 0xff),
    D("XAudioDownloadEffectsImage", 0x3aa13f, 16, xaudio_effects_image, 0x55, 0x8b, 0xec, 0x83, 0xec),
    D("DirectSoundCreate", 0x3ad4eb, 12, ds_create, 0x55, 0x8b, 0xec, 0x51, 0x56),
    D("IDirectSound_Release", 0x3a79de, 4, ds_release, 0x8b, 0x44, 0x24, 0x04, 0x8d),
    D("IDirectSound_CreateSoundBuffer", 0x3ad19d, 16, ds_create_buffer, 0xff, 0x74, 0x24, 0x10, 0x8b),
    D("IDirectSoundBuffer_Release", 0x3a7a14, 4, ds_release, 0x8b, 0x44, 0x24, 0x04, 0x8d),
    D("IDirectSoundBuffer_Unlock", 0x3a79f4, 20, ok, 0x33, 0xc0, 0xc2, 0x14, 0x00),
    D("IDirectSoundBuffer_SetVolume", 0x3a9ddc, 8, dsb_set_volume, 0x8b, 0x44, 0x24, 0x04, 0xff),
    D("IDirectSoundBuffer_SetHeadroom", 0x3a9e68, 8, ok, 0x8b, 0x44, 0x24, 0x04, 0xff),
    D("IDirectSoundBuffer_SetMixBinVolumes_8", 0x3a9e84, 8, dsb_set_mixbins_entry, 0x8b, 0x44, 0x24, 0x04, 0xff),
    D("IDirectSoundBuffer_Play", 0x3a9ea0, 16, dsb_play, 0xff, 0x74, 0x24, 0x10, 0x8b),
    D("IDirectSoundBuffer_Stop", 0x3a9ee8, 4, dsb_stop, 0x8b, 0x44, 0x24, 0x04, 0x8b),
    D("IDirectSoundBuffer_GetStatus", 0x3a9f84, 8, dsb_status, 0x8b, 0x44, 0x24, 0x04, 0xff),
    D("IDirectSoundBuffer_GetCurrentPosition", 0x3a9fa0, 12, dsb_get_pos, 0x8b, 0x44, 0x24, 0x04, 0xff),
    D("IDirectSoundBuffer_SetCurrentPosition", 0x3a9fc0, 8, dsb_set_pos, 0x8b, 0x44, 0x24, 0x04, 0xff),
    D("IDirectSoundBuffer_Lock", 0x3a9fdc, 32, dsb_lock, 0x55, 0x8b, 0xec, 0xff, 0x75),
    D("IDirectSoundBuffer_SetFrequency", 0x3abca2, 8, dsb_set_frequency, 0x8b, 0x44, 0x24, 0x04, 0xff),
    D("IDirectSoundBuffer_SetMixBins", 0x3abcda, 8, dsb_set_mixbins_entry, 0x8b, 0x44, 0x24, 0x04, 0xff),
    D("IDirectSoundBuffer_SetBufferData", 0x3ac6ba, 12, dsb_set_data, 0x8b, 0x44, 0x24, 0x04, 0xff),
};
#define NENTRIES ((int)(sizeof entries / sizeof *entries))

int hle_audio_entry_count(void) { return NENTRIES; }
uint32_t hle_audio_entry_addr(int i) { return entries[i].addr; }
int hle_audio_entry_argbytes(int i) { return entries[i].argbytes; }
const char *hle_audio_entry_name(int i) { return entries[i].name; }
const uint8_t *hle_audio_entry_sig(int i) { return entries[i].sig; }

void hle_audio_dispatch(HleFrame *f) {
    HleEntry *e = &entries[f->index];
    uint32_t r = e->fn(f->args);
    f->eax = r;
    uint32_t n = __atomic_add_fetch(&e->calls, 1, __ATOMIC_SEQ_CST);
    if ((int)n > hle_audio_log_limit) return;
    char buf[160];
    int len = 0;
    for (int i = 0; i < e->argbytes / 4 && len < (int)sizeof buf - 12; i++)
        len += snprintf(buf + len, sizeof buf - len, "%s%x", i ? ", " : "", f->args[i]);
    buf[len] = 0;
    pthread_mutex_lock(&hle_lock);
    printf("[hle] %s(%s) -> %x [from %08x]%s\n", e->name, buf, r, f->ret_addr,
           (int)n == hle_audio_log_limit ? " (further calls not logged)" : "");
    fflush(stdout);
    pthread_mutex_unlock(&hle_lock);
}

extern void hle_entry_common(void);

int hle_audio_install(HleLookup lookup) {
    uint8_t *stubs = plat_alloc_exec(NENTRIES * 16);
    if (!stubs) return 0;
    int patched = 0;
    for (int i = 0; i < NENTRIES; i++) {
        HleEntry *e = &entries[i];
        uint8_t *p = G(e->addr);
        char sym[96];
        snprintf(sym, sizeof sym, "%s__%s", e->lib, e->name);
        uint32_t s = lookup && !strchr(e->name, '?') ? lookup(sym) : 0;
        if (s && s != e->addr) {
            printf("[hle] skip %s: symbol at %08x, table expects %08x\n", e->name, s, e->addr);
            continue;
        }
        if (memcmp(p, e->sig, HLE_SIG_LEN)) {
            printf("[hle] skip %s at %08x: code does not match the expected entry bytes\n", e->name, e->addr);
            continue;
        }
        /* Stub: push i; call hle_entry_common; add esp,4; ret argbytes */
        uint8_t *st = stubs + i * 16;
        st[0] = 0x68; memcpy(st + 1, &i, 4);
        int32_t rel = (int32_t)((uintptr_t)hle_entry_common - (uintptr_t)(st + 10));
        st[5] = 0xE8; memcpy(st + 6, &rel, 4);
        st[10] = 0x83; st[11] = 0xC4; st[12] = 0x04;
        st[13] = 0xC2; st[14] = e->argbytes & 0xFF; st[15] = e->argbytes >> 8;
        rel = (int32_t)((uintptr_t)st - (uintptr_t)(p + 5));
        p[0] = 0xE9; memcpy(p + 1, &rel, 4);
        patched++;
    }
    printf("[hle] audio: %d of %d XACT/DirectSound entry points replaced\n", patched, NENTRIES);
    return patched;
}

void hle_audio_summary(void) {
    uint32_t total = 0;
    char buf[2048];
    int len = 0;
    for (int i = 0; i < NENTRIES; i++) {
        uint32_t c = __atomic_load_n(&entries[i].calls, __ATOMIC_SEQ_CST);
        total += c;
        if (c && len < (int)sizeof buf - 80) len += snprintf(buf + len, sizeof buf - len, " %s=%u", entries[i].name, c);
    }
    buf[len] = 0;
    printf("[hle] audio calls: %u total;%s\n", total, buf);
    printf("[hle] xact controls: %s, %u set, %u unmatched, %u global\n", xact_controls ? "on" : "off", ctl_sets, ctl_unmatched,
           ctl_global);
}
