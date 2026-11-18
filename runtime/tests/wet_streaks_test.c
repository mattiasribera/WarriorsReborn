/* Game-independent test of the --wet-light-streaks options and the light list the wet pass gets
 * (src/wet_streaks.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/wet_streaks.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    WetStreaksConfig c = WET_STREAKS_DEFAULTS;
    CHECK(!c.on && c.strength == WET_STREAKS_STRENGTH_DEFAULT && c.cap == WET_STREAKS_CAP_DEFAULT, "defaults: off, strength %.2f, cap %d",
          c.strength, c.cap);
    CHECK(wet_streaks_arg(&c, "--rain-hd-wet", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(wet_streaks_arg(&c, "--wet-light-streaks", NULL) == 1 && c.on, "--wet-light-streaks turns it on");
    CHECK(wet_streaks_arg(&c, "--no-wet-light-streaks", NULL) == 1 && !c.on, "--no-wet-light-streaks turns it off");
    CHECK(wet_streaks_arg(&c, "--wet-light-streaks-strength", "2.5") == 2 && c.strength == 2.5f && !c.on, "strength sets the strength only");
    CHECK(wet_streaks_arg(&c, "--wet-light-streaks-strength", "0.1") == 2 && wet_streaks_arg(&c, "--wet-light-streaks-strength", "4") == 2 &&
              c.strength == 4.0f,
          "strength range 0.1-4");
    CHECK(wet_streaks_arg(&c, "--wet-light-streaks-cap", "8") == 2 && c.cap == 8 && !c.on, "cap sets the cap only");
    CHECK(wet_streaks_arg(&c, "--wet-light-streaks-cap", "1") == 2 && wet_streaks_arg(&c, "--wet-light-streaks-cap", "32") == 2 && c.cap == 32,
          "cap range 1-32");
    const char *bad[] = {"0", "-1", "4.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        WetStreaksConfig d = WET_STREAKS_DEFAULTS;
        CHECK(wet_streaks_arg(&d, "--wet-light-streaks-strength", bad[i]) == -1 && d.strength == WET_STREAKS_STRENGTH_DEFAULT,
              "strength '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }
    const char *bad_cap[] = {"0", "33", "-2", "1.5", "x", "", NULL};
    for (int i = 0; i < 7; i++) {
        WetStreaksConfig d = WET_STREAKS_DEFAULTS;
        CHECK(wet_streaks_arg(&d, "--wet-light-streaks-cap", bad_cap[i]) == -1 && d.cap == WET_STREAKS_CAP_DEFAULT, "cap '%s' is rejected",
              bad_cap[i] ? bad_cap[i] : "(missing)");
    }
}

/* A camera at (0, 0, 2) looking along world +y (D3D row-vector view, right-handed view space: x
 * right = world x, y up = world z, the camera looks down -z = world +y). */
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
static LampLight light(float x, float y, float z, float r, float g, float b, float in) {
    LampLight l = {{x, y, z}, 4.0f, {r, g, b}, in, 0};
    return l;
}

static void test_build(void) {
    float view[16], proj[16];
    camera(view, proj);
    WetStreaksConfig c = WET_STREAKS_DEFAULTS;
    RRainWet w, before;
    memset(&w, 0, sizeof w);
    w.reflect = 3;
    before = w;
    const LampLight one = light(1, 10, 5, 1, 0.8f, 0.6f, 1);
    CHECK(wet_streaks_build(&c, &one, 1, 1, 1, view, proj, 0, 0, &w) == 0 && !memcmp(&w, &before, sizeof w), "off: w unchanged");
    c.on = 1;
    float bad_proj[16];
    memcpy(bad_proj, proj, sizeof bad_proj);
    bad_proj[15] = 1;   /* orthographic */
    CHECK(wet_streaks_build(&c, &one, 1, 1, 1, view, bad_proj, 0, 0, &w) == 0 && !memcmp(&w, &before, sizeof w), "no perspective camera: w unchanged");

    /* One lamp 10 ahead, 1 right, 3 above the eye: view (1, 3, -10); colour x intensity x scale. */
    const LampLight half = light(1, 10, 5, 1, 0.8f, 0.6f, 0.5f);
    CHECK(wet_streaks_build(&c, &half, 1, 1, 2, view, proj, 0, 0, &w) == 1 && w.nlights == 1, "one light kept (%u)", w.nlights);
    CHECK(fabsf(w.light[0].pos[0] - 1) < 1e-5f && fabsf(w.light[0].pos[1] - 3) < 1e-5f && fabsf(w.light[0].pos[2] + 10) < 1e-5f,
          "in view space (%.2f %.2f %.2f)", w.light[0].pos[0], w.light[0].pos[1], w.light[0].pos[2]);
    CHECK(w.light[0].radius == 4 && fabsf(w.light[0].rgb[0] - 1) < 1e-6f && fabsf(w.light[0].rgb[1] - 0.8f) < 1e-6f &&
              fabsf(w.light[0].rgb[2] - 0.6f) < 1e-6f,
          "radius and colour x intensity x scale (%.2f %.2f %.2f)", w.light[0].rgb[0], w.light[0].rgb[1], w.light[0].rgb[2]);
    CHECK(fabsf(w.streaks - WET_STREAKS_GAIN) < 1e-6f && w.streak_rough == WET_STREAKS_ROUGH && w.streak_reach == WET_STREAKS_REACH &&
              w.fallback == WET_STREAKS_FALLBACK && w.reflect == 3,
          "the look's constants, the wet ground's own fields kept (gain %.2f)", w.streaks);
    CHECK(fabsf(w.fog_rgb[0] - (0x0C / 255.0f)) < 1e-6f && fabsf(w.fog_rgb[2] - (0x18 / 255.0f)) < 1e-6f, "no fog: the default colour");
    CHECK(fabsf(w.eye[2] - 2) < 1e-5f && fabsf(w.eye[0]) < 1e-5f && fabsf(w.back[1] + 1) < 1e-5f && fabsf(w.up_v[2] - 1) < 1e-5f &&
              fabsf(w.right[0] - 1) < 1e-5f,
          "the camera in the world (eye %.1f %.1f %.1f, back y %.1f)", w.eye[0], w.eye[1], w.eye[2], w.back[1]);
    c.strength = 2;
    wet_streaks_build(&c, &half, 1, 1, 1, view, proj, 1, 0xFF336699u, &w);
    CHECK(fabsf(w.streaks - 2 * WET_STREAKS_GAIN) < 1e-6f, "strength scales the gain");
    CHECK(fabsf(w.fog_rgb[0] - 0x33 / 255.0f) < 1e-6f && fabsf(w.fog_rgb[1] - 0x66 / 255.0f) < 1e-6f && fabsf(w.fog_rgb[2] - 0x99 / 255.0f) < 1e-6f,
          "the fog colour (alpha ignored)");
    c.strength = 1;

    /* Left out: faded out, black, behind the camera, too far. */
    LampLight set[5] = {light(0, 10, 5, 1, 1, 1, 0), light(0, 10, 5, 0, 0, 0, 1), light(0, -6, 5, 1, 1, 1, 1), light(0, 2 + WET_STREAKS_FAR + 5, 5, 1, 1, 1, 1),
                        light(0, -2, 5, 1, 1, 1, 1)};
    /* the last one 2 behind the eye, radius 4: its pool of light reaches in front, kept */
    CHECK(wet_streaks_build(&c, set, 5, 5, 1, view, proj, 0, 0, &w) == 1 && w.nlights == 1 && fabsf(w.light[0].pos[2] - 2) < 1e-5f,
          "faded, black, far behind and too far left out; just behind kept (%u)", w.nlights);
    CHECK(wet_streaks_build(&c, set, 4, 4, 1, view, proj, 0, 0, &w) == 0 && w.nlights == 0 && w.streaks > 0,
          "no light left: still on (the fallback), none kept");

    /* The fires first, then the lamps in order, up to the cap. */
    LampLight many[40];
    for (int i = 0; i < 40; i++) many[i] = light((float)i, 10, 5, 1, 1, 1, 1);
    c.cap = 6;
    CHECK(wet_streaks_build(&c, many, 40, 36, 1, view, proj, 0, 0, &w) == 6, "the cap");
    CHECK(w.light[0].pos[0] == 36 && w.light[3].pos[0] == 39 && w.light[4].pos[0] == 0 && w.light[5].pos[0] == 1,
          "fires first (%.0f %.0f), then lamps (%.0f %.0f)", w.light[0].pos[0], w.light[3].pos[0], w.light[4].pos[0], w.light[5].pos[0]);
    c.cap = WET_STREAKS_CAP_MAX;
    CHECK(wet_streaks_build(&c, many, 40, 40, 1, view, proj, 0, 0, &w) == RENDERER_WET_LIGHTS && w.nlights == RENDERER_WET_LIGHTS,
          "never more than RENDERER_WET_LIGHTS");
    /* A left-handed projection (proj[11] = +1): view z flips so the camera still looks down -z. */
    proj[11] = 1;
    const float lview[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, -2, 0, 1};   /* the same camera, view z forward */
    const LampLight ahead = light(0, 10, 2, 1, 1, 1, 1);
    CHECK(wet_streaks_build(&c, &ahead, 1, 1, 1, lview, proj, 0, 0, &w) == 1 && fabsf(w.light[0].pos[2] + 10) < 1e-5f &&
              fabsf(w.back[1] + 1) < 1e-5f,
          "left-handed: in front, z -10 (%.1f), back y -1 (%.1f)", w.light[0].pos[2], w.back[1]);
}

int main(void) {
    test_options();
    test_build();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
