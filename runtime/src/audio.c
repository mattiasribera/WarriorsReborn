/* Host audio output: mixer thread, backends and WAV dump (see audio.h). SDL2 is loaded with
 * dlopen like the video backend, so the build needs no audio development packages; the few
 * prototypes and constants used are declared below (values from SDL2's public headers, stable
 * across 2.x). SDL2 picks the platform's audio system at run time (PulseAudio/PipeWire/ALSA on
 * Linux, WASAPI/DirectSound on Windows). */
#define _GNU_SOURCE
#include "audio.h"
#include "platform.h"
#include "resample.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_STEP 8           /* highest source/output rate ratio the FIFO is sized for */
/* One chunk at MAX_STEP, the kernel history and lookahead on both sides, the repeated first frame
 * and the silent tail after the end (resample.h). */
#define FIFO_FRAMES (AUDIO_CHUNK * MAX_STEP + 4 * RESAMPLE_MAX_REACH + 64)

/* Source frames stream through a FIFO. The resampler reads around a fractional position pos
 * (FIFO frames), which keeps hist = max_reach - 1 frames of history behind it. A new (or reset)
 * voice starts with prime = hist + delay copies of its first frame, pos = hist: the output then
 * runs delay source frames behind the source (0 for linear, RESAMPLE_SINC_DELAY for sinc), and
 * sinc asks the source for exactly the frames linear interpolation does. At the end of the source
 * the FIFO gets a tail (silence for sinc, the last frame repeated for linear, as before), and the
 * voice stops once the kernel runs past it. */
struct AudioVoice {
    AudioVoice *next;
    int channels, playing, ended, primed, tailed;
    uint32_t rate;
    ResampleKind kind;
    AudioFill fill;
    void *ctx;
    float g[2][2], vol;
    double pos;              /* source position of the next output frame inside the FIFO */
    int fifo_n;              /* buffered source frames */
    int16_t fifo[FIFO_FRAMES * 2];
};

static pthread_mutex_t lock;   /* recursive: HLE code holds it across voice_* calls */
static pthread_once_t lock_once = PTHREAD_ONCE_INIT;
static AudioVoice *voices;
static pthread_t mixer;
static volatile int running;
static uint64_t mixed;
static FILE *dump;
static uint32_t dump_bytes;
static int is_open;
static float limiter_gain = 1.0f;
static ResampleKind resampler = RESAMPLE_SINC;
static int stats_on;
static struct { uint64_t ns, chunks, voice_chunks; } mstat;   /* audio_mix cost, for --stats */
#define LIMIT 32000.f   /* limiter ceiling, about -0.2 dBFS */

static void lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &a);
    pthread_mutexattr_destroy(&a);
}
void audio_lock(void) { pthread_once(&lock_once, lock_init); pthread_mutex_lock(&lock); }
void audio_unlock(void) { pthread_mutex_unlock(&lock); }
int audio_is_open(void) { return is_open; }
void audio_set_resampler(int kind) { resampler = kind == RESAMPLE_LINEAR ? RESAMPLE_LINEAR : RESAMPLE_SINC; }
const char *audio_resampler_name(void) { return resample_name(resampler); }
void audio_set_stats(int on) { stats_on = on; }
uint64_t audio_frames_mixed(void) { return __atomic_load_n(&mixed, __ATOMIC_SEQ_CST); }

float audio_db100_to_gain(int32_t db100) {
    if (db100 >= 0) return 1.0f;
    if (db100 <= -10000) return 0.0f;
    return powf(10.0f, (float)db100 / 2000.0f);
}

/* ---- DirectSound 3D ----
 * Distance model as documented for DirectSound 3D (IDirectSound3DBuffer::SetMinDistance /
 * SetMaxDistance, IDirectSound3DListener::SetRolloffFactor): the level halves per doubling of
 * the distance beyond the minimum distance (rolloff 1), and stays constant beyond the maximum. */
float audio_ds3d_distance_gain(float dist, float min_dist, float max_dist, float rolloff) {
    if (!(min_dist > 0) || !(rolloff > 0) || !(dist > min_dist)) return 1.0f;
    if (max_dist > min_dist && dist > max_dist) dist = max_dist;
    return min_dist / (min_dist + rolloff * (dist - min_dist));
}

/* Constant-power pan: L = sqrt((1 - p) / 2), R = sqrt((1 + p) / 2), so L^2 + R^2 = 1 at every
 * position and a centered source is 0.707 (-3 dB) per ear. */
#define PAN_DEPTH 0.8f   /* hardest pan (source fully to one side): about -9.5 dB on the far ear */
void audio_ds3d_pan(const float pos[3], const float front[3], const float top[3], const float src[3], float min_dist,
                    float lr[2]) {
    float d[3] = {src[0] - pos[0], src[1] - pos[1], src[2] - pos[2]};
    float right[3] = {top[1] * front[2] - top[2] * front[1], top[2] * front[0] - top[0] * front[2],
                      top[0] * front[1] - top[1] * front[0]};
    float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    lr[0] = lr[1] = 0.70710678f;
    if (!(dl > 1e-6f) || !(rl > 1e-6f)) return;
    float x = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (dl * rl);   /* -1 left .. 1 right */
    if (min_dist > 0 && dl < min_dist) x *= dl / min_dist;
    float p = fmaxf(-1.0f, fminf(1.0f, x)) * PAN_DEPTH;
    lr[0] = sqrtf(0.5f * (1.0f - p));
    lr[1] = sqrtf(0.5f * (1.0f + p));
}

/* ---- voices ---- */
static void voice_restart(AudioVoice *v) {
    v->fifo_n = 0;
    v->primed = v->tailed = v->ended = 0;
    v->pos = resample_max_reach(v->kind) - 1;
}

AudioVoice *voice_new(int channels, uint32_t rate, AudioFill fill, void *ctx) {
    AudioVoice *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->channels = channels == 2 ? 2 : 1;
    v->rate = rate ? rate : AUDIO_RATE;
    v->fill = fill;
    v->ctx = ctx;
    v->vol = 1.0f;
    v->kind = resampler;
    v->pos = resample_max_reach(v->kind) - 1;
    if (v->channels == 2) { v->g[0][0] = 1; v->g[1][1] = 1; }
    else { v->g[0][0] = 1; v->g[0][1] = 1; }
    audio_lock();
    v->next = voices;
    voices = v;
    audio_unlock();
    return v;
}
void voice_free(AudioVoice *v) {
    if (!v) return;
    audio_lock();
    for (AudioVoice **p = &voices; *p; p = &(*p)->next)
        if (*p == v) { *p = v->next; break; }
    audio_unlock();
    free(v);
}
void voice_play(AudioVoice *v, int on) {
    audio_lock();
    v->playing = on;
    if (on && v->ended) voice_restart(v);   /* the FIFO only holds the tail of the finished sound */
    audio_unlock();
}
int voice_playing(const AudioVoice *v) { return v->playing; }
void voice_set_rate(AudioVoice *v, uint32_t rate) { audio_lock(); if (rate) v->rate = rate; audio_unlock(); }
void voice_set_gains(AudioVoice *v, const float g[2][2]) { audio_lock(); memcpy(v->g, g, sizeof v->g); audio_unlock(); }
void voice_set_volume(AudioVoice *v, float gain) { audio_lock(); v->vol = gain; audio_unlock(); }
void voice_reset(AudioVoice *v) { audio_lock(); voice_restart(v); audio_unlock(); }

static void mix_voice(AudioVoice *v, float *acc, int n) {
    double step = (double)v->rate / AUDIO_RATE;
    if (step > MAX_STEP) step = MAX_STEP;
    int ch = v->channels, hist = resample_max_reach(v->kind) - 1, reach = resample_reach(v->kind, step);
    int prime = hist + (v->kind == RESAMPLE_SINC ? RESAMPLE_SINC_DELAY : 0);
    if (!v->primed) v->fifo_n = prime;
    int need = (int)floor(v->pos + (n - 1) * step) + reach + 1;
    while (!v->ended && v->fifo_n < need) {
        int got = v->fill(v->ctx, v->fifo + v->fifo_n * ch, need - v->fifo_n);
        if (got <= 0) { v->ended = 1; break; }
        if (!v->primed) {   /* history before the first frame: that frame repeated (no ramp-in) */
            for (int i = 0; i < prime; i++) memcpy(v->fifo + i * ch, v->fifo + prime * ch, ch * sizeof(int16_t));
            v->primed = 1;
        }
        v->fifo_n += got;
    }
    if (!v->primed) { v->fifo_n = 0; v->playing = 0; return; }   /* the source had no frames */
    if (v->ended && !v->tailed) {
        /* sinc: the delay plus the kernel's ringing after the last frame; linear: one frame */
        int tail = v->kind == RESAMPLE_SINC ? RESAMPLE_SINC_DELAY + 2 * reach + 1 : 1, last = v->fifo_n - 1;
        for (int i = 0; i < tail; i++, v->fifo_n++)
            if (v->kind == RESAMPLE_SINC) memset(v->fifo + v->fifo_n * ch, 0, ch * sizeof(int16_t));
            else memcpy(v->fifo + v->fifo_n * ch, v->fifo + last * ch, ch * sizeof(int16_t));
        v->tailed = 1;
    }
    int m = n;   /* output frames whose kernel lies inside the FIFO (all of them until the end) */
    while (m > 0 && (int)floor(v->pos + (m - 1) * step) + reach >= v->fifo_n) m--;
    float s[AUDIO_CHUNK * 2], g[2][2];
    if (m) resample_block(v->kind, v->fifo, ch, v->pos, step, s, m);
    for (int c = 0; c < 2; c++) for (int o = 0; o < 2; o++) g[c][o] = v->g[c][o] * v->vol;
    for (int k = 0; k < m; k++)
        for (int c = 0; c < ch; c++) {
            acc[k * 2] += s[k * ch + c] * g[c][0];
            acc[k * 2 + 1] += s[k * ch + c] * g[c][1];
        }
    if (m < n) { v->playing = 0; v->fifo_n = 0; return; }   /* ran past the tail: the sound ended */
    v->pos += n * step;
    int drop = (int)floor(v->pos) - hist;
    if (drop > v->fifo_n) drop = v->fifo_n;
    if (drop > 0) {
        memmove(v->fifo, v->fifo + drop * ch, (size_t)(v->fifo_n - drop) * ch * sizeof(int16_t));
        v->fifo_n -= drop;
        v->pos -= drop;
    }
}

void audio_mix(int16_t *out, int n) {
    float acc[AUDIO_CHUNK * 2];
    for (int done = 0; done < n;) {
        int m = n - done < AUDIO_CHUNK ? n - done : AUDIO_CHUNK;
        memset(acc, 0, sizeof acc);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (AudioVoice *v = voices; v; v = v->next)
            if (v->playing) { mix_voice(v, acc, m); mstat.voice_chunks++; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        mstat.ns += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
        mstat.chunks++;
        /* Peak limiter: when the chunk would clip, the gain drops at once to fit it, and it
         * recovers towards unity over about half a second; the gain ramps across the chunk. */
        float peak = 0, g0 = limiter_gain;
        for (int i = 0; i < m * 2; i++) peak = fmaxf(peak, fabsf(acc[i]));
        float g1 = g0 + (1.0f - g0) * 0.01f;
        if (peak * g1 > LIMIT) g1 = LIMIT / peak;
        if (peak * g0 > LIMIT) g0 = g1;
        for (int i = 0; i < m * 2; i++) {
            float s = acc[i] * (g0 + (g1 - g0) * (float)(i / 2 + 1) / m);
            out[done * 2 + i] = (int16_t)(s > 32767.f ? 32767 : s < -32768.f ? -32768 : lrintf(s));
        }
        limiter_gain = g1;
        done += m;
    }
}

/* ---- Xbox ADPCM ---- */
static const int16_t ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

/* Layout (vgmstream src/coding/ima_decoder.c, decode_xbox_ima): per channel a 4-byte header
 * (s16 sample, u8 step index, u8 reserved) whose sample is output 0, then 32 data bytes per
 * channel in 4-byte words interleaved across channels, low nibble first; the 64th nibble is
 * not decoded (64 samples per block). */
void xadpcm_decode_block(const uint8_t *in, int channels, int16_t *out) {
    for (int c = 0; c < channels; c++) {
        const uint8_t *h = in + 4 * c;
        int hist = (int16_t)(h[0] | h[1] << 8), idx = h[2] > 88 ? 88 : h[2];
        out[c] = (int16_t)hist;
        for (int i = 1; i < XADPCM_BLOCK_SAMPLES; i++) {
            int j = i - 1;
            uint8_t byte = in[4 * channels + 4 * channels * (j / 8) + 4 * c + (j % 8) / 2];
            int nib = (byte >> ((j & 1) * 4)) & 0xF, step = ima_steps[idx], d = step >> 3;
            if (nib & 1) d += step >> 2;
            if (nib & 2) d += step >> 1;
            if (nib & 4) d += step;
            if (nib & 8) d = -d;
            hist += d;
            hist = hist > 32767 ? 32767 : hist < -32768 ? -32768 : hist;
            idx += ima_index[nib];
            idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
            out[i * channels + c] = (int16_t)hist;
        }
    }
}

/* ---- WAV ---- */
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
int wav_write_header(void *file, int channels, uint32_t rate) {
    uint8_t h[44];
    memcpy(h, "RIFF\0\0\0\0WAVEfmt ", 16);
    put32(h + 16, 16);
    put32(h + 20, 1 | (uint32_t)channels << 16);      /* PCM, channels */
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 2);
    put32(h + 32, (uint32_t)channels * 2 | 16u << 16);   /* block align, bits */
    memcpy(h + 36, "data\0\0\0\0", 8);
    return fwrite(h, 1, 44, file) == 44 ? 0 : -1;
}
void wav_finish(void *file, uint32_t data_bytes) {
    uint8_t b[4];
    long end = ftell(file);
    put32(b, data_bytes + 36);
    fseek(file, 4, SEEK_SET);
    fwrite(b, 1, 4, file);
    put32(b, data_bytes);
    fseek(file, 40, SEEK_SET);
    fwrite(b, 1, 4, file);
    fseek(file, end, SEEK_SET);
    fflush(file);
}

/* ---- backends ---- */
#define SDL_INIT_AUDIO 0x10u
#define AUDIO_S16LSB 0x8010
typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels, silence;
    uint16_t samples, padding;
    uint32_t size;
    void *callback, *userdata;
} SDL_AudioSpec;
static struct {
    void *lib;
    uint32_t dev;
    int (*InitSubSystem)(uint32_t);
    void (*QuitSubSystem)(uint32_t);
    const char *(*GetError)(void);
    uint32_t (*OpenAudioDevice)(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int);
    void (*PauseAudioDevice)(uint32_t, int);
    int (*QueueAudio)(uint32_t, const void *, uint32_t);
    uint32_t (*GetQueuedAudioSize)(uint32_t);
    void (*CloseAudioDevice)(uint32_t);
    const char *(*GetCurrentAudioDriver)(void);
} sdl;

static int sdl_open(void) {
    sdl.lib = plat_dlopen(PLAT_LIB_SDL2);
    if (!sdl.lib) { printf("[audio] cannot load SDL2: %s\n", plat_dlerror()); return -1; }
#define SYM(f) if (!(*(void **)&sdl.f = plat_dlsym(sdl.lib, "SDL_" #f))) { printf("[audio] SDL_%s missing\n", #f); return -1; }
    SYM(InitSubSystem) SYM(QuitSubSystem) SYM(GetError) SYM(OpenAudioDevice) SYM(PauseAudioDevice) SYM(QueueAudio)
    SYM(GetQueuedAudioSize) SYM(CloseAudioDevice) SYM(GetCurrentAudioDriver)
#undef SYM
    if (sdl.InitSubSystem(SDL_INIT_AUDIO)) { printf("[audio] SDL audio init: %s\n", sdl.GetError()); return -1; }
    SDL_AudioSpec want = {AUDIO_RATE, AUDIO_S16LSB, 2, 0, 1024, 0, 0, NULL, NULL}, have;
    sdl.dev = sdl.OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!sdl.dev) { printf("[audio] no audio device: %s\n", sdl.GetError()); sdl.QuitSubSystem(SDL_INIT_AUDIO); return -1; }
    sdl.PauseAudioDevice(sdl.dev, 0);
    printf("[audio] SDL2 device open (driver %s, 48000 Hz stereo)\n", sdl.GetCurrentAudioDriver());
    return 0;
}

/* Keeps the device queue between one and MAX_QUEUE_MS: the mixer runs on the host clock, so a
 * device clock that runs slow would otherwise let the queue (and the latency) grow. */
#define MAX_QUEUE_MS 150
static void sdl_write(const int16_t *pcm, int frames) {
    if (!sdl.dev) return;
    if (sdl.GetQueuedAudioSize(sdl.dev) > (uint32_t)AUDIO_RATE * 4 * MAX_QUEUE_MS / 1000) return;
    sdl.QueueAudio(sdl.dev, pcm, (uint32_t)frames * 4);
}

/* ---- mixer thread ---- */
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void *mixer_main(void *arg) {
    (void)arg;
    uint64_t t0 = now_ns(), last_patch = 0;
    int16_t pcm[AUDIO_CHUNK * 2];
    while (running) {
        uint64_t due = (now_ns() - t0) * AUDIO_RATE / 1000000000ull;
        if (due > mixed + AUDIO_RATE) mixed = due - AUDIO_RATE;   /* after a long host stall keep at most 1 s */
        while (mixed + AUDIO_CHUNK <= due) {
            audio_lock();
            audio_mix(pcm, AUDIO_CHUNK);
            audio_unlock();
            __atomic_add_fetch(&mixed, AUDIO_CHUNK, __ATOMIC_SEQ_CST);
            sdl_write(pcm, AUDIO_CHUNK);
            if (dump && fwrite(pcm, 4, AUDIO_CHUNK, dump) == AUDIO_CHUNK) dump_bytes += AUDIO_CHUNK * 4;
        }
        if (dump && mixed - last_patch >= AUDIO_RATE) { wav_finish(dump, dump_bytes); last_patch = mixed; }
        if (stats_on && mstat.chunks >= 5 * AUDIO_RATE / AUDIO_CHUNK) {
            audio_lock();
            double us = mstat.ns / 1e3, ch = (double)mstat.chunks, vc = (double)mstat.voice_chunks;
            memset(&mstat, 0, sizeof mstat);
            audio_unlock();
            /* A chunk is AUDIO_CHUNK frames = 5 ms of audio, so us per chunk / 50 = % of a core. */
            printf("[stats] audio mixer (%s): %.1f us per 5 ms chunk (%.2f%% of a core), %.1f voices playing, "
                   "%.2f us per voice per chunk (%.3f%% of a core per voice)\n",
                   resample_name(resampler), us / ch, us / ch / 50, vc / ch, vc ? us / vc : 0, vc ? us / vc / 50 : 0);
        }
        nanosleep(&(struct timespec){0, 2 * 1000 * 1000}, NULL);
    }
    return NULL;
}

const char *audio_open(const char *pref, const char *dump_path) {
    if (is_open) return sdl.dev ? "sdl" : "null";
    if (dump_path) {
        dump = fopen(dump_path, "wb");
        if (!dump || wav_write_header(dump, 2, AUDIO_RATE)) { perror(dump_path); if (dump) fclose(dump); dump = NULL; }
        else printf("[audio] dumping the mix to %s\n", dump_path);
    }
    const char *name = "null";
    if (pref && !strcmp(pref, "sdl")) {
        if (!sdl_open()) name = "sdl";
        else printf("[audio] falling back to the null backend (no sound output)\n");
    }
    running = 1;
    int err = pthread_create(&mixer, NULL, mixer_main, NULL);
    if (err) {
        running = 0;
        printf("[audio] cannot start the mixer thread: %s\n", strerror(err));
        plat_report_memory("mixer thread creation failed");
    }
    is_open = 1;
    atexit(audio_close);
    return name;
}

void audio_close(void) {
    if (!is_open) return;
    is_open = 0;
    if (running) { running = 0; pthread_join(mixer, NULL); }
    if (dump) {
        wav_finish(dump, dump_bytes);
        fclose(dump);
        dump = NULL;
        printf("[audio] dump: %u frames (%.1f s)\n", dump_bytes / 4, dump_bytes / 4.0 / AUDIO_RATE);
    }
    if (sdl.dev) { sdl.CloseAudioDevice(sdl.dev); sdl.dev = 0; }
}
