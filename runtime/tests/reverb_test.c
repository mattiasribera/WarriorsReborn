/* Game-independent I3DL2 reverb test (src/reverb.c): impulse responses of the reverb for I3DL2
 * listener settings, checked against the parameters. Prints "ALL OK" or FAIL lines and the CPU
 * cost per 5 ms chunk.
 *   - decay: RT60 (Schroeder integral, T30 fit) in a narrow band at 500 Hz within 12% of
 *     flDecayTime (I3DL2's decay time is the low-frequency one; the broadband RT60 of a white
 *     impulse is shorter because the HF part decays faster, and is only printed);
 *   - HF ratio: RT60 in a narrow band at flHFReference over RT60 at 500 Hz within 20% of
 *     flDecayHFRatio (a ratio above 1 is clamped to 1). The bands are 4 cascaded band-passes
 *     (Q 3), so the slower low-frequency tail does not leak into the HF band's decay curve;
 *   - levels: lReverb, lReflections and lRoomHF move the late, early and HF energy by their mB;
 *   - lRoom -10000 (the XDK default) is silent; extreme settings stay finite; the ears are
 *     decorrelated. */
#define _GNU_SOURCE
#include "../src/reverb.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
static const double PI = 3.14159265358979323846;
#define FS REVERB_RATE

static Reverb rv;

/* Stereo impulse response (interleaved, n frames) of an impulse of amplitude 1 at frame 0. */
static float *impulse_response(const ReverbI3dl2 *p, int n) {
    float *h = calloc((size_t)n * 2, sizeof *h), in[240];
    reverb_init(&rv);
    reverb_set_i3dl2(&rv, p);
    for (int done = 0; done < n; done += 240) {
        int m = n - done < 240 ? n - done : 240;
        memset(in, 0, sizeof in);
        if (!done) in[0] = 1.0f;
        reverb_process(&rv, in, h + done * 2, m);
    }
    return h;
}

/* Four cascaded RBJ band-passes (0 dB peak) at f0, quality q, over channel c of h into y. */
static void bandpass(const float *h, int c, int n, double f0, double q, double *y) {
    double w = 2 * PI * f0 / FS, al = sin(w) / (2 * q), a0 = 1 + al;
    double b0 = al / a0, b2 = -al / a0, a1 = -2 * cos(w) / a0, a2 = (1 - al) / a0;
    for (int i = 0; i < n; i++) y[i] = h[i * 2 + c];
    for (int s = 0; s < 4; s++) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int i = 0; i < n; i++) {
            double x = y[i], v = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = v;
            y[i] = v;
        }
    }
}

/* RT60 from the energy decay curve of e[0..n) (energy per frame): least-squares slope of the
 * Schroeder integral between -5 and -35 dB. Returns 0 when the curve never reaches -35 dB. */
static double rt60(const double *e, int n) {
    double *edc = malloc(sizeof *edc * n), acc = 0;
    for (int i = n - 1; i >= 0; i--) { acc += e[i]; edc[i] = acc; }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0, reached = 0;
    for (int i = 0; i < n; i++) {
        double db = 10 * log10(edc[i] / edc[0] + 1e-30);
        if (db > -5) continue;
        if (db < -35) { reached = 1; break; }
        double t = (double)i / FS;
        sx += t; sy += db; sxx += t * t; sxy += t * db; cnt++;
    }
    free(edc);
    if (!reached || cnt < 2) return 0;
    double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);   /* dB per second */
    return slope < 0 ? -60.0 / slope : 0;
}

static double broadband_rt60(const float *h, int n) {
    double *e = malloc(sizeof *e * n);
    for (int i = 0; i < n; i++) e[i] = (double)h[i * 2] * h[i * 2] + (double)h[i * 2 + 1] * h[i * 2 + 1];
    double t = rt60(e, n);
    free(e);
    return t;
}

static double band_rt60(const float *h, int n, double f0) {
    double *yl = malloc(sizeof *yl * n), *yr = malloc(sizeof *yr * n);
    bandpass(h, 0, n, f0, 3.0, yl);
    bandpass(h, 1, n, f0, 3.0, yr);
    for (int i = 0; i < n; i++) yl[i] = yl[i] * yl[i] + yr[i] * yr[i];
    double t = rt60(yl, n);
    free(yl);
    free(yr);
    return t;
}

/* Energy of both ears over frames [a, b). */
static double energy(const float *h, int a, int b) {
    double e = 0;
    for (int i = a * 2; i < b * 2; i++) e += (double)h[i] * h[i];
    return e;
}
static double band_energy(const float *h, int n, double f0) {
    double *y = malloc(sizeof *y * n), e = 0;
    for (int c = 0; c < 2; c++) {
        bandpass(h, c, n, f0, 1.41, y);
        for (int i = 0; i < n; i++) e += y[i] * y[i];
    }
    free(y);
    return e;
}
static double db(double ratio) { return 10 * log10(ratio + 1e-30); }

/* XDK-style presets (DSI3DL2_ENVIRONMENT_PRESET_*): City, Hangar, Alley-like short room. */
static const ReverbI3dl2 CITY = {-1000, -800, 0.0f, 1.49f, 0.67f, -2273, 0.007f, -1691, 0.011f, 50.0f, 100.0f, 5000.0f};
static const ReverbI3dl2 HANGAR = {-1000, -100, 0.0f, 10.05f, 0.23f, -602, 0.020f, 198, 0.030f, 100.0f, 100.0f, 5000.0f};
static const ReverbI3dl2 ROOM = {-1000, 0, 0.0f, 0.90f, 0.40f, -500, 0.010f, 0, 0.020f, 100.0f, 100.0f, 5000.0f};

static void check_decay(const char *name, ReverbI3dl2 p) {
    int n = (int)((p.decay_time * 1.6 + 0.3) * FS);
    float *h = impulse_response(&p, n);
    double tb = broadband_rt60(h, n), tl = band_rt60(h, n, 500), th = band_rt60(h, n, p.hf_reference);
    double want = p.decay_hf_ratio > 1 ? 1 : p.decay_hf_ratio;
    CHECK(fabs(tl / p.decay_time - 1) < 0.12, "%s: RT60 %.3f s at 500 Hz for decay %.2f s (broadband %.3f s)", name, tl,
          p.decay_time, tb);
    CHECK(tl > 0 && th > 0 && fabs(th / tl / want - 1) < 0.20, "%s: RT60 %.3f s at %.0f Hz / %.3f s at 500 Hz = %.3f for HF ratio %.2f",
          name, th, p.hf_reference, tl, tl > 0 ? th / tl : 0, p.decay_hf_ratio);
    free(h);
}

int main(void) {
    check_decay("room", ROOM);
    check_decay("city", CITY);
    ReverbI3dl2 hr = ROOM;
    hr.decay_time = 2.5f; hr.decay_hf_ratio = 0.83f; hr.hf_reference = 3000.0f;
    check_decay("room 2.5 s / 0.83 at 3 kHz", hr);
    ReverbI3dl2 lo = ROOM;
    lo.density = 20.0f; lo.diffusion = 20.0f;
    check_decay("room density/diffusion 20", lo);

    /* levels */
    int n = (int)(1.8 * FS);
    ReverbI3dl2 p = ROOM;
    float *a = impulse_response(&p, n);
    p.reverb = -600;
    float *b = impulse_response(&p, n);
    int late0 = (int)((ROOM.reflections_delay + ROOM.reverb_delay) * FS) + 2400;   /* well into the late part */
    double d = db(energy(b, late0, n) / energy(a, late0, n));
    CHECK(fabs(d + 6.0) < 0.5, "lReverb -600 mB: late energy %+.2f dB", d);
    free(b);
    p = ROOM;
    p.reflections = -1500;
    b = impulse_response(&p, n);
    int er1 = (int)((ROOM.reflections_delay + ROOM.reverb_delay) * FS);
    double ea = energy(a, 0, er1), eb = energy(b, 0, er1);
    d = db(eb / ea);
    CHECK(ea > 0 && fabs(d + 10.0) < 0.5, "lReflections -1500 mB vs -500: early energy %+.2f dB", d);
    double le = db(energy(b, late0, n) / energy(a, late0, n));
    CHECK(fabs(le) < 0.3, "lReflections does not change the late energy (%+.2f dB)", le);
    free(b);
    p = ROOM;
    p.room = -2000;
    b = impulse_response(&p, n);
    d = db(energy(b, 0, n) / energy(a, 0, n));
    CHECK(fabs(d + 10.0) < 0.3, "lRoom -2000 vs -1000 mB: total energy %+.2f dB", d);
    free(b);
    p = ROOM;
    p.room_hf = -1200;
    b = impulse_response(&p, n);
    double dh = db(band_energy(b, n, 5000) / band_energy(a, n, 5000)), dl = db(band_energy(b, n, 200) / band_energy(a, n, 200));
    CHECK(fabs(dh + 12.0) < 2.0 && fabs(dl) < 1.0, "lRoomHF -1200 mB: %+.2f dB at 5 kHz, %+.2f dB at 200 Hz", dh, dl);
    free(b);
    /* decorrelated ears in the late part */
    double lr = 0, ll = 0, rr = 0;
    for (int i = late0; i < n; i++) { lr += (double)a[i * 2] * a[i * 2 + 1]; ll += (double)a[i * 2] * a[i * 2]; rr += (double)a[i * 2 + 1] * a[i * 2 + 1]; }
    double corr = lr / sqrt(ll * rr + 1e-30);
    CHECK(ll > 0 && fabs(corr) < 0.3, "late L/R correlation %.3f", corr);
    free(a);

    /* the XDK default (lRoom -10000) is silent; reverb_active follows lRoom */
    reverb_i3dl2_default(&p);
    a = impulse_response(&p, FS / 2);
    double peak = 0;
    for (int i = 0; i < FS; i++) peak = fmax(peak, fabs(a[i]));
    CHECK(peak == 0 && !reverb_active(&rv), "default listener (lRoom -10000): silent, inactive (peak %g)", peak);
    free(a);
    p = ROOM;
    reverb_init(&rv);
    reverb_set_i3dl2(&rv, &p);
    CHECK(reverb_active(&rv), "a room above -10000 mB is active");

    /* extremes stay finite and bounded */
    p = HANGAR;
    p.decay_time = 20.0f; p.decay_hf_ratio = 2.0f; p.reverb = 2000; p.reflections = 1000; p.room = 0;
    n = 6 * FS;
    a = impulse_response(&p, n);
    peak = 0;
    int finite = 1;
    for (int i = 0; i < n * 2; i++) { if (!isfinite(a[i])) finite = 0; peak = fmax(peak, fabs(a[i])); }
    double tail = sqrt(energy(a, n - FS / 10, n) / (FS / 10 * 2)), head = sqrt(energy(a, FS / 2, FS / 2 + FS / 10) / (FS / 10 * 2));
    CHECK(finite && peak < 20 && tail < head, "decay 20 s, HF ratio 2, max levels: finite, peak %.2f, decaying (%.4f -> %.4f rms)",
          peak, head, tail);
    free(a);
    check_decay("hangar 10 s", HANGAR);

    /* CPU cost: 10 s of a noise send through the City room */
    reverb_init(&rv);
    reverb_set_i3dl2(&rv, &CITY);
    float in[240], out[480];
    uint32_t seed = 1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int chunks = 10 * FS / 240;
    for (int k = 0; k < chunks; k++) {
        for (int i = 0; i < 240; i++) { seed = seed * 1664525u + 1013904223u; in[i] = (float)((int32_t)seed >> 16); }
        memset(out, 0, sizeof out);
        reverb_process(&rv, in, out, 240);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / chunks;
    printf("cost: %.2f us per 5 ms chunk (%.3f%% of a core)\n", us, us / 50);
    CHECK(us / 50 < 2.0, "reverb CPU cost below 2%% of a core");

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
