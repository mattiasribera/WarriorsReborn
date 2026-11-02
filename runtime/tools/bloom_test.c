/* Game-independent test of the --bloom options and the scene/HUD split rule (src/bloom.c). */
#include <stdio.h>
#include <string.h>
#include "../src/bloom.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    BloomConfig c = BLOOM_DEFAULTS;
    CHECK(!c.on && c.strength == BLOOM_STRENGTH_DEFAULT && c.threshold == BLOOM_THRESHOLD_DEFAULT, "defaults: off, %.2f, %.2f",
          c.strength, c.threshold);
    CHECK(bloom_arg(&c, "--sharpen", "50") == 0 && !c.on, "other options are not consumed");
    CHECK(bloom_arg(&c, "--bloom", NULL) == 1 && c.on, "--bloom turns it on");
    CHECK(bloom_arg(&c, "--no-bloom", NULL) == 1 && !c.on, "--no-bloom turns it off (the last one wins)");
    CHECK(bloom_arg(&c, "--bloom-strength", "1.25") == 2 && c.strength == 1.25f && !c.on, "--bloom-strength sets the strength only");
    CHECK(bloom_arg(&c, "--bloom-threshold", "0.65") == 2 && c.threshold == 0.65f, "--bloom-threshold sets the threshold");
    CHECK(bloom_arg(&c, "--bloom-threshold", "0") == 2 && c.threshold == 0.0f, "threshold 0 is valid");
    CHECK(bloom_arg(&c, "--bloom-threshold", "1") == 2 && c.threshold == 1.0f, "threshold 1 is valid");
    const char *bad_strength[] = {"0", "-1", "3.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        BloomConfig d = BLOOM_DEFAULTS;
        CHECK(bloom_arg(&d, "--bloom-strength", bad_strength[i]) == -1 && d.strength == BLOOM_STRENGTH_DEFAULT,
              "--bloom-strength '%s' is rejected", bad_strength[i] ? bad_strength[i] : "(missing)");
    }
    const char *bad_threshold[] = {"-0.1", "1.5", "x", NULL};
    for (int i = 0; i < 4; i++) {
        BloomConfig d = BLOOM_DEFAULTS;
        CHECK(bloom_arg(&d, "--bloom-threshold", bad_threshold[i]) == -1 && d.threshold == BLOOM_THRESHOLD_DEFAULT,
              "--bloom-threshold '%s' is rejected", bad_threshold[i] ? bad_threshold[i] : "(missing)");
    }
    CHECK(bloom_arg(&c, "--bloom-strength", "0.1") == 2 && bloom_arg(&c, "--bloom-strength", "3") == 2 && c.strength == 3.0f,
          "strength range 0.1-3");

    /* The split: after the last depth-tested draw on the back buffer. */
    CHECK(bloom_scene_draw(0, BLOOM_DRAW_DEPTH_TEST), "a depth-tested back-buffer draw is scene");
    CHECK(!bloom_scene_draw(0, 0), "a draw without depth test (HUD, text, menus) is not scene");
    CHECK(!bloom_scene_draw(1, BLOOM_DRAW_DEPTH_TEST), "an offscreen draw (shadow map) is not scene");
    const struct { int off; unsigned flags; } frame[] = {{0, 0}, {1, 1}, {0, 1}, {0, 1}, {0, 0}, {0, 1}, {0, 0}, {0, 0}};
    int last = -1;
    for (int i = 0; i < 8; i++)
        if (bloom_scene_draw(frame[i].off, frame[i].flags)) last = i;
    CHECK(last == 5, "split after draw 5 of the sample frame (%d)", last);

    /* Halo draws: alpha blended, depth test on, depth write off, FVF 0x142, triangle list. */
    const BloomDraw halo = {1, 0x302, 0x303, 0x8006, 1, 0, BLOOM_HALO_FVF, 5};
    CHECK(bloom_halo_draw(&halo), "a lamp halo quad is a halo draw");
    BloomDraw d = halo;
    d.blend_eq = 0;
    CHECK(bloom_halo_draw(&d), "blend equation never set (ADD) still matches");
    d = halo; d.blend_enable = 0;
    CHECK(!bloom_halo_draw(&d), "without blending: not a halo");
    d = halo; d.blend_dst = 1;
    CHECK(!bloom_halo_draw(&d), "additive (SRC_ALPHA onto ONE: fire, neon) still blooms");
    d = halo; d.blend_eq = 0x800B;
    CHECK(!bloom_halo_draw(&d), "reverse subtract: not a halo");
    d = halo; d.depth_test = 0;
    CHECK(!bloom_halo_draw(&d), "without depth test (HUD): not a halo");
    d = halo; d.depth_write = 1;
    CHECK(!bloom_halo_draw(&d), "with depth write: not a halo");
    d = halo; d.fvf = 0x152;
    CHECK(!bloom_halo_draw(&d), "FVF 0x152 (with normals: decals, shadows): not a halo");
    d = halo; d.fvf = 0;
    CHECK(!bloom_halo_draw(&d), "vertex program: not a halo");
    d = halo; d.prim = 7;
    CHECK(!bloom_halo_draw(&d), "triangle fan: not a halo");

    /* Replay order: halos before the split move to right after it. */
    {
        const uint8_t k[] = {BLOOM_OP_OTHER, BLOOM_OP_CLEAR, BLOOM_OP_SCENE, BLOOM_OP_HALO, BLOOM_OP_SCENE, BLOOM_OP_HALO,
                             BLOOM_OP_SCENE, BLOOM_OP_OTHER, BLOOM_OP_HALO, BLOOM_OP_OTHER};
        const uint32_t want[] = {0, 1, 2, 4, 6, 3, 5, 7, 8, 9};
        uint32_t o[10];
        uint32_t at = bloom_order(k, 10, o);
        CHECK(at == 5, "split after the 5th op in replay order (%u)", at);
        CHECK(!memcmp(o, want, sizeof want), "order 0 1 2 4 6 | 3 5 7 8 9 (got %u %u %u %u %u %u %u %u %u %u)", o[0], o[1], o[2],
              o[3], o[4], o[5], o[6], o[7], o[8], o[9]);
    }
    {
        const uint8_t k[] = {BLOOM_OP_HALO, BLOOM_OP_SCENE, BLOOM_OP_CLEAR, BLOOM_OP_HALO, BLOOM_OP_SCENE, BLOOM_OP_OTHER};
        const uint32_t want[] = {0, 1, 2, 4, 3, 5};
        uint32_t o[6];
        uint32_t at = bloom_order(k, 6, o);
        CHECK(at == 4 && !memcmp(o, want, sizeof want), "a halo before a back-buffer clear stays in place (%u: %u %u %u %u %u %u)",
              at, o[0], o[1], o[2], o[3], o[4], o[5]);
    }
    {
        const uint8_t k[] = {BLOOM_OP_OTHER, BLOOM_OP_HALO, BLOOM_OP_OTHER};
        uint32_t o[3];
        uint32_t at = bloom_order(k, 3, o);
        CHECK(at == 0 && o[0] == 0 && o[1] == 1 && o[2] == 2, "only halos, no scene draw: no split, order unchanged (%u)", at);
    }
    {
        const uint8_t k[] = {BLOOM_OP_SCENE, BLOOM_OP_OTHER};
        uint32_t o[2];
        uint32_t at = bloom_order(k, 2, o);
        CHECK(at == 1 && o[0] == 0 && o[1] == 1, "no halos: split after the last scene draw, order unchanged (%u)", at);
    }

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
