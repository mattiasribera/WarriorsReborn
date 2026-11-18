/* Game-independent --auto-exposure test (src/auto_exposure.c): option parsing, the parameters per
 * strength, the adaptation step (no gain at the key, time constants up and down, the brightening
 * rate limit against a lightning flash, no snap without a camera cut (the caller's snap), the hideout
 * turn without wind-up, the hold with nothing metered, the
 * gain limits) and the highlight roll-off (identity, knee, end point, monotonic). */
#include "../src/auto_exposure.h"
#include "../src/filmic.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_args(void) {
    AutoExposureConfig c = AUTO_EXPOSURE_DEFAULTS;
    CHECK(!c.on && c.strength == 1.0f, "defaults: off, strength 1");
    CHECK(auto_exposure_arg(&c, "--auto-exposure", NULL) == 1 && c.on, "--auto-exposure turns it on");
    CHECK(auto_exposure_arg(&c, "--no-auto-exposure", NULL) == 1 && !c.on, "--no-auto-exposure turns it off");
    CHECK(auto_exposure_arg(&c, "--auto-exposure-strength", "0.5") == 2 && c.strength == 0.5f, "--auto-exposure-strength 0.5");
    CHECK(auto_exposure_arg(&c, "--auto-exposure-strength", "2.1") == -1 && c.strength == 0.5f, "strength 2.1 rejected, unchanged");
    CHECK(auto_exposure_arg(&c, "--auto-exposure-strength", "x") == -1 && c.strength == 0.5f, "strength x rejected");
    CHECK(auto_exposure_arg(&c, "--auto-exposure-strength", NULL) == -1, "strength without a value rejected");
    CHECK(auto_exposure_arg(&c, "--filmic", NULL) == 0, "other options are not consumed");
}

static void test_params(void) {
    RAutoExposure p;
    auto_exposure_params(1, &p);
    CHECK(fabsf(p.ev_min + 0.4f) < 1e-6f && fabsf(p.ev_max - 0.5f) < 1e-6f, "strength 1: -0.4 to +0.5 stop (%g, %g)", p.ev_min, p.ev_max);
    CHECK(fabsf(p.tau_up - 0.6f) < 1e-6f && fabsf(p.tau_down - 2.5f) < 1e-6f, "tau 0.6 s up, 2.5 s down");
    CHECK(p.adapt > 0 && p.adapt < 1 && p.knee > 0 && p.knee < 1 && p.dither, "partial adaptation %.2f, knee %.2f, dither", p.adapt, p.knee);
    CHECK(p.key > AUTO_EXPOSURE_LOG_MIN && p.key < AUTO_EXPOSURE_LOG_MAX, "key inside the meter range (%.2f)", p.key);
    auto_exposure_params(0, &p);
    CHECK(p.ev_min == 0 && p.ev_max == 0 && p.adapt == 0, "strength 0: no gain");
    auto_exposure_params(5, &p);
    CHECK(fabsf(p.ev_max - 1.0f) < 1e-6f, "strength clamped to 2 (ev_max %g)", p.ev_max);
}

/* Runs n frames of 1/60 s with a constant meter. */
static void run(const RAutoExposure *p, AutoExposureState *s, float meter, int n) {
    for (int i = 0; i < n; i++) auto_exposure_step(p, s, meter, 1, 1 / 60.0f, 0);
}

static void test_step(void) {
    RAutoExposure p;
    auto_exposure_params(1, &p);
    AutoExposureState s = {0};
    auto_exposure_step(&p, &s, p.key, 1, 1 / 60.0f, 1);
    CHECK(s.adapted == p.key && fabsf(s.ev) < 1e-6f, "snap to the key: no gain (ev %g)", s.ev);
    run(&p, &s, p.key + 1, 36);   /* 0.6 s at one stop brighter */
    float f = s.adapted - p.key;
    CHECK(f > 0.5f && f < 0.75f, "brightening: about 63%% after 0.6 s (%.3f)", f);
    auto_exposure_step(&p, &s, p.key, 1, 0, 1);
    run(&p, &s, p.key - 1, 150);   /* 2.5 s at one stop darker */
    f = p.key - s.adapted;
    CHECK(f > 0.5f && f < 0.75f, "darkening: about 63%% after 2.5 s (%.3f)", f);
    auto_exposure_step(&p, &s, p.key, 1, 0, 1);
    run(&p, &s, p.key - 1, 30);   /* 0.5 s darker: slower than up */
    CHECK(p.key - s.adapted < 0.25f, "darkening is slow: %.3f stop in 0.5 s", p.key - s.adapted);

    /* A lightning flash: the meter 4 stops up for 125 ms. */
    auto_exposure_step(&p, &s, p.key - 0.5f, 1, 0, 1);
    const float ev0 = s.ev;
    run(&p, &s, p.key + 3.5f, 8);
    CHECK(fabsf(s.ev - ev0) < 0.1f && s.adapted < p.key, "lightning 125 ms: gain moves %.3f stop, no snap", fabsf(s.ev - ev0));
    run(&p, &s, p.key - 0.5f, 60);
    CHECK(fabsf(s.ev - ev0) < 0.1f, "after the flash: back within 0.1 stop (%.3f)", fabsf(s.ev - ev0));

    /* A big meter change without a camera cut (the camera turning to a wall, a dark corner): no
     * snap, however long it holds; only the caller's snap (a real cut, a load) jumps. */
    auto_exposure_step(&p, &s, p.key, 1, 0, 1);
    float ev_prev = s.ev, worst = 0;
    for (int i = 0; i < 60; i++) {
        auto_exposure_step(&p, &s, p.key - 4, 1, 1 / 60.0f, 0);
        worst = fmaxf(worst, fabsf(s.ev - ev_prev));
        ev_prev = s.ev;
    }
    CHECK(worst < 0.02f && s.cut == 0, "4 stops darker held 1 s, no cut: no snap (largest step %.3f stop, gain %+.2f)", worst, s.ev);
    auto_exposure_step(&p, &s, p.key + 4, 1, 1 / 60.0f, 1);
    CHECK(fabsf(s.ev - p.ev_min) < 1e-6f && s.cut == 1, "a camera cut (snap) to 4 stops brighter: the gain jumps (%+.2f)", s.ev);

    /* Nothing metered (every pixel light-masked): hold. */
    const AutoExposureState held = s;
    for (int i = 0; i < 30; i++) auto_exposure_step(&p, &s, 0, 0, 1 / 60.0f, 0);
    CHECK(s.adapted == held.adapted && s.ev == held.ev, "no meter: the level holds");
    auto_exposure_step(&p, &s, p.key + 1, 1, 0, 0);
    CHECK(s.adapted == held.adapted, "dt 0: no change");

    /* Gain limits. */
    auto_exposure_step(&p, &s, AUTO_EXPOSURE_LOG_MIN - 5, 1, 0, 1);
    CHECK(fabsf(s.ev - p.ev_max) < 1e-6f && fabsf(s.adapted - (p.key - p.ev_max / p.adapt)) < 1e-4f && s.meter == AUTO_EXPOSURE_LOG_MIN,
          "black frame: +%.2f stop at most, meter clamped, level at the gain's edge (%.2f)", s.ev, s.adapted);
    auto_exposure_step(&p, &s, 3, 1, 0, 1);
    CHECK(fabsf(s.ev - p.ev_min) < 1e-6f && fabsf(s.adapted - (p.key - p.ev_min / p.adapt)) < 1e-4f,
          "white frame: %.2f stop at least, level at the gain's edge (%.2f)", s.ev, s.adapted);
    auto_exposure_step(&p, &s, p.key - 1, 1, 0, 1);
    CHECK(s.ev > 0 && s.ev < p.ev_max, "one stop darker than the key: partial gain %.3f stop", s.ev);
}

/* The hideout: the camera turns in a room that meters about -7.5
 * (gain -0.4 stop), past something dark for 0.5 s (the meter at the floor, -13) and back. The old
 * step snapped twice (+0.9 stop at once into the dark, -0.9 stop at once 0.25 s after coming back,
 * the user's "cambio brusco de tono"); the gain must glide both ways and come back within 2.5 s. */
static void test_hideout_turn(void) {
    RAutoExposure p;
    auto_exposure_params(1, &p);
    AutoExposureState s = {0};
    auto_exposure_step(&p, &s, -7.5f, 1, 1 / 60.0f, 1);
    run(&p, &s, -7.5f, 120);
    const float ev0 = s.ev;
    float ev_prev = s.ev, worst = 0, peak = s.ev;
    int back = -1;
    for (int i = 0; i < 30 + 300; i++) {
        auto_exposure_step(&p, &s, i < 30 ? AUTO_EXPOSURE_LOG_MIN : -7.5f, 1, 1 / 60.0f, 0);
        worst = fmaxf(worst, fabsf(s.ev - ev_prev));
        peak = fmaxf(peak, s.ev);
        ev_prev = s.ev;
        if (back < 0 && i >= 30 && s.ev < ev0 + 0.05f) back = i - 30;
    }
    CHECK(fabsf(ev0 - p.ev_min) < 1e-4f, "hideout: the room meters bright, gain %+.2f stop", ev0);
    CHECK(worst < 0.03f, "hideout turn: no sudden tone change (largest step %.3f stop per frame)", worst);
    CHECK(peak > ev0 + 0.1f, "hideout turn: the dark moment still brightens (peak %+.2f stop)", peak);
    CHECK(back >= 0 && back < 150, "hideout turn: back within 0.05 stop %.2f s after the dark (no wind-up)", back / 60.0f);
}

static void test_rolloff(void) {
    const float k = 0.5f;
    CHECK(auto_exposure_rolloff(0.9f, 1.0f, k) == 0.9f && auto_exposure_rolloff(0.7f, 0.8f, k) == 0.7f, "tmax <= 1: identity");
    CHECK(auto_exposure_rolloff(0.3f, 1.4f, k) == 0.3f && auto_exposure_rolloff(k, 1.4f, k) == k, "below the knee: identity");
    CHECK(fabsf(auto_exposure_rolloff(1.4f, 1.4f, k) - 1) < 1e-5f, "tmax maps to 1 (%g)", auto_exposure_rolloff(1.4f, 1.4f, k));
    const float e = 1e-3f;
    const float slope = (auto_exposure_rolloff(k + e, 1.4f, k) - k) / e;
    CHECK(fabsf(slope - 1) < 0.01f, "smooth at the knee (slope %.4f)", slope);
    int mono = 1;
    float last = -1;
    for (int i = 0; i <= 1000; i++) {
        const float v = auto_exposure_rolloff(1.4f * i / 1000, 1.4f, k);
        if (v < last || v > 1.00001f) mono = 0;
        last = v;
    }
    CHECK(mono, "monotonic, at most 1");

    RFilmic f;
    filmic_params(1, 0, 0, &f);
    CHECK(fabsf(auto_exposure_tmax(&f, 1) - 1) < 1e-4f, "filmic at gain 1: tmax 1 (%g), no roll-off", auto_exposure_tmax(&f, 1));
    CHECK(auto_exposure_tmax(&f, 1.41f) > 1.05f, "filmic at +0.5 stop: tmax %.3f", auto_exposure_tmax(&f, 1.41f));
    CHECK(auto_exposure_tmax(NULL, 1.41f) == 1.41f && auto_exposure_tmax(NULL, 0.8f) == 0.8f, "exposure only: tmax = gain");
}

int main(void) {
    test_args();
    test_params();
    test_step();
    test_hideout_turn();
    test_rolloff();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
