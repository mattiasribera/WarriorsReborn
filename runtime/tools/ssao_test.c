/* Game-independent test of the --ssao options, the opaque-world split rule and the projection
 * scale (src/ssao.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/ssao.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    SsaoConfig c = SSAO_DEFAULTS;
    CHECK(!c.on && c.strength == SSAO_STRENGTH_DEFAULT && c.radius == SSAO_RADIUS_DEFAULT, "defaults: off, %.2f, %.2f", c.strength,
          c.radius);
    CHECK(ssao_arg(&c, "--bloom", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(ssao_arg(&c, "--ssao", NULL) == 1 && c.on, "--ssao turns it on");
    CHECK(ssao_arg(&c, "--no-ssao", NULL) == 1 && !c.on, "--no-ssao turns it off (the last one wins)");
    CHECK(ssao_arg(&c, "--ssao-strength", "1.25") == 2 && c.strength == 1.25f && !c.on, "--ssao-strength sets the strength only");
    CHECK(ssao_arg(&c, "--ssao-radius", "2.5") == 2 && c.radius == 2.5f, "--ssao-radius sets the radius");
    CHECK(ssao_arg(&c, "--ssao-strength", "0.1") == 2 && ssao_arg(&c, "--ssao-strength", "2") == 2 && c.strength == 2.0f,
          "strength range 0.1-2");
    CHECK(ssao_arg(&c, "--ssao-radius", "0.1") == 2 && ssao_arg(&c, "--ssao-radius", "8") == 2 && c.radius == 8.0f, "radius range 0.1-8");
    const char *bad_strength[] = {"0", "-1", "2.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        SsaoConfig d = SSAO_DEFAULTS;
        CHECK(ssao_arg(&d, "--ssao-strength", bad_strength[i]) == -1 && d.strength == SSAO_STRENGTH_DEFAULT,
              "--ssao-strength '%s' is rejected", bad_strength[i] ? bad_strength[i] : "(missing)");
    }
    const char *bad_radius[] = {"0", "0.05", "9", "inf", "r", NULL};
    for (int i = 0; i < 6; i++) {
        SsaoConfig d = SSAO_DEFAULTS;
        CHECK(ssao_arg(&d, "--ssao-radius", bad_radius[i]) == -1 && d.radius == SSAO_RADIUS_DEFAULT, "--ssao-radius '%s' is rejected",
              bad_radius[i] ? bad_radius[i] : "(missing)");
    }

    /* Opaque world draws: back buffer, depth test and depth write. */
    CHECK(ssao_opaque_draw(0, SSAO_DRAW_DEPTH_TEST, 1), "a depth-tested, depth-writing back-buffer draw is opaque world");
    CHECK(!ssao_opaque_draw(0, SSAO_DRAW_DEPTH_TEST, 0), "a depth-write-off draw (particles, smoke, halos) is not");
    CHECK(!ssao_opaque_draw(0, 0, 1), "a draw without depth test (HUD, text, rain layers) is not");
    CHECK(!ssao_opaque_draw(1, SSAO_DRAW_DEPTH_TEST, 1), "an offscreen draw (shadow map) is not");

    /* The split: after the last opaque draw that follows the last depth clear before it. */
    enum { O = SSAO_OP_OTHER, W = SSAO_OP_OPAQUE, C = SSAO_OP_CLEAR };
    {   /* the measured frame shape: clear, sky, depth clear, offscreen, world, effects, HUD */
        const uint8_t k[] = {C, O, O, W, C, O, O, O, W, W, W, W, O, O, O, O};
        CHECK(ssao_split(k, sizeof k) == 11, "split after the world's last opaque draw (%u)", ssao_split(k, sizeof k));
    }
    {   /* effects between opaque draws do not end the world early */
        const uint8_t k[] = {C, W, O, W, O, O};
        CHECK(ssao_split(k, sizeof k) == 3, "split after the last opaque draw, not the first effect (%u)", ssao_split(k, sizeof k));
    }
    {   /* a frame whose last opaque draw comes before its last depth clear: no split */
        const uint8_t k[] = {C, W, W, C, O, O};
        CHECK(ssao_split(k, sizeof k) == sizeof k, "no split when the depth was cleared after the 3D (%u)", ssao_split(k, sizeof k));
    }
    {
        const uint8_t k[] = {C, O, O, O};
        CHECK(ssao_split(k, sizeof k) == sizeof k, "no split without an opaque draw (menus, movies)");
        CHECK(ssao_split(k, 0) == 0, "empty frame: n");
    }
    {
        const uint8_t k[] = {W, O};
        CHECK(ssao_split(k, sizeof k) == 0, "no clear: the last opaque draw (%u)", ssao_split(k, sizeof k));
    }

    /* Projection scale from the world-view-projection columns. */
    {
        const float px = 1.3971f, py = 1.9955f, A = 1.0013f, B = 0.1001f;
        const float a = 0.6f, ca = cosf(a), sa = sinf(a), sc = 2.5f;   /* view rotation about y, uniform world scale */
        /* row-vector convention: clip = v * M; column j of M holds clip component j */
        const float rx[3] = {ca * sc, 0, -sa * sc}, ry[3] = {0, sc, 0}, rz[3] = {sa * sc, 0, ca * sc};
        float c0[4], c1[4], c3[4], out[2] = {-1, -1};
        for (int i = 0; i < 3; i++) {
            const float r[3] = {rx[i], ry[i], rz[i]};   /* row i of the world-view rotation (x, y, z columns) */
            c0[i] = px * r[0];
            c1[i] = py * r[1];
            c3[i] = -r[2];   /* w = -z_view (the title's right-handed projection: m[11] = -1) */
        }
        c0[3] = 0.3f;
        c1[3] = -0.2f;
        c3[3] = 4.0f;
        (void)A; (void)B;
        CHECK(ssao_proj_scale(c0, c1, c3, out) == 0 && fabsf(out[0] - px) < 1e-4f && fabsf(out[1] - py) < 1e-4f, "scale %.4f %.4f", out[0],
              out[1]);
        const float zero[4] = {0, 0, 0, 1};
        float keep[2] = {7, 7};
        CHECK(ssao_proj_scale(c0, c1, zero, keep) == -1 && keep[0] == 7, "a degenerate w column is rejected");
        float big[4] = {100 * c0[0], 100 * c0[1], 100 * c0[2], 0};
        CHECK(ssao_proj_scale(big, c1, c3, keep) == -1 && keep[0] == 7, "a scale out of 0.2-20 is rejected");
    }

    if (!fails) printf("ALL OK\n");
    return fails ? 1 : 0;
}
