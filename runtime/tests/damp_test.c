/* Game-independent test of the --damp-streets options, the damp ground's parameters, the night test
 * and the fade (src/damp.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/damp.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    DampConfig c = DAMP_DEFAULTS;
    CHECK(!c.on && c.strength == DAMP_STRENGTH_DEFAULT, "defaults: off, strength %.2f", c.strength);
    CHECK(damp_arg(&c, "--rain-hd", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(damp_arg(&c, "--damp-streets", NULL) == 1 && c.on, "--damp-streets turns it on");
    CHECK(damp_arg(&c, "--no-damp-streets", NULL) == 1 && !c.on, "--no-damp-streets turns it off");
    CHECK(damp_arg(&c, "--damp-streets-strength", "1.5") == 2 && c.strength == 1.5f && !c.on, "strength sets the strength only");
    CHECK(damp_arg(&c, "--damp-streets-strength", "0.1") == 2 && damp_arg(&c, "--damp-streets-strength", "2") == 2 && c.strength == 2.0f,
          "strength range 0.1-2");
    const char *bad[] = {"0", "0.05", "-1", "2.5", "abc", "1x", "", "nan", NULL};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        DampConfig d = DAMP_DEFAULTS;
        CHECK(damp_arg(&d, "--damp-streets-strength", bad[i]) == -1 && d.strength == DAMP_STRENGTH_DEFAULT, "strength '%s' is rejected",
              bad[i] ? bad[i] : "(missing)");
    }
}

/* A camera at (0, 0, 2) looking along world +y (as wet_streaks_test). */
static void camera(float view[16], float proj[16]) {
    const float v[16] = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, -2, 0, 1};
    memcpy(view, v, sizeof v);
    memset(proj, 0, 16 * sizeof(float));
    proj[0] = 1.5f;
    proj[5] = 2.0f;
    proj[10] = -1.0f;
    proj[11] = -1.0f;
    proj[14] = -0.1f;
}

static void test_build(void) {
    float view[16], proj[16];
    camera(view, proj);
    DampConfig c = DAMP_DEFAULTS;
    RRainWet w, before;
    memset(&w, 0x5a, sizeof w);
    before = w;
    const float ab[2] = {1.0005f, 0.05f};
    CHECK(damp_wet_build(&c, view, proj, ab, &w) == 0 && !memcmp(&w, &before, sizeof w), "off: nothing built, w unchanged");
    c.on = 1;
    float bad[16];
    memcpy(bad, proj, sizeof bad);
    bad[15] = 1;
    CHECK(damp_wet_build(&c, view, bad, ab, &w) == 0 && !memcmp(&w, &before, sizeof w), "no perspective camera: w unchanged");
    CHECK(damp_wet_build(&c, view, proj, ab, &w) == 1, "built");
    CHECK(w.depth_a == ab[0] && w.depth_b == ab[1] && w.px == 1.5f && w.py == 2.0f, "depth projection and x/y scale");
    CHECK(fabsf(w.up_view[0]) < 1e-6f && fabsf(w.up_view[1] - 1) < 1e-6f && fabsf(w.up_view[2]) < 1e-6f, "world up in view space (%.2f %.2f %.2f)",
          w.up_view[0], w.up_view[1], w.up_view[2]);
    CHECK(fabsf(w.eye[0]) < 1e-6f && fabsf(w.eye[1]) < 1e-6f && fabsf(w.eye[2] - 2) < 1e-6f, "eye in the world (%.2f %.2f %.2f)", w.eye[0], w.eye[1],
          w.eye[2]);
    CHECK(fabsf(w.back[1] + 1) < 1e-6f && fabsf(w.right[0] - 1) < 1e-6f && fabsf(w.up_v[2] - 1) < 1e-6f, "camera axes in the world");
    CHECK(fabsf(w.reflect - DAMP_REFLECT) < 1e-6f && fabsf(w.darken - DAMP_DARKEN) < 1e-6f && w.reach == DAMP_REACH, "the look at strength 1");
    CHECK(fabsf(w.puddles - DAMP_PUDDLES) < 1e-6f && w.time == 0, "damp patches, no ripple clock");
    CHECK(w.damp == 1 && w.streaks == 0 && w.materials == 0 && w.nlights == 0 && w.fallback == 0, "damp mode; streaks and materials left to their builders");
    /* Far below the rain's wet ground (reflection 2.5 x 1.8, darkening 0.3 x 1.8). */
    CHECK(w.reflect < 0.5f * 2.5f * 1.8f && w.darken < 0.5f * 0.3f * 1.8f, "fainter than the rain's");
    c.strength = 2;
    RRainWet s2;
    CHECK(damp_wet_build(&c, view, proj, NULL, &s2) == 1 && fabsf(s2.reflect - 2 * DAMP_REFLECT) < 1e-6f &&
              fabsf(s2.darken - 2 * DAMP_DARKEN) < 1e-6f && s2.damp == 2 && fabsf(s2.puddles - DAMP_PUDDLES) < 1e-6f,
          "strength 2 doubles reflection and darkening (%.2f %.2f)", s2.reflect, s2.darken);
    CHECK(s2.depth_a > 1 && s2.depth_b > 0, "no depth fit: the default projection");
    c.strength = 2;
    s2.darken = 0;
    c.strength = DAMP_STRENGTH_MAX;
    damp_wet_build(&c, view, proj, ab, &s2);
    CHECK(s2.darken <= DAMP_DARKEN_MAX, "darkening capped (%.2f)", s2.darken);

    RRainWet t;
    memset(&t, 0, sizeof t);
    t.streaks = 2;
    t.fallback = 0.5f;
    damp_streaks_apply(&t);
    CHECK(fabsf(t.streaks - 2 * DAMP_STREAKS) < 1e-6f && t.fallback == 0, "streaks at their damp share (%.2f), no miss fallback", t.streaks);
    CHECK(t.streak_reach == DAMP_STREAK_REACH && DAMP_STREAK_REACH < 2.5f, "streaks reach less far than the rain's 2.5 radii: glints by the lamps (%.2f)", t.streak_reach);
}

static void test_night(void) {
    CHECK(damp_night(0xFF000000u) && damp_night(0x00101828u) && damp_night(0xFF303040u), "dark zeniths are night");
    CHECK(!damp_night(0xFF6080C0u) && !damp_night(0xFFFFFFFFu) && !damp_night(0xFF808080u), "bright zeniths are not");
    CHECK(damp_night(0xFF3F3F3Fu) && !damp_night(0xFF414141u), "the luma threshold %.0f", DAMP_NIGHT_LUMA);
}

static void test_fade(void) {
    DampFade f;
    memset(&f, 0, sizeof f);
    CHECK(damp_fade_step(&f, 1, 1, 1000) == 0, "the first clock reading moves nothing");
    CHECK(fabsf(damp_fade_step(&f, 1, 1, 1000 + (uint32_t)(DAMP_FADE_MS / 2)) - 0.5f) < 1e-4f, "half the fade time: half");
    CHECK(damp_fade_step(&f, 1, 1, 100000) == 1, "a long step is clamped to one fade: full");
    CHECK(damp_fade_step(&f, 1, 1, 100100) == 1, "stays at 1");
    CHECK(fabsf(damp_fade_step(&f, 0, 1, 100100 + (uint32_t)(DAMP_FADE_MS / 4)) - 0.75f) < 1e-4f, "fades out by the clock");
    DampFade g;
    memset(&g, 0, sizeof g);
    float lv = 0;
    for (int i = 0; i < 10; i++) lv = damp_fade_step(&g, 1, 0, 0);
    CHECK(fabsf(lv - 10 * DAMP_FADE_FRAME_MS / DAMP_FADE_MS) < 1e-4f, "no clock: a frame step each (%.3f)", lv);
    for (int i = 0; i < 200; i++) lv = damp_fade_step(&g, 0, 0, 0);
    CHECK(lv == 0, "back to 0, never below");
}

int main(void) {
    test_options();
    test_build();
    test_night();
    test_fade();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
