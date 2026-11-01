/* Game-independent test of XACT 3D sound positioning (built by runtime/Makefile as
 * build/audio3d_test, run by tests/test_audio3d.py).
 *
 * 1. The DirectSound 3D distance model and stereo pan helpers in src/audio.c.
 * 2. The HLE setters in src/hle_audio.c, end to end: a synthetic sound bank whose cue 0 is a 3D
 *    sound (DS3D record min 1, max 40) plays on a sound source; the listener and source setters
 *    are called through hle_audio_dispatch with the title's argument layout, and the mixer
 *    (audio_mix, called directly: no mixer thread) must attenuate and pan the cue. 2D cues stay
 *    at their entry volume. */
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

    printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
