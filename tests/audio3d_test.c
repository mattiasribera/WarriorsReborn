/* Game-independent test of XACT 3D sound positioning (built by runtime/Makefile as
 * build/audio3d_test, run by tests/test_audio3d.py).
 *
 * 1. The DirectSound 3D distance model and stereo pan helpers in src/audio.c.
 * 2. The HLE setters in src/hle_audio.c, end to end: a synthetic sound bank whose cue 0 is a 3D
 *    sound (DS3D record min 1, max 40) plays on a sound source; the listener and source setters
 *    are called through hle_audio_dispatch with the title's argument layout, and the mixer
 *    (audio_mix, called directly: no mixer thread) must attenuate and pan the cue. 2D cues stay
 *    at their entry volume.
 * 3. The I3DL2 reverb wiring: SetI3DL2Listener reaches the mixer's reverb, 3D cues send to it
 *    (a tail rings after the cue stops), 2D cues do not, and --no-reverb mutes it.
 * 4. Doppler and the front/back cue: the ratio math, the velocity estimate from position
 *    deltas, the rear low-pass coefficient, and end to end the pitch of a 3D cue on a moving
 *    source (title velocities or estimated), 2D cues and --no-doppler unchanged. */
#define _GNU_SOURCE
#include "../runtime/src/hle_audio.h"
#include "../runtime/src/audio.h"
#include "../runtime/src/steam_audio.h"
#include "../runtime/src/xact.h"
#include "../runtime/src/resample.h"
#include "../runtime/tools/audio_synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

/* Same layout as hle_audio.c's frame (built by hle_entry_common). */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t stub_ret, index, ret_addr;
    uint32_t args[16];
} Frame;
void hle_audio_dispatch(Frame *f);

static uint32_t call(const char *name, int nargs, const uint32_t *args) {
    for (int i = 0; i < hle_audio_entry_count(); i++)
        if (!strcmp(hle_audio_entry_name(i), name)) {
            Frame f;
            memset(&f, 0, sizeof f);
            f.index = (uint32_t)i;
            memcpy(f.args, args, (size_t)nargs * 4);
            hle_audio_dispatch(&f);
            return f.eax;
        }
    printf("FAIL no entry %s\n", name);
    failures++;
    return 0xFFFFFFFFu;
}
#define A(...) (const uint32_t[]){__VA_ARGS__}
#define P(x) ((uint32_t)(uintptr_t)(x))
static uint32_t F(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }

/* Mean absolute level of the left and right channels over the next n output frames. */
static void level(int n, double lr[2]) {
    int16_t *out = calloc((size_t)n * 2, sizeof *out);
    audio_lock();
    audio_mix(out, n);
    audio_unlock();
    lr[0] = lr[1] = 0;
    for (int i = 0; i < n; i++) { lr[0] += abs(out[2 * i]); lr[1] += abs(out[2 * i + 1]); }
    lr[0] /= n;
    lr[1] /= n;
    free(out);
}

/* Frequency (cycles per output frame) of the left channel over frames [a, b) of the next n
 * output frames, from its first and last upward zero crossings (linearly interpolated). */
static double tone(int n, int a, int b) {
    int16_t *out = calloc((size_t)n * 2, sizeof *out);
    audio_lock();
    audio_mix(out, n);
    audio_unlock();
    double first = -1, last = -1;
    int cycles = -1;
    for (int i = a + 1; i < b && i < n; i++) {
        int y0 = out[2 * (i - 1)], y1 = out[2 * i];
        if (y0 < 0 && y1 >= 0) {
            double x = i - 1 + (double)-y0 / (y1 - y0);
            if (first < 0) first = x;
            last = x;
            cycles++;
        }
    }
    free(out);
    return cycles > 0 ? cycles / (last - first) : 0;
}
static double fake_now;
static double fake_clock(void) { return fake_now; }
/* One-pole y += a (x - y) magnitude at f Hz (48 kHz). */
static double lp_gain(double a, double f) {
    double w = 2 * 3.14159265358979323846 * f / 48000, re = 1 - (1 - a) * cos(w), im = (1 - a) * sin(w);
    return a / sqrt(re * re + im * im);
}

/* ---- 3D audio: air absorption, occlusion, binaural ---- */
static double one_pole_a(double fc) { return 1 - exp(-2 * 3.14159265358979323846 * fc / 48000); }

/* A mono 48 kHz voice over a caller's samples (looping when loop is set). */
typedef struct { const int16_t *pcm; int n, pos, loop; } Src;
static int src_fill(void *ctx, int16_t *dst, int max) {
    Src *s = ctx;
    int k = 0;
    while (k < max) {
        if (s->pos >= s->n) { if (!s->loop) break; s->pos = 0; }
        dst[k++] = s->pcm[s->pos++];
    }
    return k;
}
/* Mixes n frames of the voices that play now into out (stereo s16). */
static void mix_n(int16_t *out, int n) { audio_lock(); audio_mix(out, n); audio_unlock(); }

static int fake_walls_n;
static int fake_walls(const float lis[3], const float s[3]) { (void)lis, (void)s; return fake_walls_n; }
static int fake_geo_ok;
static uint32_t fake_geo_key;
static float fake_geo_pos[3];
static int fake_geo_bend;   /* pathing: report the sound as coming from fake_geo_pos */
static int fake_geo(uint32_t key, const float lis[3], const float s[3], float *g, float *lp, float pos[3]) {
    (void)lis, (void)s;
    fake_geo_key = key;
    if (!fake_geo_ok) return -1;
    *g = 0.5f;
    *lp = (float)one_pole_a(1200);
    if (fake_geo_bend) memcpy(pos, fake_geo_pos, sizeof fake_geo_pos);
    return 0;
}

/* Left and right RMS over the next n output frames. */
static void rms_lr(int n, double lr[2]) {
    int16_t *out = calloc((size_t)n * 2, sizeof *out);
    mix_n(out, n);
    lr[0] = lr[1] = 0;
    for (int i = 0; i < n; i++) { lr[0] += (double)out[2 * i] * out[2 * i]; lr[1] += (double)out[2 * i + 1] * out[2 * i + 1]; }
    lr[0] = sqrt(lr[0] / n);
    lr[1] = sqrt(lr[1] / n);
    free(out);
}

/* ---- Steam Audio: --audio-hrtf through Steam Audio's measured HRTF ---- */
/* Plays cue 0 on src at (x, y, z) and mixes n frames into out (stereo s16). */
static void play_mix(uint32_t bank, uint32_t src, float x, float y, float z, int16_t *out, int n) {
    uint32_t pl[4] = {0, 0, src, 0}, cue = 0;
    call("IXACTSoundSource_SetPosition?", 5, A(src, F(x), F(y), F(z), 1));
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    mix_n(out, n);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
}
static void energy_lr(const int16_t *out, int from, int n, double e[2]) {
    e[0] = e[1] = 0;
    for (int i = from; i < n; i++) e[0] += (double)out[2 * i] * out[2 * i], e[1] += (double)out[2 * i + 1] * out[2 * i + 1];
}
static void test_steam_hrtf(uint32_t bank, uint32_t src, uint8_t *xwb) {
    enum { N = 1920 };
    static int16_t ref[2 * N], got[2 * N];
    uint32_t r = 777;
    for (int i = 0; i < 1000; i++) { r = r * 1103515245u + 12345u; sy_w16(xwb + 0x80 + 2 * i, (uint16_t)(int16_t)((int)(r >> 16 & 0x3fff) - 8192)); }
    hle_audio_set_hrtf(1);
    /* a missing library: --audio-hrtf keeps the built-in model, sample for sample */
    play_mix(bank, src, 4, 0, 1, ref, N);
    CHECK(sa_open("/nonexistent/" SA_LIB_NAME) == -1, "steam audio: a missing library fails to load");
    audio_set_steam_hrtf(1);
    CHECK(!audio_steam_hrtf(), "steam audio: the measured HRTF stays off without the library");
    play_mix(bank, src, 4, 0, 1, got, N);
    CHECK(!memcmp(ref, got, sizeof ref), "steam audio missing: --audio-hrtf sample-identical to the built-in model");
    sa_close();
    if (sa_open(NULL)) {
        printf("SKIP steam audio: library not built (make steam-audio)\n");
        hle_audio_set_hrtf(0);
        return;
    }
    audio_set_steam_hrtf(1);
    CHECK(audio_steam_hrtf(), "steam audio: the measured HRTF is on");
    double e[2], pan[2], ahead[2];
    play_mix(bank, src, 4, 0, 0, got, N);
    energy_lr(got, 480, N, e);
    CHECK(e[1] > 4 * e[0], "steam audio HRTF: a source on the right is louder right (%.1f dB)", 10 * log10(e[1] / (e[0] + 1)));
    play_mix(bank, src, -4, 0, 0, got, N);
    energy_lr(got, 480, N, e);
    CHECK(e[0] > 4 * e[1], "steam audio HRTF: a source on the left is louder left (%.1f dB)", 10 * log10(e[0] / (e[1] + 1)));
    static int16_t front[2 * N];
    play_mix(bank, src, 0, 0, 4, front, N);
    energy_lr(front, 480, N, ahead);
    hle_audio_set_hrtf(0);
    play_mix(bank, src, 0, 0, 4, ref, N);
    energy_lr(ref, 480, N, pan);
    hle_audio_set_hrtf(1);
    const double lvl = 10 * log10((ahead[0] + ahead[1]) / (pan[0] + pan[1]));
    CHECK(fabs(lvl) < 3, "steam audio HRTF: a source ahead about as loud as the pan (%+.1f dB)", lvl);
    /* behind: another timbre than ahead (the pinna's front/back cue; first-difference energy: the
     * treble), while the built-in model needs the rear low-pass for it */
    play_mix(bank, src, 0, 0, -4, got, N);
    double hf_a = 0, hf_b = 0;
    for (int i = 481; i < N; i++)
        for (int c = 0; c < 2; c++) {
            const double da = front[2 * i + c] - front[2 * i - 2 + c], db = got[2 * i + c] - got[2 * i - 2 + c];
            hf_a += da * da, hf_b += db * db;
        }
    CHECK(fabs(10 * log10(hf_b / hf_a)) > 1, "steam audio HRTF: behind sounds different from ahead (treble %+.1f dB)", 10 * log10(hf_b / hf_a));
    audio_set_steam_hrtf(0);
    hle_audio_set_hrtf(0);
    sa_close();
}

static void test_audio3d_extras(uint32_t eng, uint32_t bank, uint32_t src, uint8_t *xwb) {
    const float o[3] = {0, 0, 0}, fz[3] = {0, 0, 1}, up[3] = {0, 1, 0};
    /* air absorption */
    {
        const double a2k = one_pole_a(2000);
        float a0 = audio_air_lowpass(0.5f, 1), a1 = audio_air_lowpass(1, 1), a10 = audio_air_lowpass(11, 1), a25 = audio_air_lowpass(26, 1),
              a100 = audio_air_lowpass(101, 1);
        CHECK(a0 == 1 && a1 == 1, "air: inside and at the min distance: bypass (%.3f %.3f)", a0, a1);
        CHECK(near(a25, (float)(1 - 0.5 * (1 - a2k)), 1e-5f), "air: 25 units beyond min: halfway to the 2 kHz one-pole (a %.4f)", a25);
        int mono = 1;
        float prev = 1;
        for (int d = 1; d <= 400; d++) { float a = audio_air_lowpass((float)d, 1); mono &= a <= prev && a > a2k; prev = a; }
        CHECK(mono, "air: falls with distance, never below the 2 kHz one-pole");
        CHECK(lp_gain(a10, 10000) > 0.75 && lp_gain(a100, 10000) < 0.45 && lp_gain(a100, 300) > 0.98,
              "air: gentle: 10 kHz at 10 units beyond min %.1f dB, at 100 %.1f dB; 300 Hz at 100 %.2f dB", 20 * log10(lp_gain(a10, 10000)),
              20 * log10(lp_gain(a100, 10000)), 20 * log10(lp_gain(a100, 300)));
    }
    /* occlusion curve */
    {
        float g, lp;
        audio_occlusion(0, &g, &lp);
        CHECK(g == 1 && lp == 1, "occlusion 0: unchanged (%.3f %.3f)", g, lp);
        audio_occlusion(1, &g, &lp);
        CHECK(near(g, 0.5f, 1e-6f) && near(lp, (float)one_pole_a(1200), 1e-5f), "one wall: x0.5 and a 1.2 kHz one-pole (%.3f %.4f)", g, lp);
        audio_occlusion(2, &g, &lp);
        float g2 = g, lp2 = lp;
        CHECK(near(g, 1 / 3.0f, 1e-6f) && near(lp, (float)one_pole_a(650), 1e-5f), "two walls: x0.33 and 650 Hz (%.3f %.4f)", g, lp);
        audio_occlusion(5, &g, &lp);
        CHECK(g == g2 && lp == lp2, "more walls: as two");
        audio_occlusion(0.5f, &g, &lp);
        CHECK(near(g, 0.75f, 1e-6f) && lp < 1 && lp > one_pole_a(1200), "half a wall (easing): in between (%.3f %.4f)", g, lp);
        CHECK(audio_lowpass_min(0.5f, 0.8f) == 0.5f && audio_lowpass_min(1, 0.3f) == 0.3f && audio_lowpass_min(1, 1) == 1, "two one-poles: the duller");
    }
    /* binaural parameters */
    {
        AudioBinaural b;
        const float itd = 0.0875f / 343 * (3.14159265f / 2 + 1) * 48000;
        audio_binaural(o, fz, up, (const float[3]){0, 0, 10}, 1, &b);
        CHECK(near(b.gain[0], 0.70710678f, 1e-5f) && near(b.gain[1], 0.70710678f, 1e-5f) && b.delay[0] == 0 && b.delay[1] == 0 && b.shadow[0] == 1 &&
                  b.shadow[1] == 1 && near(b.elev, 0, 1e-6f),
              "binaural ahead: 0.707 per ear, no delay, no shadow");
        audio_binaural(o, fz, up, (const float[3]){10, 0, 0}, 1, &b);
        CHECK(near(b.delay[0], itd, 0.05f) && b.delay[1] == 0, "binaural right: left ear %.2f frames late (Woodworth %.2f), right 0", b.delay[0], itd);
        CHECK(b.gain[1] > b.gain[0] && near(b.gain[0] * b.gain[0] + b.gain[1] * b.gain[1], 1, 1e-5f) && b.gain[0] > 0.5f,
              "binaural right: a small broadband level difference, power 1 (%.3f %.3f)", b.gain[0], b.gain[1]);
        CHECK(b.shadow[1] == 1 && near(b.shadow[0], (float)one_pole_a(1800), 0.01f) && lp_gain(b.shadow[0], 8000) < 0.25,
              "binaural right: head shadow on the left ear (a %.3f, %.1f dB at 8 kHz)", b.shadow[0], 20 * log10(lp_gain(b.shadow[0], 8000)));
        AudioBinaural l;
        audio_binaural(o, fz, up, (const float[3]){-10, 0, 0}, 1, &l);
        CHECK(near(l.delay[1], b.delay[0], 1e-5f) && l.delay[0] == 0 && near(l.gain[0], b.gain[1], 1e-6f) && near(l.shadow[1], b.shadow[0], 1e-6f),
              "binaural left: mirrored");
        audio_binaural(o, fz, up, (const float[3]){7, 0, 7}, 1, &l);
        CHECK(l.delay[0] > 0.3f * itd && l.delay[0] < b.delay[0] && l.shadow[0] > b.shadow[0] && l.shadow[0] < 1,
              "binaural 45 degrees right: in between (%.2f frames, a %.3f)", l.delay[0], l.shadow[0]);
        audio_binaural(o, fz, up, (const float[3]){0, 10, 0}, 1, &l);
        CHECK(l.elev > 0.9f && l.delay[0] == 0 && l.delay[1] == 0, "binaural above: elevation cue %.2f", l.elev);
        audio_binaural(o, fz, up, (const float[3]){0, -10, 3}, 1, &l);
        CHECK(l.elev < -0.8f, "binaural below: elevation cue %.2f", l.elev);
        audio_binaural(o, fz, up, (const float[3]){0.25f, 0, 0}, 1, &l);
        CHECK(l.delay[0] > 0 && l.delay[0] < 0.3f * itd, "binaural inside the min distance: narrowed (%.2f frames)", l.delay[0]);
        audio_binaural(o, fz, up, o, 1, &l);
        CHECK(l.delay[0] == 0 && l.delay[1] == 0 && near(l.gain[0], 0.70710678f, 1e-6f), "binaural at the listener: centered");
    }
    /* binaural voices in the mixer: an impulse on the right reaches the left ear ~31.5 frames late,
     * duller; a voice whose binaural render was turned off again is sample-identical to one never
     * given it. */
    {
        audio_set_resampler(RESAMPLE_LINEAR);
        static int16_t imp[2000];
        imp[100] = 20000;
        Src s1 = {imp, 2000, 0, 0};
        AudioVoice *v = voice_new(1, 48000, src_fill, &s1);
        AudioBinaural b;
        audio_binaural(o, fz, up, (const float[3]){10, 0, 0}, 1, &b);
        voice_set_binaural(v, &b);
        voice_play(v, 1);
        static int16_t out[2 * 960];
        mix_n(out, 960);
        int pl = 0, pr = 0;
        double el = 0, er = 0;
        for (int i = 0; i < 960; i++) {
            if (abs(out[2 * i]) > abs(out[2 * pl])) pl = i;
            if (abs(out[2 * i + 1]) > abs(out[2 * pr + 1])) pr = i;
            el += (double)out[2 * i] * out[2 * i];
            er += (double)out[2 * i + 1] * out[2 * i + 1];
        }
        CHECK(pl - pr >= 31 && pl - pr <= 34, "binaural impulse on the right: left ear peak %d frames after the right", pl - pr);
        CHECK(er > 4 * el && out[2 * pr + 1] > 10000, "binaural impulse on the right: right ear louder (energy x%.1f), peak %d", er / (el + 1), out[2 * pr + 1]);
        voice_free(v);

        static int16_t noise[4800];
        uint32_t r = 12345;
        for (int i = 0; i < 4800; i++) { r = r * 1103515245u + 12345u; noise[i] = (int16_t)((int)(r >> 16 & 0x7fff) - 16384); }
        static int16_t ref[2 * 4800], got[2 * 4800];
        Src sa = {noise, 4800, 0, 0}, sb = {noise, 4800, 0, 0};
        AudioVoice *va = voice_new(1, 48000, src_fill, &sa);
        voice_set_gains(va, (const float[2][2]){{0.4f, 0.9f}, {0, 0}});
        voice_set_volume(va, 0.7f);
        voice_set_lowpass(va, 0.6f);
        voice_play(va, 1);
        mix_n(ref, 4800);
        voice_free(va);
        AudioVoice *vb = voice_new(1, 48000, src_fill, &sb);
        voice_set_gains(vb, (const float[2][2]){{0.4f, 0.9f}, {0, 0}});
        voice_set_volume(vb, 0.7f);
        voice_set_lowpass(vb, 0.6f);
        voice_set_binaural(vb, &b);
        voice_set_binaural(vb, NULL);
        voice_play(vb, 1);
        mix_n(got, 4800);
        voice_free(vb);
        CHECK(!memcmp(ref, got, sizeof ref), "binaural off again: sample-identical to the stereo pan");
        audio_set_resampler(RESAMPLE_SINC);
    }

    /* End to end through the HLE: cue 0 (3D, mono 24 kHz, min 1 max 40) on src. A 6 kHz pattern
     * (four source frames per cycle) shows the low-passes; the wave is 1000 frames (2000 output
     * frames), so each window measures the first 960. */
    for (int i = 0; i < 1000; i++) sy_w16(xwb + 0x80 + 2 * i, (uint16_t)(int16_t)(i % 4 == 1 ? 8000 : i % 4 == 3 ? -8000 : 0));
    uint32_t pl[4] = {0, 0, src, 0}, cue = 0;
    double base[2], got[2];
    hle_audio_set_air(0);
    hle_audio_set_occlusion(0);
    call("IXACTSoundSource_SetPosition?", 5, A(src, F(0), F(0), F(30), 1));
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, base);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    hle_audio_set_air(1);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    double want = lp_gain(audio_air_lowpass(30, 1), 6000);
    CHECK(base[0] > 10 && near((float)(got[0] / base[0]), (float)want, 0.02f), "--audio-air: 6 kHz at 30 units x%.3f (one-pole x%.3f)", got[0] / base[0], want);
    hle_audio_set_air(0);

    /* occlusion: the occluder says one wall; the title clock eases it in */
    hle_audio_set_occluder(fake_walls);
    hle_audio_set_occlusion(1);
    fake_walls_n = 0;
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(got[0] == base[0] && got[1] == base[1], "--audio-occlusion, no wall: unchanged (%.2f %.2f)", got[0], base[0]);
    double same[2];   /* the unoccluded level in the window the easing is read in (frames 1440-1920) */
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(1440, same);
    rms_lr(480, same);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    fake_walls_n = 1;
    hle_audio_clock = fake_clock;
    fake_now = 100;
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    want = 0.5 * lp_gain(one_pole_a(1200), 6000);
    CHECK(near((float)(got[0] / base[0]), (float)want, 0.03f), "--audio-occlusion, one wall: 6 kHz x%.3f (x0.5 and a 1.2 kHz one-pole: x%.3f)",
          got[0] / base[0], want);
    fake_walls_n = 0;   /* the wall is gone: eases out on the title clock */
    fake_now += 1 / 60.0;
    call("XACTEngineDoWork", 0, A(0));
    rms_lr(480, got);
    double mid = got[0];
    for (int k = 0; k < 60; k++) { fake_now += 1 / 60.0; call("XACTEngineDoWork", 0, A(0)); }
    rms_lr(480, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(mid < 0.8 * base[0] && near((float)(got[0] / same[0]), 1, 0.01f), "--audio-occlusion: eased out over the title clock (x%.3f after a frame, x%.3f after 1 s)",
          mid / base[0], got[0] / same[0]);
    /* Steam Audio: the geometry's result takes the wall map's place; without one, the wall map */
    fake_walls_n = 2;
    fake_geo_ok = 1;
    hle_audio_set_geo_occluder(fake_geo);
    fake_now += 5;
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    want = 0.5 * lp_gain(one_pole_a(1200), 6000);
    CHECK(near((float)(got[0] / base[0]), (float)want, 0.03f) && fake_geo_key != 0,
          "--audio-occlusion with the geometry: its gain and one-pole (x%.3f, want x%.3f), not the wall map's two walls", got[0] / base[0], want);
    fake_geo_ok = 0;
    fake_now += 5;
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    want = 1.0 / 3 * lp_gain(one_pole_a(650), 6000);
    CHECK(near((float)(got[0] / base[0]), (float)want, 0.03f), "--audio-occlusion, no geometry result: the wall map (two walls x%.3f, want x%.3f)",
          got[0] / base[0], want);
    hle_audio_set_geo_occluder(NULL);
    fake_walls_n = 1;
    hle_audio_set_occlusion(0);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(got[0] == base[0] && got[1] == base[1], "--no-audio-occlusion with a wall: unchanged");
    hle_audio_clock = NULL;

    /* --audio-hrtf: a 3D cue on the right is louder right and the left ear is late; off again, the pan */
    call("IXACTSoundSource_SetPosition?", 5, A(src, F(4), F(0), F(0), 1));
    for (int i = 0; i < 1000; i++) sy_w16(xwb + 0x80 + 2 * i, 4000);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, base);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    hle_audio_set_hrtf(1);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(got[1] > 1.1 * got[0] && got[1] > 0.6 * base[1], "--audio-hrtf, source on the right: R %.0f > L %.0f (pan: %.0f / %.0f)", got[1], got[0], base[1], base[0]);
    hle_audio_set_hrtf(0);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(got[0] == base[0] && got[1] == base[1], "--audio-hrtf off: the pan, unchanged");
    /* Steam Audio pathing: the geometry says the source on the right is heard through an
     * opening on the left: the pan follows the opening */
    hle_audio_set_occlusion(1);
    hle_audio_set_geo_occluder(fake_geo);
    fake_geo_ok = 1;
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, base);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    fake_geo_bend = 1;
    fake_geo_pos[0] = -4, fake_geo_pos[1] = 0, fake_geo_pos[2] = 0;
    hle_audio_clock = fake_clock;
    fake_now += 5;   /* a long gap: the new direction at once */
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    rms_lr(960, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(base[1] > 1.5 * base[0] && got[0] > 1.5 * got[1] && near((float)(got[0] * got[0] + got[1] * got[1]), (float)(base[0] * base[0] + base[1] * base[1]), 0.02f * (float)(base[0] * base[0] + base[1] * base[1])),
          "pathing: a source on the right heard through an opening on the left: L %.0f R %.0f (in place: L %.0f R %.0f), same power", got[0], got[1], base[0], base[1]);
    hle_audio_clock = NULL;
    fake_geo_bend = 0;
    fake_geo_ok = 0;
    hle_audio_set_geo_occluder(NULL);
    hle_audio_set_occlusion(0);
    test_steam_hrtf(bank, src, xwb);
    hle_audio_set_occluder(NULL);
    hle_audio_set_air(1);
    hle_audio_set_occlusion(1);
}

int main(void) {
    /* ---- distance model: gain = min / (min + rolloff * (clamp(d, min, max) - min)) ---- */
    CHECK(near(audio_ds3d_distance_gain(0.5f, 1, 40, 1), 1.0f, 1e-6f), "inside the minimum distance -> 1");
    CHECK(near(audio_ds3d_distance_gain(4, 1, 40, 1), 0.25f, 1e-6f), "4 x min distance -> 1/4 (-12 dB)");
    CHECK(near(audio_ds3d_distance_gain(10, 5, 50, 1), 0.5f, 1e-6f), "min 5, distance 10 -> 1/2");
    CHECK(near(audio_ds3d_distance_gain(40, 1, 40, 1), 1.0f / 40, 1e-6f), "at max distance -> min/max");
    CHECK(near(audio_ds3d_distance_gain(400, 1, 40, 1), 1.0f / 40, 1e-6f), "beyond max distance the gain stays at min/max");
    CHECK(near(audio_ds3d_distance_gain(4, 1, 40, 2), 1.0f / 7, 1e-6f), "rolloff factor 2 at 4 x min -> 1/7");
    CHECK(near(audio_ds3d_distance_gain(4, 1, 40, 0), 1.0f, 1e-6f), "rolloff factor 0 -> no attenuation");
    CHECK(near(audio_ds3d_distance_gain(4, 0, 40, 1), 1.0f, 1e-6f), "degenerate min distance 0 -> no attenuation");

    /* ---- rolloff curve (Xbox SetRolloffCurve, full-HRTF calculator 0x3af4c8): n points at
     * min + k (max - min) / n, k = 1..n, 1.0 at min, linear between them. This title's curve:
     * bytes 206 163 124 91 63 40 22 10 2 0, each / 255. ---- */
    {
        static const uint8_t pts[10] = {206, 163, 124, 91, 63, 40, 22, 10, 2, 0};
        float cv[10];
        for (int k = 0; k < 10; k++) cv[k] = pts[k] / 255.0f;
        CHECK(near(audio_ds3d_curve_gain(0.5f, 1, 41, cv, 10, 1), 1.0f, 1e-6f), "curve: inside min -> 1");
        CHECK(near(audio_ds3d_curve_gain(1, 1, 41, cv, 10, 1), 1.0f, 1e-6f), "curve: at min -> 1");
        CHECK(near(audio_ds3d_curve_gain(3, 1, 41, cv, 10, 1), 1.0f + (cv[0] - 1.0f) * 0.5f, 1e-5f),
              "curve: halfway to the first point -> %.4f (expected %.4f)", audio_ds3d_curve_gain(3, 1, 41, cv, 10, 1),
              1.0f + (cv[0] - 1.0f) * 0.5f);
        CHECK(near(audio_ds3d_curve_gain(5, 1, 41, cv, 10, 1), cv[0], 1e-5f), "curve: at the first point -> 206/255");
        CHECK(near(audio_ds3d_curve_gain(11, 1, 41, cv, 10, 1), (cv[1] + cv[2]) / 2, 1e-5f),
              "curve: between points 2 and 3 -> %.4f (expected %.4f)", audio_ds3d_curve_gain(11, 1, 41, cv, 10, 1), (cv[1] + cv[2]) / 2);
        CHECK(near(audio_ds3d_curve_gain(39, 1, 41, cv, 10, 1), cv[8] / 2, 1e-5f), "curve: between the last two points -> 1/255");
        CHECK(audio_ds3d_curve_gain(41, 1, 41, cv, 10, 1) == 0.0f && audio_ds3d_curve_gain(400, 1, 41, cv, 10, 1) == 0.0f,
              "curve: at and beyond max -> the last point (0, silent)");
        CHECK(near(audio_ds3d_curve_gain(4, 1, 40, cv, 0, 1), 0.25f, 1e-6f) && near(audio_ds3d_curve_gain(4, 1, 40, NULL, 10, 1), 0.25f, 1e-6f),
              "no curve -> the inverse distance law");
        CHECK(near(audio_ds3d_curve_gain(4, 5, 5, cv, 10, 1), 1.0f, 1e-6f), "curve with max = min -> the inverse distance law (1 inside min)");
        /* The voices' records (min 1, max 40 / 50): a speaker 10 units away. */
        float g40 = audio_ds3d_curve_gain(10, 1, 40, cv, 10, 1);
        CHECK(near(20 * log10f(g40), -4.5f, 0.1f), "voice record min 1 max 40 at 10 units: %.1f dB (inverse distance: -20 dB)", 20 * log10f(g40));
    }

    /* ---- pan: DirectSound left-handed axes, right = top x front; constant power ----
     * L = sqrt((1 - p) / 2), R = sqrt((1 + p) / 2): centered 0.707/0.707 (-3 dB per ear) and
     * L^2 + R^2 = 1 at every pan position. */
    const float o[3] = {0, 0, 0}, fz[3] = {0, 0, 1}, up[3] = {0, 1, 0};
    const float c = 0.70710678f;
    float lr[2];
    audio_ds3d_pan(o, fz, up, (const float[3]){0, 0, 10}, 1, lr);
    CHECK(near(lr[0], c, 1e-5f) && near(lr[1], c, 1e-5f), "source straight ahead -> centered 0.707/0.707 (%.3f %.3f)", lr[0], lr[1]);
    audio_ds3d_pan(o, fz, up, (const float[3]){10, 0, 0}, 1, lr);
    CHECK(near(lr[0], sqrtf(0.1f), 1e-5f) && near(lr[1], sqrtf(0.9f), 1e-5f) && near(lr[0] * lr[0] + lr[1] * lr[1], 1, 1e-5f),
          "source fully right -> L 0.316 R 0.949, power sum 1 (%.3f %.3f)", lr[0], lr[1]);
    float r_right = lr[1];
    audio_ds3d_pan(o, fz, up, (const float[3]){-10, 0, 0}, 1, lr);
    CHECK(near(lr[0], r_right, 1e-5f) && near(lr[1], sqrtf(0.1f), 1e-5f), "source on the left -> mirrored (%.3f %.3f)", lr[0], lr[1]);
    audio_ds3d_pan(o, fz, up, (const float[3]){0, 0, -10}, 1, lr);
    CHECK(near(lr[0], c, 1e-5f) && near(lr[1], c, 1e-5f), "source behind -> centered (%.3f %.3f)", lr[0], lr[1]);
    audio_ds3d_pan(o, fz, up, (const float[3]){0.25f, 0, 0}, 1, lr);
    CHECK(near(lr[1], sqrtf(0.6f), 1e-5f) && near(lr[0], sqrtf(0.4f), 1e-5f),
          "source inside the minimum distance -> pan narrowed to p 0.2 (%.3f %.3f)", lr[0], lr[1]);
    audio_ds3d_pan(o, fz, up, o, 1, lr);
    CHECK(near(lr[0], c, 1e-6f) && near(lr[1], c, 1e-6f), "source at the listener -> centered 0.707/0.707");
    int power_ok = 1;
    for (int a = 0; a < 72; a++) {
        float th = (float)a * 0.0872665f;
        audio_ds3d_pan(o, fz, up, (const float[3]){10 * cosf(th), 0, 10 * sinf(th)}, 1, lr);
        power_ok &= near(lr[0] * lr[0] + lr[1] * lr[1], 1, 1e-5f);
    }
    CHECK(power_ok, "power sum L^2 + R^2 = 1 at every pan position (72 directions)");
    /* Turned listener: facing +x, so +z is on the left. */
    audio_ds3d_pan(o, (const float[3]){1, 0, 0}, up, (const float[3]){0, 0, 10}, 1, lr);
    CHECK(lr[0] > 0.9f && lr[1] < 0.4f, "listener facing +x: a source at +z is on the left (%.3f %.3f)", lr[0], lr[1]);

    /* ---- HLE: listener/source setters drive the 3D cue's gains ---- */
    hle_audio_log_limit = 0;
    static uint8_t xwb[4096], xsb[1024];
    uint32_t xwb_len = synth_xwb(xwb, 1000), xsb_len = synth_xsb(xsb);
    for (int i = 0; i < 1000; i++) sy_w16(xwb + 0x80 + 2 * i, 4000);   /* constant level instead of the ramp */
    /* Cue 0 (entry 0, mono PCM, volume -4 dB) becomes 3D with DS3D record 0 (min 1, max 40)
     * and cue 2 (entry 2, looping) stays 2D. The 3D table goes after the bank's last byte. */
    uint32_t t3d = (xsb_len + 3) & ~3u;
    sy_w32(xsb + 0x10, t3d);
    uint8_t *rec = xsb + t3d;
    sy_w16(rec, 360); sy_w16(rec + 2, 360);
    float fl[5] = {1, 40, 1, 1, 1};
    memcpy(rec + 8, fl, sizeof fl);
    uint8_t *e0 = xsb + 0x38 + 3 * 0x14;
    e0[0xb] |= 1;
    sy_w16(e0 + 0xc, 0);
    xsb_len = t3d + 0x28;

    uint32_t params[8] = {0}, eng = 0, bank = 0, wb = 0, src = 0, cue = 0;
    call("XACTEngineCreate", 2, A(P(params), P(&eng)));
    call("IXACTEngine_RegisterWaveBank", 4, A(eng, P(xwb), xwb_len, P(&wb)));
    call("IXACTEngine_CreateSoundBank", 4, A(eng, P(xsb), xsb_len, P(&bank)));
    call("IXACTEngine_CreateSoundSource", 3, A(eng, 2, P(&src)));
    CHECK(eng && bank && wb && src, "engine, banks and source created");

    /* Reference: the 3D cue with its source at the listener. */
    call("IXACTEngine_SetListenerPosition?", 5, A(eng, F(0), F(0), F(0), 1));
    call("IXACTEngine_SetListenerOrientation?", 8, A(eng, F(0), F(0), F(1), F(0), F(1), F(0), 1));
    call("IXACTSoundSource_SetPosition?", 5, A(src, F(0), F(0), F(0), 1));
    call("IXACTEngine_CommitDeferredSettings?", 1, A(eng));
    uint32_t pl[4] = {0, 0, src, 0};
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    double ref[2], got[2];
    level(480, ref);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(ref[0] > 100 && near((float)ref[0], (float)ref[1], 1), "3D cue at the listener: centered, level %.0f/%.0f", ref[0], ref[1]);

    /* The same wave as a 2D cue (cue 2: entry 2, 0 dB vs entry 0's -4 dB) gives the unpanned
     * level: a centered 3D cue is 0.707 (-3 dB) of it per ear, after the entry volume. */
    float vol0 = audio_db100_to_gain(-400);
    uint32_t pl2[4] = {0, 2, src, 0}, cue2 = 0;
    double flat[2];
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl2), P(&cue2)));
    level(480, flat);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(near((float)(ref[0] / flat[0]), vol0 * c, 0.02f) && near((float)(ref[1] / flat[1]), vol0 * c, 0.02f),
          "centered 3D cue vs the 2D cue: x%.3f x%.3f (expected entry volume x 0.707 = x%.3f)", ref[0] / flat[0], ref[1] / flat[1],
          vol0 * c);

    /* --sfx-3d-volume: a linear gain on 3D cues only. */
    hle_audio_set_sfx_3d_volume(-6.0f);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    level(480, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    float g6 = powf(10.0f, -6.0f / 20);
    CHECK(near((float)(got[0] / ref[0]), g6, 0.02f) && near((float)(got[1] / ref[1]), g6, 0.02f),
          "--sfx-3d-volume -6: 3D cue x%.3f x%.3f (expected x%.3f)", got[0] / ref[0], got[1] / ref[1], g6);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl2), P(&cue2)));
    level(480, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(near((float)(got[0] / flat[0]), 1, 0.02f) && near((float)(got[1] / flat[1]), 1, 0.02f),
          "--sfx-3d-volume -6: 2D cue unchanged (x%.3f x%.3f)", got[0] / flat[0], got[1] / flat[1]);
    hle_audio_set_sfx_3d_volume(0.0f);

    /* Player volumes per class: --sfx-volume on effects (2D and 3D), --music-volume on category 1
     * (cue 2's entry made a music sound), --dialogue-volume on neither of them here. */
    {
        uint8_t *e2 = xsb + 0x38 + 3 * 0x14 + 2 * 0x14;
        hle_audio_set_class_volume(HLE_AUDIO_CLASS_SFX, -6.0f);
        call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
        level(480, got);
        call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
        CHECK(near((float)(got[0] / ref[0]), g6, 0.02f), "--sfx-volume -6: 3D effect x%.3f (expected x%.3f)", got[0] / ref[0], g6);
        call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl2), P(&cue2)));
        level(480, got);
        call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
        CHECK(near((float)(got[0] / flat[0]), g6, 0.02f), "--sfx-volume -6: 2D effect x%.3f (expected x%.3f)", got[0] / flat[0], g6);
        hle_audio_set_class_volume(HLE_AUDIO_CLASS_SFX, 0.0f);
        e2[0xa] = 1;
        hle_audio_set_class_volume(HLE_AUDIO_CLASS_MUSIC, -6.0f);
        hle_audio_set_class_volume(HLE_AUDIO_CLASS_DIALOGUE, -20.0f);
        call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl2), P(&cue2)));
        level(480, got);
        call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
        CHECK(near((float)(got[0] / flat[0]), g6, 0.02f), "--music-volume -6: music cue x%.3f (expected x%.3f)", got[0] / flat[0], g6);
        call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
        level(480, got);
        call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
        CHECK(near((float)(got[0] / ref[0]), 1, 0.02f), "--music-volume/--dialogue-volume: effect unchanged (x%.3f)", got[0] / ref[0]);
        hle_audio_set_class_volume(HLE_AUDIO_CLASS_MUSIC, 0.0f);
        hle_audio_set_class_volume(HLE_AUDIO_CLASS_DIALOGUE, 0.0f);
        e2[0xa] = 0;
    }
    /* Dialogue classification: the two name-sorted blocks of the USA bank (23523 cues, header word
     * +6 = 0x62d8); another bank has no dialogue class. */
    {
        static uint8_t hdr[16] = {'S', 'D', 'B', 'K', 11, 0, 0xd8, 0x62};
        XsbBank us = {.d = hdr, .len = sizeof hdr, .cues = 23523}, other = us;
        static uint8_t hdr2[16] = {'S', 'D', 'B', 'K', 11, 0, 0x00, 0x11};
        other.d = hdr2;
        const uint32_t dlg[] = {0x537, 0x2000, 0x4d62, 0x519c, 0x5952}, fx[] = {0, 0x536, 0x4d63, 0x519b, 0x5953, 0x5be1};
        int ok_dlg = 1, ok_fx = 1, ok_other = 1;
        for (unsigned i = 0; i < sizeof dlg / sizeof *dlg; i++) {
            ok_dlg &= hle_audio_cue_class(&us, dlg[i], 0) == HLE_AUDIO_CLASS_DIALOGUE;
            ok_other &= hle_audio_cue_class(&other, dlg[i], 0) == HLE_AUDIO_CLASS_SFX;
        }
        for (unsigned i = 0; i < sizeof fx / sizeof *fx; i++) ok_fx &= hle_audio_cue_class(&us, fx[i], 0) == HLE_AUDIO_CLASS_SFX;
        CHECK(ok_dlg, "dialogue class: voices 0x537-0x4d62 and speeches 0x519c-0x5952");
        CHECK(ok_fx, "effects class: cues outside the two blocks");
        CHECK(ok_other, "another sound bank: no dialogue class");
        CHECK(hle_audio_cue_class(&us, 0x2000, 1) == HLE_AUDIO_CLASS_MUSIC && hle_audio_cue_class(NULL, 5, 1) == HLE_AUDIO_CLASS_MUSIC,
              "music class: category 1");
    }

    /* Source 4 units to the right (DirectSound +x with the listener facing +z). */
    call("IXACTSoundSource_SetPosition?", 5, A(src, F(4), F(0), F(0), 1));
    call("IXACTEngine_CommitDeferredSettings?", 1, A(eng));
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    level(480, got);
    float exp_lr[2];
    audio_ds3d_pan(o, fz, up, (const float[3]){4, 0, 0}, 1, exp_lr);
    /* Relative to the unpanned level (2D level x entry volume): distance 1/4 x constant-power
     * pan, L 0.25 x sqrt(0.1) = 0.079, R 0.25 x sqrt(0.9) = 0.237. */
    CHECK(near((float)(got[0] / (flat[0] * vol0)), 0.25f * exp_lr[0], 0.01f) &&
              near((float)(got[1] / (flat[1] * vol0)), 0.25f * exp_lr[1], 0.01f) && near(exp_lr[0], sqrtf(0.1f), 1e-4f),
          "source at distance 4 on the right: L x%.3f R x%.3f (expected x%.3f x%.3f)", got[0] / (flat[0] * vol0),
          got[1] / (flat[1] * vol0), 0.25f * exp_lr[0], 0.25f * exp_lr[1]);

    /* The listener walks up to the playing source: the next DoWork applies it. */
    call("IXACTEngine_SetListenerPosition?", 5, A(eng, F(4), F(0), F(-1), 1));
    call("IXACTEngine_CommitDeferredSettings?", 1, A(eng));
    call("XACTEngineDoWork", 0, A(0));
    level(480, got);
    CHECK(near((float)(got[0] / ref[0]), 1, 0.05f) && near((float)(got[1] / ref[1]), 1, 0.05f),
          "listener moved to 1 unit from the source (straight ahead): back to full level (x%.3f x%.3f)", got[0] / ref[0], got[1] / ref[1]);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));

    /* Far beyond the max distance: min/max = 1/40. */
    call("IXACTEngine_SetListenerPosition?", 5, A(eng, F(0), F(0), F(0), 1));
    call("IXACTSoundSource_SetPosition?", 5, A(src, F(0), F(0), F(500), 1));
    call("IXACTEngine_CommitDeferredSettings?", 1, A(eng));
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    level(480, got);
    CHECK(near((float)(got[0] / ref[0]), 1.0f / 40, 0.01f) && near((float)(got[1] / ref[1]), 1.0f / 40, 0.01f),
          "source beyond max distance: x%.4f x%.4f (expected 1/40)", got[0] / ref[0], got[1] / ref[1]);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));

    /* A 2D cue on the same far source is not attenuated (cue 2: entry 2, same wave, 0 dB vs
     * entry 0's -4 dB). */
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl2), P(&cue2)));
    level(480, got);
    CHECK(near((float)(got[0] / ref[0]), 1 / (vol0 * c), 0.03f) && near((float)(got[1] / ref[1]), 1 / (vol0 * c), 0.03f),
          "2D cue on a far source keeps its entry volume (x%.3f x%.3f, expected x%.3f)", got[0] / ref[0], got[1] / ref[1],
          1 / (vol0 * c));
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));

    /* A 3D cue played without a source is heard as if at the listener. */
    uint32_t pl0[4] = {0, 0, 0, 0};
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl0), P(&cue)));
    level(480, got);
    CHECK(near((float)(got[0] / ref[0]), 1, 0.03f) && near((float)(got[1] / ref[1]), 1, 0.03f),
          "3D cue without a source: unattenuated (x%.3f x%.3f)", got[0] / ref[0], got[1] / ref[1]);

    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));

    /* The record's rolloff curve (count +0x1d, points +0x1e): a source 10 units away, min 1 max 40,
     * follows the curve; --no-rolloff-curves keeps the inverse distance law (1/10). */
    {
        static const uint8_t pts[10] = {206, 163, 124, 91, 63, 40, 22, 10, 2, 0};
        float cv[10];
        for (int k = 0; k < 10; k++) cv[k] = pts[k] / 255.0f;
        rec[0x1d] = 10;
        memcpy(rec + 0x1e, pts, 10);
        call("IXACTSoundSource_SetPosition?", 5, A(src, F(0), F(0), F(10), 1));
        call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
        level(480, got);
        call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
        float want = audio_ds3d_curve_gain(10, 1, 40, cv, 10, 1);
        CHECK(near((float)(got[0] / ref[0]), want, 0.01f) && near((float)(got[1] / ref[1]), want, 0.01f),
              "3D cue with the rolloff curve at 10 units: x%.3f x%.3f (expected x%.3f, %.1f dB)", got[0] / ref[0], got[1] / ref[1], want,
              20 * log10f(want));
        call("IXACTSoundSource_SetPosition?", 5, A(src, F(0), F(0), F(45), 1));
        call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
        level(480, got);
        call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
        CHECK(got[0] == 0 && got[1] == 0, "3D cue with the rolloff curve beyond max: silent (%.1f %.1f)", got[0], got[1]);
        hle_audio_set_rolloff_curves(0);
        call("IXACTSoundSource_SetPosition?", 5, A(src, F(0), F(0), F(10), 1));
        call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
        level(480, got);
        call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
        CHECK(near((float)(got[0] / ref[0]), 0.1f, 0.01f) && near((float)(got[1] / ref[1]), 0.1f, 0.01f),
              "--no-rolloff-curves: inverse distance at 10 units (x%.3f x%.3f)", got[0] / ref[0], got[1] / ref[1]);
        hle_audio_set_rolloff_curves(1);
        rec[0x1d] = 0;
        memset(rec + 0x1e, 0, 10);
        call("IXACTSoundSource_SetPosition?", 5, A(src, F(0), F(0), F(0), 1));
    }

    /* ---- I3DL2 reverb ---- */
    /* The listener the title sets (logged in a level 1 run): room -1000, roomHF -800, rolloff 0,
     * decay 1.49 s, HF ratio 0.67, reflections -2273 (0.007 s), reverb -2217 (0.011 s),
     * diffusion 50, density 100, HF reference 5000 Hz. */
    uint32_t lis[12] = {(uint32_t)-1000, (uint32_t)-800, F(0), F(1.49f), F(0.67f), (uint32_t)-2273, F(0.007f),
                        (uint32_t)-2217, F(0.011f), F(50), F(100), F(5000)};
    double tail[2];
    level(48000, tail);   /* drain */
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    level(4800, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    level(2400, tail);
    CHECK(tail[0] == 0 && tail[1] == 0, "default listener (no room): no tail after a 3D cue (%.1f %.1f)", tail[0], tail[1]);
    call("IXACTEngine_SetI3DL2Listener?", 3, A(eng, P(lis), 0));
    ReverbI3dl2 rp;
    audio_get_i3dl2(&rp);
    CHECK(rp.room == -1000 && rp.room_hf == -800 && near(rp.decay_time, 1.49f, 1e-6f) && near(rp.decay_hf_ratio, 0.67f, 1e-6f) &&
              rp.reflections == -2273 && near(rp.reflections_delay, 0.007f, 1e-6f) && rp.reverb == -2217 &&
              near(rp.reverb_delay, 0.011f, 1e-6f) && near(rp.diffusion, 50, 1e-6f) && near(rp.density, 100, 1e-6f) &&
              near(rp.hf_reference, 5000, 1e-3f),
          "SetI3DL2Listener: the mixer's reverb has the title's 12 fields");
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    level(4800, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    level(2400, tail);
    CHECK(tail[0] > 1 && tail[1] > 1, "3D cue in the room: a reverb tail after it stops (%.1f %.1f, cue %.0f)", tail[0], tail[1], got[0]);
    level(4 * 48000, tail);   /* let the tail die (1.49 s) */
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl2), P(&cue2)));
    level(4800, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    level(2400, tail);
    CHECK(got[0] > 100 && tail[0] == 0 && tail[1] == 0, "2D cue: dry, no tail (%.1f %.1f)", tail[0], tail[1]);
    audio_set_reverb(0);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    level(4800, got);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    level(2400, tail);
    CHECK(tail[0] == 0 && tail[1] == 0, "--no-reverb: no tail after a 3D cue (%.1f %.1f)", tail[0], tail[1]);
    audio_set_reverb(1);

    /* ---- Doppler math ---- */
    {
        const float C = 343, zero[3] = {0, 0, 0}, src10[3] = {0, 0, 10};
        float r = audio_doppler_ratio(zero, zero, src10, (const float[3]){0, 0, -34.3f}, C, 2);
        CHECK(near(r, 343.0f / (343 - 34.3f), 1e-4f), "source approaching at c/10: ratio %.4f (expected %.4f)", r, 343.0f / (343 - 34.3f));
        r = audio_doppler_ratio(zero, zero, src10, (const float[3]){0, 0, 34.3f}, C, 2);
        CHECK(near(r, 343.0f / (343 + 34.3f), 1e-4f), "source receding at c/10: ratio %.4f (expected %.4f)", r, 343.0f / (343 + 34.3f));
        r = audio_doppler_ratio(zero, (const float[3]){0, 0, 34.3f}, src10, zero, C, 2);
        CHECK(near(r, (343 + 34.3f) / 343.0f, 1e-4f), "listener approaching at c/10: ratio %.4f (expected %.4f)", r, 1.1f);
        r = audio_doppler_ratio(zero, (const float[3]){0, 0, 20}, src10, (const float[3]){0, 0, 20}, C, 2);
        CHECK(near(r, 1, 1e-5f), "listener and source moving together: ratio %.4f", r);
        r = audio_doppler_ratio(zero, zero, src10, (const float[3]){30, 0, 0}, C, 2);
        CHECK(near(r, 1, 1e-5f), "source moving across the line of sight: ratio %.4f", r);
        r = audio_doppler_ratio(zero, zero, src10, (const float[3]){0, 0, -1000}, C, 2);
        CHECK(near(r, 2, 1e-5f), "supersonic approach: projected speed clamped to c/2, ratio %.4f (expected 2)", r);
        r = audio_doppler_ratio(zero, zero, src10, (const float[3]){0, 0, -100}, C, 1.2f);
        CHECK(near(r, 1.2f, 1e-5f), "ratio clamped to max_ratio 1.2 (%.4f)", r);
        r = audio_doppler_ratio(zero, zero, src10, (const float[3]){0, 0, 1000}, C, 1.2f);
        CHECK(near(r, 1 / 1.2f, 1e-5f), "ratio clamped to 1/max_ratio (%.4f)", r);
        r = audio_doppler_ratio(src10, zero, src10, (const float[3]){0, 0, -50}, C, 2);
        CHECK(near(r, 1, 1e-6f), "source at the listener: ratio 1 (%.4f)", r);

        /* velocity estimate */
        AudioVelocity e = {0};
        float p[3] = {0, 0, 40};
        audio_velocity_update(&e, p, 0.0, 100);
        CHECK(e.have && near(e.vel[2], 0, 1e-6f), "first position: velocity 0");
        for (int k = 1; k <= 60; k++) {   /* 1 s at 60 Hz, -34.3 units/s along z, a duplicate update per frame */
            p[2] = 40 - 34.3f * k / 60.0f;
            audio_velocity_update(&e, p, k / 60.0, 100);
            audio_velocity_update(&e, p, k / 60.0 + 1e-5, 100);
        }
        CHECK(near(e.vel[2], -34.3f, 0.05f) && near(e.vel[0], 0, 1e-4f), "constant motion: velocity %.3f units/s (expected -34.3)", e.vel[2]);
        float half = 0;
        AudioVelocity e2 = {0};
        float q[3] = {0, 0, 0};
        audio_velocity_update(&e2, q, 0.0, 100);
        for (int k = 1; k <= 6; k++) { q[0] = 10.0f * k / 60.0f; audio_velocity_update(&e2, q, k / 60.0, 100); }
        half = e2.vel[0];
        CHECK(half > 4 && half < 8, "smoothed: 0.1 s after a step to 10 units/s the estimate is %.2f (about 6.3)", half);
        p[2] += 50;   /* a 50-unit jump in one frame: 3000 units/s */
        audio_velocity_update(&e, p, 61 / 60.0, 100);
        CHECK(near(e.vel[2], 0, 1e-6f), "teleport: velocity restarts at 0 (%.3f)", e.vel[2]);
        p[2] -= 0.5f;
        audio_velocity_update(&e, p, 61 / 60.0 + 1.0, 100);
        CHECK(near(e.vel[2], 0, 1e-6f), "an update after a 1 s gap: velocity restarts at 0 (%.3f)", e.vel[2]);

        /* rear cue */
        float a5k = 1 - expf(-2 * 3.14159265f * 5000 / 48000);
        float lf = audio_rear_lowpass(o, fz, (const float[3]){0, 0, 10}, 1);
        float ls = audio_rear_lowpass(o, fz, (const float[3]){10, 0, 0}, 1);
        float lb = audio_rear_lowpass(o, fz, (const float[3]){0, 0, -10}, 1);
        float l45 = audio_rear_lowpass(o, fz, (const float[3]){7, 0, -7}, 1);
        float lin = audio_rear_lowpass(o, fz, (const float[3]){0, 0, -0.5f}, 1);
        CHECK(lf == 1 && ls == 1, "front and side: no rear low-pass (%.3f %.3f)", lf, ls);
        CHECK(near(lb, a5k, 1e-4f) && lp_gain(lb, 10000) < 0.63 && lp_gain(lb, 500) > 0.99,
              "straight behind: 5 kHz one-pole (a %.3f), %.1f dB at 10 kHz, %.2f dB at 500 Hz", lb, 20 * log10(lp_gain(lb, 10000)),
              20 * log10(lp_gain(lb, 500)));
        CHECK(l45 < 1 && l45 > lb, "45 degrees behind: in between (%.3f)", l45);
        CHECK(lin > lb && lin < 1, "behind inside the min distance: weaker (%.3f)", lin);
    }

    /* ---- Doppler end to end: a 1 kHz tone on cue 0 (3D) and cue 2 (2D) ---- */
    for (int i = 0; i < 1000; i++) sy_w16(xwb + 0x80 + 2 * i, (uint16_t)(int16_t)lrint(8000 * sin(2 * 3.14159265358979 * i / 24.0)));
    uint32_t dry[12] = {(uint32_t)-10000, 0, F(0), F(1), F(0.5f), (uint32_t)-10000, F(0.02f), (uint32_t)-10000, F(0.04f), F(100), F(100), F(5000)};
    call("IXACTEngine_SetI3DL2Listener?", 3, A(eng, P(dry), 0));   /* dry: the tone is measured alone */
    call("IXACTEngine_SetListenerPosition?", 5, A(eng, F(0), F(0), F(0), 1));
    call("IXACTSoundSource_SetPosition?", 5, A(src, F(0), F(0), F(10), 1));
    level(48000, got);
    double f0 = 1000.0 / 48000;
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    double f = tone(2400, 100, 1500);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(near((float)(f / f0), 1, 0.002f), "3D cue on a still source: 1 kHz (x%.4f)", f / f0);
    call("IXACTSoundSource_SetVelocity?", 5, A(src, F(0), F(0), F(-34.3f), 1));
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    f = tone(2400, 100, 1500);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    double want = 343.0 / (343 - 34.3);
    CHECK(near((float)(f / f0), (float)want, 0.003f), "title velocity: source approaching at c/10 -> x%.4f (expected x%.4f)", f / f0, want);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl2), P(&cue2)));
    f = tone(2400, 100, 1500);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(near((float)(f / f0), 1, 0.002f), "2D cue on the moving source: no Doppler (x%.4f)", f / f0);
    hle_audio_set_doppler(0);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    f = tone(2400, 100, 1500);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(near((float)(f / f0), 1, 0.002f), "--no-doppler: 3D cue unchanged (x%.4f)", f / f0);
    hle_audio_set_doppler(1);
    call("IXACTSoundSource_SetVelocity?", 5, A(src, F(0), F(0), F(0), 1));

    /* A source the title never gives a velocity: estimated from its positions on the title clock. */
    uint32_t src2 = 0;
    call("IXACTEngine_CreateSoundSource", 3, A(eng, 2, P(&src2)));
    hle_audio_clock = fake_clock;
    for (int k = 0; k <= 60; k++) {
        fake_now = k / 60.0;
        call("IXACTSoundSource_SetPosition?", 5, A(src2, F(0), F(0), F(40 - 34.3f * k / 60.0f), 1));
        call("XACTEngineDoWork", 0, A(0));
    }
    uint32_t pl3[4] = {0, 0, src2, 0};
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl3), P(&cue)));
    f = tone(2400, 100, 1500);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    CHECK(near((float)(f / f0), (float)want, 0.005f), "estimated velocity: source approaching at c/10 -> x%.4f (expected x%.4f)", f / f0, want);
    /* The listener walks towards a still source (no listener velocity from the title). */
    call("IXACTSoundSource_SetPosition?", 5, A(src2, F(0), F(0), F(40), 1));
    for (int k = 0; k <= 60; k++) {   /* restart the source estimate at rest, the listener at rest too */
        fake_now = 2 + k / 60.0;
        call("IXACTSoundSource_SetPosition?", 5, A(src2, F(0), F(0), F(40), 1));
        call("IXACTEngine_SetListenerPosition?", 5, A(eng, F(0), F(0), F(0), 1));
        call("XACTEngineDoWork", 0, A(0));
    }
    for (int k = 1; k <= 60; k++) {
        fake_now = 3 + k / 60.0;
        call("IXACTSoundSource_SetPosition?", 5, A(src2, F(0), F(0), F(40), 1));
        call("IXACTEngine_SetListenerPosition?", 5, A(eng, F(0), F(0), F(34.3f * k / 60.0f), 1));
        call("XACTEngineDoWork", 0, A(0));
    }
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl3), P(&cue)));
    f = tone(2400, 100, 1500);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    want = (343 + 34.3) / 343.0;
    CHECK(near((float)(f / f0), (float)want, 0.005f), "estimated listener velocity: approaching at c/10 -> x%.4f (expected x%.4f)", f / f0, want);
    hle_audio_clock = NULL;
    call("IXACTEngine_SetListenerPosition?", 5, A(eng, F(0), F(0), F(0), 1));

    test_audio3d_extras(eng, bank, src, xwb);

    printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
