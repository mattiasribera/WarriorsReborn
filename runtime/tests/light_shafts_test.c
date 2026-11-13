/* Game-independent test of the --light-shafts options, the rain easing, the density and the cone
 * shape the renderer's shader shares (src/light_shafts.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/light_shafts.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    LightShaftsConfig c = LIGHT_SHAFTS_DEFAULTS;
    CHECK(!c.on && !c.cheap && c.strength == LIGHT_SHAFTS_STRENGTH_DEFAULT, "defaults: off, full path, strength %.2f", c.strength);
    CHECK(light_shafts_arg(&c, "--lamp-lights", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(light_shafts_arg(&c, "--light-shafts", NULL) == 1 && c.on, "--light-shafts turns it on");
    CHECK(light_shafts_arg(&c, "--no-light-shafts", NULL) == 1 && !c.on, "--no-light-shafts turns it off");
    CHECK(light_shafts_arg(&c, "--light-shafts-cheap", NULL) == 1 && c.cheap && !c.on, "--light-shafts-cheap picks the cheap path only");
    CHECK(light_shafts_arg(&c, "--no-light-shafts-cheap", NULL) == 1 && !c.cheap, "--no-light-shafts-cheap goes back to the full path");
    CHECK(light_shafts_arg(&c, "--light-shafts-strength", "2.5") == 2 && c.strength == 2.5f && !c.on, "strength sets the strength only");
    CHECK(light_shafts_arg(&c, "--light-shafts-strength", "0.1") == 2 && light_shafts_arg(&c, "--light-shafts-strength", "3") == 2 &&
              c.strength == 3.0f,
          "strength range 0.1-3");
    const char *bad[] = {"0", "-1", "3.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        LightShaftsConfig d = LIGHT_SHAFTS_DEFAULTS;
        CHECK(light_shafts_arg(&d, "--light-shafts-strength", bad[i]) == -1 && d.strength == LIGHT_SHAFTS_STRENGTH_DEFAULT,
              "strength '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }
}

static void test_rain(void) {
    float r = 0;
    int up = 0;
    while (r < 1 && up < 1000) r = light_shafts_rain_step(r, 1), up++;
    CHECK(r == 1.0f && up >= 30 && up <= 120, "rain eases in over %d frames", up);
    int down = 0;
    while (r > 0 && down < 1000) r = light_shafts_rain_step(r, 0), down++;
    CHECK(r == 0.0f && down > up, "and out more slowly (%d frames)", down);
    CHECK(light_shafts_rain_step(-1, 0) == 0 && light_shafts_rain_step(2, 1) == 1, "the level stays in 0-1");
}

static void test_density(void) {
    const float dry = light_shafts_density(1, 0), wet = light_shafts_density(1, 1);
    CHECK(dry > 0 && fabsf(wet - dry * (1 + LIGHT_SHAFTS_RAIN_GAIN)) < 1e-6f, "rain thickens the air: %.4f -> %.4f", dry, wet);
    CHECK(fabsf(light_shafts_density(2, 0) - 2 * dry) < 1e-6f, "strength scales it");
    CHECK(light_shafts_length(0.5f) == LIGHT_SHAFTS_LEN_MIN && light_shafts_length(100) == LIGHT_SHAFTS_LEN_MAX, "length clamped");
    const float mid = light_shafts_length(5);
    CHECK(mid > LIGHT_SHAFTS_LEN_MIN && mid < LIGHT_SHAFTS_LEN_MAX && fabsf(mid - 5 * LIGHT_SHAFTS_LEN_SCALE) < 1e-6f, "length %.2f", mid);
}

static void test_cone(void) {
    const float apex[3] = {0, 0, 0}, down[3] = {0, 0, -1};
    const float len = 4;
    float on_axis[8];
    for (int i = 0; i < 8; i++) {
        const float p[3] = {0, 0, -0.5f - 0.45f * i};
        on_axis[i] = light_shafts_cone(p, apex, down, len);
    }
    CHECK(on_axis[0] > 0, "lit right under the lamp: %.3f", on_axis[0]);
    for (int i = 1; i < 8; i++) CHECK(on_axis[i] < on_axis[i - 1], "fades down the axis (%d: %.3f)", i, on_axis[i]);
    const float end[3] = {0, 0, -len - 0.01f};
    CHECK(light_shafts_cone(end, apex, down, len) == 0, "nothing past the length");
    const float side[3] = {2, 0, -1};   /* 63 degrees off the axis */
    CHECK(light_shafts_cone(side, apex, down, len) == 0, "nothing outside the cone");
    const float edge[3] = {0.6f, 0, -1}, core[3] = {0, 0, -1};
    const float e = light_shafts_cone(edge, apex, down, len), c = light_shafts_cone(core, apex, down, len);
    CHECK(e > 0 && e < c, "soft edge: %.3f under the core %.3f", e, c);
    const float above[3] = {0, 0, 1.5f};
    CHECK(light_shafts_cone(above, apex, down, len) == 0, "nothing above the lamp");
    const float rot[3] = {0, -1, 0}, p[3] = {0, -1, 0}, q[3] = {0, 0, -1};
    CHECK(fabsf(light_shafts_cone(p, apex, rot, len) - light_shafts_cone(q, apex, down, len)) < 1e-6f, "follows the axis");
}

int main(void) {
    test_options();
    test_rain();
    test_density();
    test_cone();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("light_shafts_test: ALL OK\n");
    return 0;
}
