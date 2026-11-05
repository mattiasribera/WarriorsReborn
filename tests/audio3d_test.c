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

    printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
