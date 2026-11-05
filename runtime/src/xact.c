/* XACT 1 wave bank / sound bank parsing and the wave reader (see xact.h). */
#define _GNU_SOURCE
#include "xact.h"
#include "audio.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include <unistd.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int fail(char *err, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (err && n) vsnprintf(err, n, fmt, ap);
    va_end(ap);
    return -1;
}

/* ---- wave banks ----
 * Header (version 2..3, "XACT 1.1"): "WBND", version, then 4 segments {offset, size}: bank
 * data, entry metadata, (unused), wave data. Bank data: flags, entry count, name[16], entry
 * size (0x18), meta name size, alignment, compact format. Entry: flags, format, offset,
 * length, loop start, loop length. Format: bits 0-1 codec, 2-4 channels, 5-30 sample rate,
 * 31 = 16-bit PCM. */
int xwb_parse(const uint8_t *buf, size_t len, XwbBank *wb, size_t *needed, char *err, size_t errlen) {
    memset(wb, 0, sizeof *wb);
    wb->fd = -1;
    if (needed) *needed = 0x50;
    if (len < 0x28 || memcmp(buf, "WBND", 4)) return fail(err, errlen, "not a wave bank (no WBND)");
    uint32_t ver = rd32(buf + 4);
    if (ver < 2 || ver > 3) return fail(err, errlen, "unsupported wave bank version %u", ver);
    uint32_t bank_off = rd32(buf + 8), entry_off = rd32(buf + 0x10), entry_size = rd32(buf + 0x14);
    if (bank_off < 0x28 || bank_off > 0x1000 || entry_off > 0x100000 || entry_size > 0x1000000)
        return fail(err, errlen, "bad wave bank segments");
    size_t need = entry_off + entry_size > bank_off + 0x28 ? entry_off + entry_size : bank_off + 0x28;
    if (needed) *needed = need;
    if (len < need) return fail(err, errlen, "wave bank header truncated (%zu of %zu bytes)", len, need);
    const uint8_t *bd = buf + bank_off;
    wb->flags = rd32(bd);
    uint32_t count = rd32(bd + 4), esz = rd32(bd + 0x18);
    memcpy(wb->name, bd + 8, 16);
    wb->name[16] = 0;
    if (esz < 0x18 || (uint64_t)count * esz > entry_size) return fail(err, errlen, "bad wave bank entry table");
    wb->entries = calloc(count ? count : 1, sizeof *wb->entries);
    if (!wb->entries) return fail(err, errlen, "out of memory");
    wb->count = (int)count;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = buf + entry_off + i * esz;
        uint32_t fmt = rd32(e + 4);
        XwbEntry *x = &wb->entries[i];
        x->codec = fmt & 3;
        x->channels = (fmt >> 2) & 7;
        x->rate = (fmt >> 5) & 0x3FFFFFF;
        x->bits = fmt >> 31 ? 16 : 8;
        x->offset = rd32(e + 8);
        x->length = rd32(e + 12);
        x->loop_start = rd32(e + 16);
        x->loop_length = rd32(e + 20);
        if (x->channels < 1 || x->channels > 2) x->codec = -1;   /* unsupported: plays as silence */
    }
    return 0;
}

void xwb_free(XwbBank *wb) {
    free(wb->entries);
    wb->entries = NULL;
    wb->count = 0;
}

int xwb_open_stream(int fd, uint64_t off, XwbBank *wb, char *err, size_t errlen) {
    size_t len = 0x50, need = 0;
    for (int tries = 0; tries < 2; tries++) {
        uint8_t *buf = malloc(len);
        if (!buf) return fail(err, errlen, "out of memory");
        ssize_t got = plat_pread(fd, buf, len, off);
        int r = got > 0 ? xwb_parse(buf, (size_t)got, wb, &need, err, errlen) : fail(err, errlen, "cannot read the wave bank");
        if (!r) {
            wb->fd = fd;
            wb->data_offset = off + rd32(buf + 0x20);
            free(buf);
            return 0;
        }
        free(buf);
        if (need <= len || (size_t)got < len) return -1;
        len = need;
    }
    return -1;
}

uint32_t xwb_entry_frames(const XwbEntry *e) {
    if (e->codec == XWB_XADPCM) return e->length / (36u * e->channels) * XADPCM_BLOCK_SAMPLES;
    if (e->codec == XWB_PCM) return e->length / ((uint32_t)e->channels * (e->bits / 8));
    return 0;
}

uint32_t xwb_entry_ms(const XwbEntry *e) {
    uint64_t v = e->length;
    if (e->codec == XWB_XADPCM) v = v / 36 * 64;
    else if (e->bits == 16) v /= 2;
    if (!e->channels || !e->rate) return 0;
    return (uint32_t)(v / (uint64_t)e->channels * 1000 / e->rate);
}

/* ---- sound banks ----
 * Header (version 11, "XACT 1.2"): 0x08 wave bank name table offset, 0x10 3D parameter table
 * offset, 0x1c entry count, 0x1e cue count, 0x22 wave bank count, 0x28 name; the cue index
 * table (0x14 bytes per cue) starts at 0x38 and the sound entry table (0x14 bytes) follows it.
 * Cue index: +2 s16 sound entry (negative: +8 is the offset of a random table).
 * Sound entry: +0 offset (tracks, variation table) or wave/bank, +4 bits 0-8 volume
 * (attenuation in 0.16 dB steps), +8 track count, +9 category, +0xb flags (0x08: +0 is
 * wave/bank, 0x10: +0 is a variation table). Track table: one dword per track, count in bits
 * 0-7, event list offset in bits 8-31. Event: +0 type (0/1 = play wave), +4 extra size,
 * +5 flags (0x04: +8 is a variation table), +6 loop count (0xffff = forever), +8 wave,
 * +0xa wave bank. Variation table: dword (count bits 0-12, current bits 17-29), then
 * 8-byte entries starting with the wave and the bank (u16 each). */
int xsb_parse(const uint8_t *d, size_t len, XsbBank *sb, char *err, size_t errlen) {
    memset(sb, 0, sizeof *sb);
    if (len < 0x38 || memcmp(d, "SDBK", 4)) return fail(err, errlen, "not a sound bank (no SDBK)");
    uint16_t ver = rd16(d + 4);
    if (ver < 9 || ver > 11) return fail(err, errlen, "unsupported sound bank version %u", ver);
    sb->d = d;
    sb->len = len;
    sb->entries = rd16(d + 0x1c);
    sb->cues = rd16(d + 0x1e);
    sb->banks = rd16(d + 0x22);
    sb->bank_names = rd32(d + 8);
    if (0x38 + ((size_t)sb->cues + sb->entries) * 0x14 > len || sb->bank_names + (size_t)sb->banks * 16 > len)
        return fail(err, errlen, "sound bank tables out of range");
    /* Parameter controls (version 11 layout); a table out of range leaves the bank without. */
    uint32_t ct = rd32(d + 0x14), nc = rd16(d + 0x20);
    if (ver == 11 && nc && ct >= 0x38 && ct + (size_t)nc * 0x18 <= len) {
        sb->controls = ct;
        sb->ncontrols = (int)nc;
        sb->controls_sorted = 1;
        for (int i = 1; i < sb->ncontrols && sb->controls_sorted; i++)
            sb->controls_sorted = rd16(d + ct + (size_t)(i - 1) * 0x18) <= rd16(d + ct + (size_t)i * 0x18);
    }
    return 0;
}

/* ---- parameter controls (layout in xact.h) ---- */
static uint16_t ctl_sound(const XsbBank *sb, int i) { return rd16(sb->d + sb->controls + (size_t)i * 0x18); }

/* The XBE scans linearly for the first entry of the sound (0x3d7542) and counts the entries
 * that follow (0x3d757c); a sorted table gives the same range by binary search. */
int xsb_sound_controls(const XsbBank *sb, int entry, int *first) {
    *first = 0;
    if (!sb->ncontrols || entry < 0 || entry > 0xFFFF) return 0;
    int lo = 0, hi = sb->ncontrols;
    if (sb->controls_sorted) {
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (ctl_sound(sb, mid) < entry) lo = mid + 1; else hi = mid;
        }
    } else {
        while (lo < sb->ncontrols && ctl_sound(sb, lo) != entry) lo++;
    }
    int n = 0;
    while (lo + n < sb->ncontrols && ctl_sound(sb, lo + n) == entry) n++;
    if (n) *first = lo;
    return n;
}

int xsb_control(const XsbBank *sb, int i, XsbControl *out) {
    memset(out, 0, sizeof *out);
    if (i < 0 || i >= sb->ncontrols) return -1;
    const uint8_t *e = sb->d + sb->controls + (size_t)i * 0x18;
    memcpy(out->name, e + 8, 16);
    out->ncurves = e[3];
    out->curves = rd32(e + 4);
    return 0;
}

int xsb_curve(const XsbBank *sb, const XsbControl *c, int k, XsbCurve *out) {
    memset(out, 0, sizeof *out);
    if (k < 0 || k >= c->ncurves || (size_t)c->curves + (k + 1) * 0x18u > sb->len) return -1;
    const uint8_t *p = sb->d + c->curves + k * 0x18;
    out->track = rd16(p);
    out->target = rd16(p + 2);
    for (int i = 0; i < 4; i++) {
        out->x[i] = rd16(p + 4 + 4 * i);
        out->y[i] = (int16_t)rd16(p + 6 + 4 * i);
    }
    return 0;
}

int32_t xsb_curve_eval(const XsbCurve *c, float value) {
    float fx = value * 65536.0f;
    /* CVTTSS2SI: truncation; NaN and out-of-range give the integer indefinite 0x80000000. */
    int32_t x = fx > -2147483648.0f && fx < 2147483648.0f ? (int32_t)fx : INT32_MIN;
    if (x <= c->x[0]) return c->y[0];
    if (x >= c->x[3]) return c->y[3];
    for (int k = 1; k < 4; k++)
        if (x < c->x[k]) {   /* 64-bit product (the XBE's is 32-bit; this title's curves stay far from overflow) */
            int64_t dy = c->y[k] - c->y[k - 1], dx = x - c->x[k - 1], w = c->x[k] - c->x[k - 1];
            return (int16_t)(dy * dx / w + c->y[k - 1]);
        }
    return c->y[3];
}

const char *xsb_bank_name(const XsbBank *sb, int i, char out[17]) {
    out[0] = 0;
    if (i >= 0 && i < sb->banks) { memcpy(out, sb->d + sb->bank_names + i * 16, 16); out[16] = 0; }
    return out;
}

static uint32_t lcg(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s >> 16; }

/* Variation table at off: picks one entry. Returns 0 and fills wave/bank. */
static int variation(const XsbBank *sb, uint32_t off, uint32_t *rng, int *wave, int *bank) {
    if (off + 4 > sb->len) return -1;
    uint32_t n = rd32(sb->d + off) & 0x1FFF;
    if (!n || off + 4 + n * 8 > sb->len) return -1;
    const uint8_t *v = sb->d + off + 4 + (lcg(rng) % n) * 8;   /* entry: wave, bank, weights */
    *wave = rd16(v);
    *bank = rd16(v + 2);
    return 0;
}

int xsb_cue_sound(const XsbBank *sb, uint32_t cue, uint32_t *rng, XsbSound *out) {
    memset(out, 0, sizeof *out);
    out->category = 0xff;
    if (cue >= (uint32_t)sb->cues) return -1;
    const uint8_t *ix = sb->d + 0x38 + cue * 0x14;
    int entry = (int16_t)rd16(ix + 2);
    if (entry < 0) {   /* random table: count byte, 8-byte entries with the entry index first */
        uint32_t t = rd32(ix + 8);
        if (t + 4 > sb->len) return -1;
        uint32_t n = sb->d[t];
        if (!n || t + 4 + n * 8 > sb->len) return -1;
        entry = (int16_t)rd16(sb->d + t + 4 + (lcg(rng) % n) * 8);
    }
    if (entry < 0 || entry >= sb->entries) return -1;
    out->entry = entry;
    const uint8_t *e = sb->d + 0x38 + ((size_t)sb->cues + entry) * 0x14;
    uint8_t flags = e[0xb];
    out->volume = -(int32_t)(rd16(e + 4) & 0x1FF) * 16;
    out->category = e[9];
    /* 3D record (table at header +0x10, index at entry +0xc, as the XBE's 0x3d12a7 reads it):
     * u16 inside/outside cone angles, s16 cone outside volume, then floats: min distance, max
     * distance and three more that are 1.0 in every record of this title's bank (unused here). */
    size_t t3d = (size_t)rd32(sb->d + 0x10) + rd16(e + 0xc) * 0x28u;
    if ((flags & 0x01) && t3d + 0x28 <= sb->len) {
        uint32_t mn = rd32(sb->d + t3d + 8), mx = rd32(sb->d + t3d + 12);
        out->is3d = 1;
        out->rec3d_index = rd16(e + 0xc);
        memcpy(out->rec3d, sb->d + t3d, sizeof out->rec3d);
        memcpy(&out->min_dist, &mn, 4);
        memcpy(&out->max_dist, &mx, 4);
    }
    if (flags & 0x08) {
        out->w[0] = (XsbWave){rd16(e + 2), rd16(e), 0};
        out->nwaves = 1;
        return 0;
    }
    if (flags & 0x10) {
        int w, b;
        if (variation(sb, rd32(e), rng, &w, &b)) return -1;
        out->w[0] = (XsbWave){b, w, 0};
        out->nwaves = 1;
        return 0;
    }
    uint32_t tracks = rd32(e), ntracks = e[8];
    if (tracks + ntracks * 4 > sb->len) return -1;
    for (uint32_t t = 0; t < ntracks; t++) {
        uint32_t td = rd32(sb->d + tracks + t * 4), nev = td & 0xFF, p = td >> 8;
        for (uint32_t k = 0; k < nev && p + 8 <= sb->len; k++) {
            const uint8_t *ev = sb->d + p;
            uint32_t size = 8u + ev[4];
            if ((ev[0] == 0 || ev[0] == 1) && p + 12 <= sb->len && out->nwaves < XSB_MAX_WAVES) {
                int w = rd16(ev + 8), b = rd16(ev + 10);
                if (!(ev[5] & 0x04) || !variation(sb, rd32(ev + 8), rng, &w, &b)) {
                    uint16_t lc = rd16(ev + 6);
                    out->w[out->nwaves++] = (XsbWave){b, w, lc == 0xFFFF ? -1 : lc};
                }
            }
            p += size;
        }
    }
    return 0;
}

/* ---- wave reader ---- */
void wave_reader_init(WaveReader *r, const XwbBank *wb, const XwbEntry *e, int loops) {
    memset(r, 0, sizeof *r);
    r->wb = wb;
    r->e = e;
    r->loops = loops;
}

/* Returns a pointer to n bytes of the entry at byte position pos (n <= sizeof io), or NULL. */
static const uint8_t *wave_bytes(WaveReader *r, uint32_t pos, uint32_t n) {
    const XwbEntry *e = r->e;
    if (pos + n > e->length) return NULL;
    if (r->wb->mem) return r->wb->mem + e->offset + pos;
    if (pos >= r->io_pos && pos + n <= r->io_pos + r->io_len) return r->io + (pos - r->io_pos);
    uint32_t want = sizeof r->io;
    if (pos + want > e->length) want = e->length - pos;
    ssize_t got = plat_pread(r->wb->fd, r->io, want, (uint64_t)r->wb->data_offset + e->offset + pos);
    if (got < (ssize_t)n) return NULL;
    r->io_pos = pos;
    r->io_len = (uint32_t)got;
    return r->io;
}

/* Restarts at the loop region when the entry is exhausted and repeats remain. */
static int wave_wrap(WaveReader *r, uint32_t block) {
    if (!r->loops) return 0;
    if (r->loops > 0) r->loops--;
    uint32_t start = r->e->loop_length ? r->e->loop_start : 0;
    r->pos = start / block * block;
    return 1;
}

int wave_reader_fill(void *ctx, int16_t *dst, int max) {
    WaveReader *r = ctx;
    const XwbEntry *e = r->e;
    int ch = e->channels, done = 0;
    if (e->codec == XWB_XADPCM) {
        uint32_t block = 36u * ch;
        while (done < max) {
            if (r->blk_i < r->blk_n) {   /* serve the rest of the decoded block first */
                int n = r->blk_n - r->blk_i < max - done ? r->blk_n - r->blk_i : max - done;
                memcpy(dst + done * ch, r->blk + r->blk_i * ch, (size_t)n * ch * sizeof(int16_t));
                r->blk_i += n;
                done += n;
                continue;
            }
            uint32_t end = (e->loop_length && r->loops ? e->loop_start + e->loop_length : e->length) / block * block;
            if (r->pos >= end && !wave_wrap(r, block)) break;
            const uint8_t *b = wave_bytes(r, r->pos, block);
            if (!b) break;   /* truncated data or a read error ends the wave */
            xadpcm_decode_block(b, ch, r->blk);
            r->pos += block;
            r->blk_n = XADPCM_BLOCK_SAMPLES;
            r->blk_i = 0;
        }
        return done;
    }
    if (e->codec != XWB_PCM) return 0;
    uint32_t fb = (uint32_t)ch * (e->bits / 8);
    while (done < max) {
        uint32_t end = (e->loop_length && r->loops ? e->loop_start + e->loop_length : e->length) / fb * fb;
        if (r->pos >= end && !wave_wrap(r, fb)) break;
        uint32_t n = (end - r->pos) / fb, room = (uint32_t)(max - done);
        if (n > room) n = room;
        if (n * fb > sizeof r->io) n = sizeof r->io / fb;
        const uint8_t *b = wave_bytes(r, r->pos, n * fb);
        if (!n || !b) break;
        for (uint32_t i = 0; i < n * ch; i++)
            dst[done * ch + i] = e->bits == 16 ? (int16_t)rd16(b + i * 2) : (int16_t)((b[i] - 128) << 8);
        r->pos += n * fb;
        done += (int)n;
    }
    return done;
}

/* Wave and bank of a variation table's current entry (the XBE reads the current index, bits
 * 17-29, not a random pick). Returns 0, or -1 when the index is past the count. */
static int variation_current(const XsbBank *sb, uint32_t off, int *wave, int *bank) {
    if (off + 4 > sb->len) return -1;
    uint32_t d = rd32(sb->d + off), cur = d >> 17 & 0x1FFF, n = d & 0x1FFF;
    if (cur >= n || off + 4 + (cur + 1) * 8 > sb->len) return -1;
    *wave = rd16(sb->d + off + 4 + cur * 8);
    *bank = rd16(sb->d + off + 6 + cur * 8);
    return 0;
}

int xsb_cue_props(const XsbBank *sb, uint32_t cue, XsbWaveMs ms, void *ctx, uint32_t out[25]) {
    memset(out, 0, 25 * 4);
    if (cue >= (uint32_t)sb->cues) return -1;
    uint32_t entry = rd16(sb->d + 0x38 + cue * 0x14 + 2);   /* unsigned, as the XBE reads it */
    if (entry >= (uint32_t)sb->entries) return -1;
    const uint8_t *e = sb->d + 0x38 + ((size_t)sb->cues + entry) * 0x14;
    uint8_t flags = e[0xb];
    if (flags & 0x01) {   /* 3D: the DS3D parameter record */
        out[0] = 1;
        size_t t = (size_t)rd32(sb->d + 0x10) + rd16(e + 0xc) * 0x28u;
        if (t + 0x28 <= sb->len) {
            const uint8_t *r = sb->d + t;
            out[0x40 / 4] = rd16(r);
            out[0x44 / 4] = rd16(r + 2);
            out[0x48 / 4] = (uint32_t)(int16_t)rd16(r + 4);
            out[0x4c / 4] = r[0x1c];
            for (int k = 0; k < 5; k++) out[0x50 / 4 + k] = rd32(r + 8 + 4 * k);
        }
    }
    uint16_t w4 = rd16(e + 4), w12 = rd16(e + 0x12);
    out[0x04 / 4] = e[0xe];
    out[0x28 / 4] = e[8];
    out[0x2c / 4] = entry;
    out[0x0c / 4] = (uint32_t)(int16_t)rd16(e + 6);
    out[0x10 / 4] = e[9];
    out[0x14 / 4] = e[0xa];
    out[0x18 / 4] = (uint32_t)(int16_t)rd16(e + 0x10);
    out[0x1c / 4] = w12 & 7;
    out[0x20 / 4] = w12 >> 3;
    out[0x08 / 4] = (uint32_t)(-(int32_t)(w4 & 0x1FF) * 16);
    out[0x3c / 4] = (uint32_t)((int32_t)(w4 >> 9 & 0x7F) * -50);
    out[0x38 / 4] = (uint32_t)(-(int32_t)e[0xf] * 256);
    out[0x30 / 4] = 0xFFFF;
    if (flags & 0x18) {   /* simple entry or variation table: one wave */
        int wave = 0xFFFF, bank = 0;
        if (flags & 0x10) {
            if (variation_current(sb, rd32(e), &wave, &bank)) return 0;
        } else {
            wave = rd16(e);
            bank = rd16(e + 2);
        }
        out[0x30 / 4] = (uint32_t)wave;
        if (wave == 0xFFFF) return 0;
        uint32_t d = ms ? ms(ctx, bank, wave) : XSB_NO_MS;
        if (d != XSB_NO_MS && d > out[0x34 / 4]) out[0x34 / 4] = d;
        return 0;
    }
    int same = 1;
    uint32_t tracks = rd32(e), ntracks = e[8];
    for (uint32_t t = 0; t < ntracks && tracks + (t + 1) * 4 <= sb->len; t++) {
        uint32_t td = rd32(sb->d + tracks + t * 4), nev = td & 0xFF, p = td >> 8;
        for (uint32_t k = 0; k < nev && p + 12 <= sb->len; k++) {   /* the first play-wave event */
            const uint8_t *ev = sb->d + p;
            if (ev[0] != 0 && ev[0] != 1) { p += 8u + ev[4]; continue; }
            uint32_t loops = rd16(ev + 6);
            if (loops > out[0x24 / 4]) out[0x24 / 4] = loops;
            int wave = 0xFFFF, bank = rd16(ev + 10);
            if (!(ev[5] & 0x04)) wave = rd16(ev + 8);
            else if (variation_current(sb, rd32(ev + 8), &wave, &bank)) wave = 0xFFFF;
            if (out[0x30 / 4] == 0xFFFF) out[0x30 / 4] = (uint32_t)wave;
            else if (out[0x30 / 4] != (uint32_t)wave) same = 0;
            if (wave != 0xFFFF) {
                uint32_t d = ms ? ms(ctx, bank, wave) : XSB_NO_MS;
                if (d != XSB_NO_MS && d > out[0x34 / 4]) out[0x34 / 4] = d;
            }
            break;
        }
    }
    if (!same) out[0x30 / 4] = 0xFFFF;
    return 0;
}
