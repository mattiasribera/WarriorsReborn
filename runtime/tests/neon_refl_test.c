/* Game-independent test of the --neon-reflections options, the signs' colour and position and the
 * lights they add to the wet ground (src/neon_refl.c). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/neon_refl.h"
#include "../src/wet_streaks.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    NeonReflConfig c = NEON_REFL_DEFAULTS;
    CHECK(!c.on && c.strength == NEON_REFL_STRENGTH_DEFAULT, "defaults: off, strength %.2f", c.strength);
    CHECK(neon_refl_arg(&c, "--neon-glow", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(neon_refl_arg(&c, "--neon-reflections", NULL) == 1 && c.on, "--neon-reflections turns it on");
    CHECK(neon_refl_arg(&c, "--no-neon-reflections", NULL) == 1 && !c.on, "--no-neon-reflections turns it off");
    CHECK(neon_refl_arg(&c, "--neon-reflections-strength", "2.5") == 2 && c.strength == 2.5f && !c.on, "strength sets the strength only");
    CHECK(neon_refl_arg(&c, "--neon-reflections-strength", "0.1") == 2 && neon_refl_arg(&c, "--neon-reflections-strength", "3") == 2 &&
              c.strength == 3.0f,
          "strength range 0.1-3");
    const char *bad[] = {"0", "0.05", "-1", "3.5", "abc", "1x", "", "nan", NULL};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        NeonReflConfig d = NEON_REFL_DEFAULTS;
        CHECK(neon_refl_arg(&d, "--neon-reflections-strength", bad[i]) == -1 && d.strength == NEON_REFL_STRENGTH_DEFAULT,
              "strength '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }
}

/* A 64 x 32 mask chain whose right half is lit (255) at every level, level 0 only lit at x >= 32. */
static uint8_t *mask_chain(uint32_t w, uint32_t h, uint32_t *levels) {
    uint32_t n = 0, total = 0;
    for (uint32_t lw = w, lh = h;; n++) {
        total += lw * lh;
        if (lw == 1 && lh == 1) break;
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    *levels = n + 1;
    uint8_t *c = calloc(total, 1), *p = c;
    for (uint32_t l = 0; l < *levels; l++) {
        const uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        for (uint32_t y = 0; y < lh; y++)
            for (uint32_t x = 0; x < lw; x++) p[y * lw + x] = x >= lw / 2 ? 255 : 0;
        if (l == 1) p[1] = 77;   /* marks level 1 (32 x 16: the smallest level at least 16 wide and high) */
        p += lw * lh;
    }
    return c;
}

static void test_colour(void) {
    uint32_t levels;
    uint8_t *chain = mask_chain(64, 32, &levels);
    uint8_t g[NEON_REFL_GRID * NEON_REFL_GRID];
    CHECK(neon_refl_grid(NULL, 64, 32, levels, g) == -1, "no data: -1");
    CHECK(neon_refl_grid(chain, 64, 32, levels, g) == 0, "grid of a chain");
    CHECK(g[0] == 77, "read from the smallest level at least %d wide and high (level 1), got %u", NEON_REFL_GRID, g[0]);
    CHECK(g[1] == 0 && g[NEON_REFL_GRID - 1] == 255 && g[NEON_REFL_GRID / 2] == 255 && g[NEON_REFL_GRID / 2 - 1] == 0, "left dark, right lit");
    uint8_t small[4] = {0, 255, 0, 255};
    CHECK(neon_refl_grid(small, 2, 2, 1, g) == 0 && g[0] == 0 && g[NEON_REFL_GRID - 1] == 255, "a level 0 smaller than the grid: nearest");
    free(chain);

    /* Texture 8 x 8: left half grey, right half red; the mask lights the right half: pure red. */
    uint32_t tex[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) tex[y * 8 + x] = x >= 4 ? 0xFFFF0000u : 0xFF808080u;
    for (int i = 0; i < NEON_REFL_GRID * NEON_REFL_GRID; i++) g[i] = (i % NEON_REFL_GRID) >= NEON_REFL_GRID / 2 ? 255 : 0;
    float rgb[3], cover;
    CHECK(neon_refl_colour(g, tex, 8, 8, rgb, &cover) == 0, "a lit mask has a colour");
    CHECK(fabsf(rgb[0] - 1) < 1e-4f && rgb[1] < 1e-4f && rgb[2] < 1e-4f, "the lit parts' colour (red), got %.3f %.3f %.3f", rgb[0], rgb[1], rgb[2]);
    CHECK(fabsf(cover - 0.5f) < 1e-3f, "cover = the mask's mean (0.5), got %.3f", cover);
    /* Half-lit mask texels weigh half. */
    for (int i = 0; i < NEON_REFL_GRID * NEON_REFL_GRID; i++) g[i] = (i % NEON_REFL_GRID) >= NEON_REFL_GRID / 2 ? 255 : 85;
    CHECK(neon_refl_colour(g, tex, 8, 8, rgb, &cover) == 0 && fabsf(rgb[0] - (255 + 85 * 128 / 255.0f) / 340) < 2e-3f,
          "weighted by the mask, got %.3f", rgb[0]);
    memset(g, 0, sizeof g);
    CHECK(neon_refl_colour(g, tex, 8, 8, rgb, &cover) == -1, "a dark mask: no light");
    for (int i = 0; i < NEON_REFL_GRID * NEON_REFL_GRID; i++) g[i] = 255;
    CHECK(neon_refl_colour(g, NULL, 8, 8, rgb, &cover) == -1 && neon_refl_colour(g, tex, 0, 8, rgb, &cover) == -1, "no texture: -1");
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
/* World point to clip (row vectors: clip = world x view x proj). */
static void to_clip(const float view[16], const float proj[16], const float w[3], float c[4]) {
    float v[4];
    for (int i = 0; i < 4; i++) v[i] = w[0] * view[i] + w[1] * view[4 + i] + w[2] * view[8 + i] + view[12 + i];
    for (int i = 0; i < 4; i++) c[i] = v[0] * proj[i] + v[1] * proj[4 + i] + v[2] * proj[8 + i] + v[3] * proj[12 + i];
}

static void test_sprite(void) {
    float view[16], proj[16];
    camera(view, proj);
    /* A sign 4 wide (x -2..2), 1 high (z 5..6) at y 10, in a vertex buffer with 2 extra floats per vertex. */
    const float corners[4][3] = {{-2, 10, 5}, {2, 10, 5}, {2, 10, 6}, {-2, 10, 6}};
    float buf[4][6];
    for (int i = 0; i < 4; i++) {
        to_clip(view, proj, corners[i], buf[i]);
        buf[i][4] = buf[i][5] = 123;
    }
    const float red[3] = {1, 0.1f, 0.2f};
    LampSprite s;
    CHECK(neon_refl_sprite(&buf[0][0], 6, 4, view, proj, red, &s) == 0, "a sign in front of the camera");
    CHECK(fabsf(s.pos[0]) < 1e-3f && fabsf(s.pos[1] - 10) < 1e-3f && fabsf(s.pos[2] - 5.5f) < 1e-3f, "centre in the world (0 10 5.5), got %.3f %.3f %.3f",
          s.pos[0], s.pos[1], s.pos[2]);
    CHECK(fabsf(s.size - 4) < 1e-3f, "size = the largest extent (4), got %.3f", s.size);
    CHECK(s.rgb[0] == red[0] && s.rgb[1] == red[1] && s.rgb[2] == red[2] && s.weight == 1, "colour and weight 1");
    /* A tiny sign gets the minimum size; a vertex behind the camera is left out. */
    const float tiny[2][3] = {{0, 10, 5}, {0.1f, 10, 5}};
    float tb[3][4];
    to_clip(view, proj, tiny[0], tb[0]);
    to_clip(view, proj, tiny[1], tb[1]);
    const float behind[3] = {50, -20, 5};
    to_clip(view, proj, behind, tb[2]);
    CHECK(tb[2][3] < 0, "the third vertex is behind the camera");
    CHECK(neon_refl_sprite(&tb[0][0], 4, 3, view, proj, red, &s) == 0 && fabsf(s.size - NEON_REFL_MIN_SIZE) < 1e-6f && fabsf(s.pos[0] - 0.05f) < 1e-3f,
          "minimum size, behind-camera vertex ignored: size %.3f x %.3f", s.size, s.pos[0]);
    CHECK(neon_refl_sprite(&tb[2][0], 4, 1, view, proj, red, &s) == -1, "only a vertex behind the camera: -1");
    CHECK(neon_refl_sprite(&buf[0][0], 6, 0, view, proj, red, &s) == -1, "no vertex: -1");
    float ortho[16];
    memcpy(ortho, proj, sizeof ortho);
    ortho[11] = 0;
    ortho[15] = 1;
    CHECK(neon_refl_sprite(&buf[0][0], 6, 4, view, ortho, red, &s) == -1, "not a perspective camera: -1");
}

static void test_apply(void) {
    float view[16], proj[16];
    camera(view, proj);
    NeonReflConfig c = NEON_REFL_DEFAULTS;
    LampLight sign[NEON_REFL_CAP + 3];
    for (int i = 0; i < NEON_REFL_CAP + 3; i++) sign[i] = (LampLight){{(float)i, 10, 5}, 8.0f, {0.2f, 0.4f, 1.0f}, 0.5f, (uint32_t)i};
    RRainWet w, before;
    memset(&w, 0, sizeof w);
    w.streaks = 1.0f;
    w.nlights = 3;
    before = w;
    CHECK(neon_refl_apply(&c, sign, 2, view, proj, &w) == 0 && !memcmp(&w, &before, sizeof w), "off: nothing changes");
    c.on = 1;
    c.strength = 2;
    w.streaks = 0;
    before = w;
    CHECK(neon_refl_apply(&c, sign, 2, view, proj, &w) == 0 && !memcmp(&w, &before, sizeof w), "without streaks: nothing changes");
    w.streaks = 1;
    CHECK(neon_refl_apply(&c, sign, 2, view, proj, &w) == 2 && w.nlights == 5, "appended after the lamps: %u lights", w.nlights);
    CHECK(w.neon == 2, "the reflection keeps saturated colours at the strength, got %.2f", w.neon);
    /* View space (x right, y up, z towards the viewer): world (1, 10, 5) is 1 right, 3 up, 10 ahead. */
    CHECK(fabsf(w.light[4].pos[0] - 1) < 1e-4f && fabsf(w.light[4].pos[1] - 3) < 1e-4f && fabsf(w.light[4].pos[2] + 10) < 1e-4f,
          "view position, got %.3f %.3f %.3f", w.light[4].pos[0], w.light[4].pos[1], w.light[4].pos[2]);
    const float k = 0.5f * NEON_REFL_GAIN * 2;
    CHECK(fabsf(w.light[3].rgb[2] - k) < 1e-4f && fabsf(w.light[3].rgb[0] - 0.2f * k) < 1e-4f, "colour x intensity x gain x strength, got %.3f",
          w.light[3].rgb[2]);
    CHECK(w.light[3].radius == (8 < NEON_REFL_RADIUS_MAX ? 8 : NEON_REFL_RADIUS_MAX) && fabsf(w.light[3].size - 8 / LAMP_RADIUS_SCALE) < 1e-6f,
          "radius (at most %.1f) and extent: %.2f %.2f", NEON_REFL_RADIUS_MAX, w.light[3].radius, w.light[3].size);
    /* The cap, and the wet pass's limit. */
    memset(&w, 0, sizeof w);
    w.streaks = 1;
    CHECK(neon_refl_apply(&c, sign, NEON_REFL_CAP + 3, view, proj, &w) == NEON_REFL_CAP && w.nlights == NEON_REFL_CAP, "at most %d signs",
          NEON_REFL_CAP);
    w.nlights = RENDERER_WET_LIGHTS - 2;
    CHECK(neon_refl_apply(&c, sign, NEON_REFL_CAP, view, proj, &w) == 2 && w.nlights == RENDERER_WET_LIGHTS, "at most %d lights in all",
          RENDERER_WET_LIGHTS);
    /* Out-of-reach and dark signs are left out (as the lamps). */
    memset(&w, 0, sizeof w);
    w.streaks = 1;
    LampLight odd[3] = {{{0, -30, 5}, 4, {1, 1, 1}, 1, 0}, {{0, 10, 5}, 4, {0, 0, 0}, 1, 1}, {{0, 10 + WET_STREAKS_FAR + 10, 5}, 4, {1, 1, 1}, 1, 2}};
    CHECK(neon_refl_apply(&c, odd, 3, view, proj, &w) == 0 && w.nlights == 0 && w.neon == 2, "behind, black, too far: none (neon still on)");
    /* A lamp light keeps size 0 (wet_streaks_build). */
    memset(&w, 0, sizeof w);
    WetStreaksConfig sc = {1, 1, 16};
    LampLight lamp = {{0, 10, 5}, 4, {1, 1, 1}, 1, 0};
    w.light[0].size = 9;
    CHECK(wet_streaks_build(&sc, &lamp, 1, 1, 1, view, proj, 0, 0, &w) == 1 && w.light[0].size == 0, "a lamp is a point light (size 0)");
}

int main(void) {
    test_options();
    test_colour();
    test_sprite();
    test_apply();
    if (fails) {
        printf("neon_refl_test: %d FAILED\n", fails);
        return 1;
    }
    printf("neon_refl_test: ALL OK\n");
    return 0;
}
