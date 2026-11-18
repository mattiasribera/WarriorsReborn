/* Game-independent --dynres test (src/dynres.c): option parsing, the render size, and the controller
 * against a GPU model (cost = fixed + per-pixel * scale^2, with noise and spikes): it converges below
 * the target, stays at the ceiling when there is headroom, reacts to a heavier scene within a few
 * frames, does not oscillate in a steady scene and never leaves its bounds. */
#include "../src/dynres.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_parse(void) {
    float v = -1;
    CHECK(dynres_options.on == 0 && dynres_options.target_ms == 0 && dynres_options.min_ratio == DYNRES_MIN_RATIO_DEFAULT,
          "defaults: off, auto target, min ratio %.1f", DYNRES_MIN_RATIO_DEFAULT);
    CHECK(dynres_parse_target("auto", &v) == 0 && v == 0, "--dynres auto parses (0)");
    CHECK(dynres_parse_target("14.5", &v) == 0 && fabsf(v - 14.5f) < 1e-6f, "--dynres 14.5 parses (%.2f)", v);
    CHECK(dynres_parse_target("16", &v) == 0 && v == 16, "--dynres 16 parses");
    static const char *bad[] = {"", "3.9", "101", "abc", "14ms", "-5", "nan", "inf", "Auto"};
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        v = 7;
        CHECK(dynres_parse_target(bad[k], &v) == -1 && v == 7, "--dynres \"%s\" is rejected and leaves the value", bad[k]);
    }
    CHECK(dynres_parse_min("2", &v) == 0 && v == 2 && dynres_parse_min("1.5", &v) == 0 && v == 1.5f, "--dynres-min 2 and 1.5 parse");
    CHECK(dynres_parse_min("1.0", &v) == -1 && dynres_parse_min("3.5", &v) == -1 && dynres_parse_min("x", &v) == -1,
          "--dynres-min 1.0, 3.5, x are rejected");
    CHECK(dynres_target(0) == DYNRES_AUTO_MS && dynres_target(12) == 12, "auto stands for %.1f ms", DYNRES_AUTO_MS);
}

static void test_size(void) {
    int w = 0, h = 0;
    dynres_size(1 / 1.3f, 1920, 1080, 1477, 831, &w, &h);
    CHECK(w == 1477 && h == 831, "scale 1/1.3 at 1080p: 1477x831 (%dx%d)", w, h);
    dynres_size(0.5f, 1920, 1080, 1477, 831, &w, &h);
    CHECK(w == 960 && h == 540, "scale 0.5: 960x540 (%dx%d)", w, h);
    dynres_size(1.0f, 1920, 1080, 1477, 831, &w, &h);
    CHECK(w == 1477 && h == 831, "never above the allocated rectangle (%dx%d)", w, h);
    dynres_size(0.0001f, 1920, 1080, 1477, 831, &w, &h);
    CHECK(w >= 1 && h >= 1, "at least 1x1 (%dx%d)", w, h);
}

/* Model GPU: fixed + var * (scale / ceiling)^2 with deterministic noise. */
static unsigned rng = 12345;
static float noise(float amp) {
    rng = rng * 1103515245u + 12345u;
    return amp * (((rng >> 8) & 0xffff) / 32768.0f - 1.0f);
}
/* Runs n frames with a two-frame timestamp delay; returns the mean GPU ms of the last `tail`. */
static float run(Dynres *d, int n, float fixed, float var, float amp, int tail, float *worst, unsigned *changes_tail) {
    float q[2] = {-1, -1}, sum = 0;
    unsigned c0 = 0;
    *worst = 0;
    for (int f = 0; f < n; f++) {
        const float s = d->scale / d->scale_max;
        const float ms = fixed + var * s * s + noise(amp);
        dynres_update(d, q[0]);
        q[0] = q[1];
        q[1] = ms;
        if (f == n - tail) c0 = d->changes;
        if (f >= n - tail) {
            sum += ms;
            if (ms > *worst) *worst = ms;
        }
        if (d->scale < d->scale_min - 1e-6f || d->scale > d->scale_max + 1e-6f) return -1;
    }
    *changes_tail = d->changes - c0;
    return sum / tail;
}

static void test_controller(void) {
    Dynres d;
    float worst;
    unsigned ch;
    dynres_init(&d, 14, 0.5f, 1 / 1.3f);
    CHECK(fabsf(d.scale - 1 / 1.3f) < 1e-6f, "starts at the ceiling (%.3f)", d.scale);
    /* Light scene: 9 ms at the ceiling: no change. */
    float m = run(&d, 600, 3, 6, 0.4f, 300, &worst, &ch);
    CHECK(fabsf(d.scale - d.scale_max) < 1e-6f && d.changes == 0, "headroom: stays at the ceiling (%.3f, %u changes, %.2f ms)", d.scale,
          d.changes, m);
    /* RTX 2050-like: 20 ms at the ceiling (4 fixed + 16 per pixel). */
    dynres_init(&d, 14, 0.5f, 1 / 1.3f);
    m = run(&d, 1200, 4, 16, 0.6f, 600, &worst, &ch);
    CHECK(m > 0 && m <= 14.0f && m >= 11.0f, "heavy scene: settles below the 14 ms target, close to it (%.2f ms, scale %.3f)", m, d.scale);
    CHECK(ch <= 6, "steady: at most 6 changes in the last 600 frames (%u)", ch);
    /* Too heavy even at the floor: stays at the floor. */
    dynres_init(&d, 14, 0.5f, 1 / 1.3f);
    m = run(&d, 600, 12, 30, 0.5f, 300, &worst, &ch);
    CHECK(fabsf(d.scale - 0.5f) < 1e-6f, "over budget at the floor: holds the floor (%.3f, %.2f ms)", d.scale, m);
    /* A heavier scene appears: back under the target within 12 frames. */
    dynres_init(&d, 14, 0.5f, 1 / 1.3f);
    run(&d, 300, 3, 6, 0.3f, 100, &worst, &ch);
    float q[2] = {-1, -1};
    int back = -1;
    for (int f = 0; f < 120; f++) {
        const float s = d.scale / d.scale_max, ms = 3 + 15 * s * s;
        dynres_update(&d, q[0]);
        q[0] = q[1];
        q[1] = ms;
        if (back < 0 && f > 0 && ms <= 14.0f) back = f;
    }
    CHECK(back >= 0 && back <= 12, "load jump 18 ms: under 14 ms after %d frames", back);
    /* The load goes away again: back to the ceiling. */
    run(&d, 1200, 3, 6, 0.3f, 100, &worst, &ch);
    CHECK(fabsf(d.scale - d.scale_max) < 1e-6f, "load gone: back at the ceiling (%.3f)", d.scale);
    /* A small overload (15 ms for a 14 ms target) steps down by at most 12 %, not to the floor. */
    dynres_init(&d, 14, 0.5f, 1 / 1.3f);
    for (int f = 0; f < 20 && d.changes == 0; f++) dynres_update(&d, 15);
    CHECK(d.changes == 1 && d.scale >= 0.88f * d.scale_max - 1e-5f, "small overload: one step of at most 12 %% (%.3f of %.3f)", d.scale, d.scale_max);
    /* A load that reads light at the lower size and heavy one step up (a GPU changing its clock with
     * the load): a rise that is undone soon waits longer before the next one, so it does not flap. */
    dynres_init(&d, 14, 0.5f, 1 / 1.3f);
    float lq[2] = {-1, -1};
    for (int f = 0; f < 2400; f++) {
        const float ms = d.scale > 0.62f ? 15.5f : 10.0f;
        dynres_update(&d, lq[0]);
        lq[0] = lq[1];
        lq[1] = ms;
    }
    /* at most one rise and its drop per 16 x DYNRES_UP_FRAMES frames once backed off (10 in 2400) plus the
     * first steps; 97 without the back-off */
    CHECK(d.changes <= 20, "clock-driven flapping: at most 20 changes in 2400 frames (%u)", d.changes);
    /* Missing timestamps change nothing. */
    dynres_init(&d, 14, 0.5f, 1 / 1.3f);
    int moved = 0;
    for (int f = 0; f < 100; f++) moved |= dynres_update(&d, -1) | dynres_update(&d, 0);
    CHECK(!moved && d.scale == d.scale_max, "no timestamps: no change");
    /* A single spike does not drop the size. */
    dynres_init(&d, 14, 0.5f, 1 / 1.3f);
    for (int f = 0; f < 60; f++) dynres_update(&d, 9);
    dynres_update(&d, 40);
    for (int f = 0; f < 10; f++) dynres_update(&d, 9);
    CHECK(d.scale == d.scale_max, "one 40 ms spike among 9 ms frames: no change (%.3f)", d.scale);
}

int main(void) {
    test_parse();
    test_size();
    test_controller();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
