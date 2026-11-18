/* Game-independent test of the --reflections options (src/sssr.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/sssr.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    SssrConfig c = SSSR_DEFAULTS;
    CHECK(!c.on && c.quality == SSSR_QUALITY_MEDIUM, "defaults: classic, medium quality (%d %d)", c.on, c.quality);
    CHECK(sssr_renderer_steps(&c) == 0, "classic: renderer steps 0");
    CHECK(sssr_arg(&c, "--rain-hd", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(sssr_arg(&c, "--reflections", "sssr") == 2 && c.on, "--reflections sssr turns it on");
    CHECK(sssr_renderer_steps(&c) == sssr_max_steps(SSSR_QUALITY_MEDIUM), "sssr: medium's budget (%d)", sssr_renderer_steps(&c));
    CHECK(sssr_arg(&c, "--reflections", "classic") == 2 && !c.on, "--reflections classic turns it off");
    CHECK(sssr_arg(&c, "--reflections-quality", "high") == 2 && c.quality == SSSR_QUALITY_HIGH && !c.on, "quality sets the quality only");
    CHECK(sssr_arg(&c, "--reflections-quality", "low") == 2 && c.quality == SSSR_QUALITY_LOW, "quality low");
    CHECK(sssr_arg(&c, "--reflections-quality", "medium") == 2 && c.quality == SSSR_QUALITY_MEDIUM, "quality medium");
    c.on = 1;
    c.quality = SSSR_QUALITY_LOW;
    CHECK(sssr_renderer_steps(&c) == sssr_max_steps(SSSR_QUALITY_LOW) && sssr_max_steps(SSSR_QUALITY_LOW) > 0, "sssr low: low's budget");
    const char *bad[] = {"", "SSSR", "on", "1", "ssr", NULL};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        SssrConfig d = SSSR_DEFAULTS;
        CHECK(sssr_arg(&d, "--reflections", bad[i]) == -1 && !d.on, "--reflections '%s' is rejected", bad[i] ? bad[i] : "(missing)");
        CHECK(sssr_arg(&d, "--reflections-quality", bad[i]) == -1 && d.quality == SSSR_QUALITY_MEDIUM, "--reflections-quality '%s' is rejected",
              bad[i] ? bad[i] : "(missing)");
    }
}

/* The march budget grows with the quality; every quality fits the shader's loop bound. */
static void test_budget(void) {
    int prev = 0;
    for (int q = SSSR_QUALITY_LOW; q <= SSSR_QUALITY_HIGH; q++) {
        const int n = sssr_max_steps(q);
        CHECK(n > prev && n <= SSSR_MAX_STEPS_LIMIT, "quality %d: %d steps (after %d, limit %d)", q, n, prev, SSSR_MAX_STEPS_LIMIT);
        prev = n;
    }
    CHECK(sssr_max_steps(0) == sssr_max_steps(SSSR_QUALITY_MEDIUM) && sssr_max_steps(9) == sssr_max_steps(SSSR_QUALITY_MEDIUM),
          "an unknown quality takes medium's budget");
}

/* Stage 2: --reflections-glass, --reflections-res, rays per quality. */
static void test_glass_options(void) {
    SssrConfig c = SSSR_DEFAULTS;
    CHECK(c.glass == 1.0f && !c.full, "defaults: glass 1, half resolution (%g %d)", (double)c.glass, c.full);
    CHECK(!sssr_renderer_glass(&c), "classic: no glass pass");
    c.on = 1;
    CHECK(sssr_renderer_glass(&c), "sssr: the glass pass");
    CHECK(sssr_arg(&c, "--reflections-glass", "0.5") == 2 && c.glass == 0.5f, "--reflections-glass 0.5");
    CHECK(sssr_arg(&c, "--reflections-glass", "0") == 2 && c.glass == 0 && !sssr_renderer_glass(&c), "--reflections-glass 0 turns it off");
    CHECK(sssr_arg(&c, "--reflections-glass", "2") == 2 && c.glass == 2.0f, "--reflections-glass 2 (the most)");
    const char *bad[] = {"", "2.5", "-1", "x", "1x", NULL};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(sssr_arg(&c, "--reflections-glass", bad[i]) == -1 && c.glass == 2.0f, "--reflections-glass '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    CHECK(sssr_arg(&c, "--reflections-res", "full") == 2 && c.full == 1, "--reflections-res full");
    CHECK(sssr_arg(&c, "--reflections-res", "half") == 2 && c.full == 0, "--reflections-res half");
    CHECK(sssr_arg(&c, "--reflections-res", "quarter") == -1 && sssr_arg(&c, "--reflections-res", NULL) == -1 && c.full == 0, "--reflections-res bad values");
    CHECK(sssr_rays(SSSR_QUALITY_HIGH) == 2 && sssr_rays(SSSR_QUALITY_MEDIUM) == 1 && sssr_rays(SSSR_QUALITY_LOW) == 1, "rays: high 2, else 1");
}

/* A dry frame's glass pass: the camera as the wet builds take it, no wet ground, the glass's strength. */
static void test_glass_frame(void) {
    /* A camera at (1, 2, 3) looking along +y (world z up): view x = world x, view y = world z, view z = -world y. */
    const float view[16] = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, -1, -3, 2, 1};
    const float proj[16] = {1.5f, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1.0001f, 1, 0, 0, -0.01f, 0};
    SssrConfig c = SSSR_DEFAULTS;
    RRainWet w;
    memset(&w, 0x55, sizeof w);
    CHECK(!sssr_glass_frame(&c, view, proj, NULL, &w) && w.dry == 0x55555555, "classic: no glass frame, out unchanged");
    c.on = 1;
    c.glass = 0.7f;
    const float ab[2] = {1.01f, 0.1f};
    CHECK(sssr_glass_frame(&c, view, proj, ab, &w), "sssr: a glass frame");
    CHECK(w.dry == 1 && w.glass == 0.7f && w.reflect == 0 && w.darken == 0 && w.puddles == 0 && w.streaks == 0 && w.materials == 0 && w.damp == 0 &&
              w.nlights == 0,
          "only the glass: dry, its strength, no wet ground (reflect %g darken %g)", (double)w.reflect, (double)w.darken);
    CHECK(w.depth_a == ab[0] && w.depth_b == ab[1] && w.px == 1.5f && w.py == 2 && w.reach == SSSR_GLASS_REACH, "the projection and reach");
    CHECK(fabsf(w.eye[0] - 1) < 1e-5f && fabsf(w.eye[1] - 2) < 1e-5f && fabsf(w.eye[2] - 3) < 1e-5f, "the eye in the world (%g %g %g)", (double)w.eye[0],
          (double)w.eye[1], (double)w.eye[2]);
    CHECK(fabsf(w.up_view[1] - 1) < 1e-5f && fabsf(w.up_view[0]) < 1e-5f && fabsf(w.up_view[2]) < 1e-5f, "world up is view y (%g %g %g)", (double)w.up_view[0],
          (double)w.up_view[1], (double)w.up_view[2]);
    float ortho[16];
    memcpy(ortho, proj, sizeof ortho);
    ortho[11] = 0;
    ortho[15] = 1;
    CHECK(!sssr_glass_frame(&c, view, ortho, ab, &w), "an orthographic projection: none");
    RRainWet r;
    memset(&r, 0, sizeof r);
    sssr_glass_apply(&c, &r);
    CHECK(r.glass == 0.7f && r.dry == 0, "a wet frame takes the glass (%g)", (double)r.glass);
    c.glass = 0;
    sssr_glass_apply(&c, &r);
    CHECK(r.glass == 0, "glass 0: none");
}

int main(void) {
    test_options();
    test_budget();
    test_glass_options();
    test_glass_frame();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
