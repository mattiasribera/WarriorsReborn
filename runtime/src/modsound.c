/* A mod's own sounds (src/modsound.h). */
#include "modsound.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char id[41], name[64];
    int16_t *pcm;
    int channels, rate, frames;
} Snd;

typedef struct {
    int handle;   /* 0: free slot */
    const Snd *s;
    void *voice;
    int pos, loop, is3d;
    float at[3];   /* world */
    float volume;
} Play;

#define MAX_SOUNDS 256
#define MAX_PLAYS 128
static Snd snd[MAX_SOUNDS];
static int nsnd;
static Play plays[MAX_PLAYS];
static int next_handle = 1;
static ModSoundAudio au;
static int have_audio;

void modsound_setup(const ModSoundAudio *a) {
    if (a) au = *a;
    have_audio = a && a->voice_new;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static size_t mod_bytes(const char *id) {
    size_t n = 0;
    for (int i = 0; i < nsnd; i++)
        if (!strcmp(snd[i].id, id)) n += (size_t)snd[i].frames * snd[i].channels * 2;
    return n;
}

/* The engine's own WAV (ma_wav_write: 44-byte header, 16-bit PCM). */
static int load_wav(const char *path, Snd *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[44];
    int ok = fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4) && h[20] == 1 && h[34] == 16;
    const int ch = h[22];
    const uint32_t rate = le32(h + 24), bytes = le32(h + 40);
    ok = ok && (ch == 1 || ch == 2) && rate >= 8000 && rate <= 96000 && bytes >= (uint32_t)ch * 2 && bytes <= MODSOUND_BYTES_MAX;
    if (ok) {
        s->pcm = malloc(bytes);
        ok = s->pcm && fread(s->pcm, 1, bytes, f) == bytes;
        if (!ok) free(s->pcm), s->pcm = NULL;
    }
    fclose(f);
    if (!ok) return -1;
    s->channels = ch, s->rate = (int)rate, s->frames = (int)(bytes / ((uint32_t)ch * 2));
    return 0;
}

int modsound_add_dir(const char *id, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        const size_t L = strlen(e->d_name);
        if (L < 5 || L - 4 >= sizeof snd[0].name || strcmp(e->d_name + L - 4, ".wav")) continue;
        if (nsnd == MAX_SOUNDS) {
            printf("[modsound] %s: more than %d sounds in all; %s skipped\n", id, MAX_SOUNDS, e->d_name);
            continue;
        }
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        Snd *s = &snd[nsnd];
        memset(s, 0, sizeof *s);
        snprintf(s->id, sizeof s->id, "%s", id);
        snprintf(s->name, sizeof s->name, "%.*s", (int)(L - 4), e->d_name);
        if (load_wav(path, s)) {
            printf("[modsound] %s: %s is not a sound of the asset cache; skipped\n", id, e->d_name);
            continue;
        }
        if (mod_bytes(id) + (size_t)s->frames * s->channels * 2 > MODSOUND_BYTES_MAX) {
            printf("[modsound] %s: %s: more than %u MiB of sounds; skipped\n", id, s->name, MODSOUND_BYTES_MAX >> 20);
            free(s->pcm);
            continue;
        }
        nsnd++, n++;
    }
    closedir(d);
    return n;
}

int modsound_count(const char *id) {
    int n = 0;
    for (int i = 0; i < nsnd; i++) n += !id || !strcmp(snd[i].id, id);
    return n;
}

static int fill(void *ctx, int16_t *dst, int max) {
    Play *p = ctx;
    const Snd *s = p->s;
    int done = 0;
    while (done < max) {
        if (p->pos >= s->frames) {
            if (!p->loop) break;
            p->pos = 0;
        }
        int n = s->frames - p->pos;
        if (n > max - done) n = max - done;
        memcpy(dst + (size_t)done * s->channels, s->pcm + (size_t)p->pos * s->channels, (size_t)n * s->channels * 2);
        p->pos += n, done += n;
    }
    return done;
}

void modsound_gains(const float lis[3], const float front[3], const float top[3], const float src[3], float volume, int channels, float g[2][2]) {
    const float d[3] = {src[0] - lis[0], src[1] - lis[1], src[2] - lis[2]};
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float gain = dist <= MODSOUND_MIN_DIST ? 1.0f : MODSOUND_MIN_DIST / dist;
    if (dist >= MODSOUND_MAX_DIST) gain = 0;
    else if (dist > MODSOUND_MAX_DIST * 0.5f) gain *= (MODSOUND_MAX_DIST - dist) / (MODSOUND_MAX_DIST * 0.5f);
    /* right = top x front (left-handed audio axes: DirectSound) */
    const float r[3] = {top[1] * front[2] - top[2] * front[1], top[2] * front[0] - top[0] * front[2], top[0] * front[1] - top[1] * front[0]};
    const float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    float pan = 0;   /* -1 left .. 1 right */
    if (dist > 1e-4f && rl > 1e-6f) pan = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (dist * rl);
    const float a = (pan + 1.0f) * 0.25f * 3.14159265f;
    const float L = cosf(a) * gain * volume, R = sinf(a) * gain * volume;
    if (channels == 1) g[0][0] = L, g[0][1] = R, g[1][0] = g[1][1] = 0;
    else g[0][0] = L, g[0][1] = 0, g[1][0] = 0, g[1][1] = R;
}

static void world_to_audio(const float *w, float *a) { a[0] = w[0], a[1] = w[2], a[2] = w[1]; }

static void apply(Play *p) {
    float g[2][2];
    float lis[3], front[3], top[3];
    if (!p->is3d || !au.listener || !au.listener(lis, front, top)) {
        const float v = p->volume * (p->is3d ? 0.7071f : 1.0f);
        if (p->s->channels == 1) g[0][0] = g[0][1] = v * (p->is3d ? 1.0f : 0.7071f), g[1][0] = g[1][1] = 0;
        else g[0][0] = g[1][1] = v, g[0][1] = g[1][0] = 0;
    } else {
        float src[3];
        world_to_audio(p->at, src);
        modsound_gains(lis, front, top, src, p->volume, p->s->channels, g);
    }
    if (au.voice_gains) au.voice_gains(p->voice, g);
}

static int playing_of(const char *id) {
    int n = 0;
    for (int i = 0; i < MAX_PLAYS; i++) n += plays[i].handle && !strcmp(plays[i].s->id, id);
    return n;
}

int modsound_play(const char *id, const char *name, const float *pos, float volume, int loop, char *err, size_t cap) {
    if (!id || !name) return snprintf(err, cap, "ModSound(name, ...): a sound name"), 0;
    const Snd *s = NULL;
    for (int i = 0; i < nsnd && !s; i++)
        if (!strcmp(snd[i].id, id) && !strcmp(snd[i].name, name)) s = &snd[i];
    if (!s) return snprintf(err, cap, "no sound %.60s (a [sound] of the mod's assets.ini; see its [modassets] lines)", name), 0;
    if (!have_audio) return snprintf(err, cap, "no audio"), 0;
    modsound_tick();   /* frees finished voices first */
    if (playing_of(id) >= MODSOUND_PLAYING_MAX) return snprintf(err, cap, "%d sounds already playing (budget)", MODSOUND_PLAYING_MAX), 0;
    Play *p = NULL;
    for (int i = 0; i < MAX_PLAYS && !p; i++)
        if (!plays[i].handle) p = &plays[i];
    if (!p) return snprintf(err, cap, "too many mod sounds playing"), 0;
    memset(p, 0, sizeof *p);
    p->s = s, p->loop = loop != 0, p->is3d = pos != NULL;
    p->volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
    if (pos) memcpy(p->at, pos, sizeof p->at);
    p->voice = au.voice_new(s->channels, (uint32_t)s->rate, fill, p);
    if (!p->voice) return snprintf(err, cap, "no audio voice"), 0;
    p->handle = next_handle++;
    if (next_handle <= 0) next_handle = 1;
    apply(p);
    au.voice_play(p->voice, 1);
    return p->handle;
}

static Play *find(const char *id, int h) {
    for (int i = 0; h > 0 && id && i < MAX_PLAYS; i++)
        if (plays[i].handle == h && !strcmp(plays[i].s->id, id)) return &plays[i];
    return NULL;
}

int modsound_move(const char *id, int handle, const float *pos) {
    Play *p = find(id, handle);
    if (!p || !pos) return -1;
    if (au.lock) au.lock();
    memcpy(p->at, pos, sizeof p->at);
    p->is3d = 1;
    if (au.unlock) au.unlock();
    apply(p);
    return 0;
}

static void release(Play *p) {
    if (au.voice_free) au.voice_free(p->voice);   /* takes the mixer lock: the fill is done with p */
    p->handle = 0, p->voice = NULL;
}

int modsound_stop(const char *id, int handle) {
    Play *p = find(id, handle);
    if (!p) return -1;
    release(p);
    return 0;
}

int modsound_playing(const char *id) {
    modsound_tick();
    return playing_of(id);
}

void modsound_tick(void) {
    for (int i = 0; i < MAX_PLAYS; i++) {
        Play *p = &plays[i];
        if (!p->handle) continue;
        if (!au.voice_playing || !au.voice_playing(p->voice)) release(p);
        else if (p->is3d) apply(p);
    }
}

void modsound_new_state(void) {
    for (int i = 0; i < MAX_PLAYS; i++)
        if (plays[i].handle) release(&plays[i]);
}

void modsound_reset(void) {
    modsound_new_state();
    for (int i = 0; i < nsnd; i++) free(snd[i].pcm);
    nsnd = 0;
    next_handle = 1;
}
