/* Game-independent test of the --soft-particles options, the particle draw signature and the soft
 * quad rule (src/soft_particles.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/soft_particles.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Vertices of a quad in view space (centre c, half axes a and b), projected as the title's camera
 * does (clip x = px X, y = py Y, w = zsign Z), four floats per vertex. */
static void quad(float *out, const float c[3], const float a[3], const float b[3], const SoftView *v) {
    const float sa[4] = {-1, 1, 1, -1}, sb[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 4; k++) {
        float p[3];
        for (int i = 0; i < 3; i++) p[i] = c[i] + sa[k] * a[i] + sb[k] * b[i];
        out[k * 4 + 0] = v->px * p[0];
        out[k * 4 + 1] = v->py * p[1];
        out[k * 4 + 2] = 0.5f;
        out[k * 4 + 3] = v->zsign * p[2];
    }
}
static int one(const float c[3], const float a[3], const float b[3], const SoftView *v) {
    float x[16];
    quad(x, c, a, b, v);
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    uint8_t s = 7;
    return soft_classify(x, 4, idx, 6, v, &s) == 1 ? s : -1;
}

int main(void) {
    SoftConfig c = SOFT_DEFAULTS;
    CHECK(!c.on && c.distance == SOFT_DISTANCE_DEFAULT, "defaults: off, %.2f", c.distance);
    CHECK(soft_arg(&c, "--ssao", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(soft_arg(&c, "--soft-particles", NULL) == 1 && c.on, "--soft-particles turns it on");
    CHECK(soft_arg(&c, "--no-soft-particles", NULL) == 1 && !c.on, "--no-soft-particles turns it off (the last one wins)");
    CHECK(soft_arg(&c, "--soft-particles-distance", "1.25") == 2 && c.distance == 1.25f && !c.on, "--soft-particles-distance sets the distance only");
    CHECK(soft_arg(&c, "--soft-particles-distance", "0.05") == 2 && soft_arg(&c, "--soft-particles-distance", "4") == 2 && c.distance == 4.0f,
          "distance range 0.05-4");
    const char *bad[] = {"0", "-1", "0.04", "4.5", "abc", "1x", "", "nan", "inf", NULL};
    for (int i = 0; i < 10; i++) {
        SoftConfig d = SOFT_DEFAULTS;
        CHECK(soft_arg(&d, "--soft-particles-distance", bad[i]) == -1 && d.distance == SOFT_DISTANCE_DEFAULT,
              "--soft-particles-distance '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }

    /* The particle draw signature. */
    const SoftDraw smoke = {0, SOFT_FVF, SOFT_PRIM_TRIANGLELIST, SOFT_STRIDE, 1, 1, 0, 1, 0x302, 0x303, 0x8006};
    CHECK(soft_particle_draw(&smoke), "alpha-blended particles (smoke, dust, halos) match");
    SoftDraw d = smoke;
    d.blend_src = 1;
    d.blend_dst = 1;
    CHECK(soft_particle_draw(&d), "additive particles (fire, sparks) match");
    d.blend_src = 0x302;
    d.blend_dst = 1;
    d.blend_eq = 0;
    CHECK(soft_particle_draw(&d), "SRC_ALPHA onto ONE with the unset (ADD) equation matches");
    d = smoke;
    d.depth_test = 0;
    CHECK(!soft_particle_draw(&d), "the glow/overlay pass (no depth test) does not match");
    d = smoke;
    d.depth_write = 1;
    CHECK(!soft_particle_draw(&d), "a depth-writing draw does not match");
    d = smoke;
    d.offscreen = 1;
    CHECK(!soft_particle_draw(&d), "an offscreen draw does not match");
    d = smoke;
    d.fvf = 0x152;
    CHECK(!soft_particle_draw(&d), "another FVF does not match");
    d = smoke;
    d.fvf = 0;
    CHECK(!soft_particle_draw(&d), "a vertex program does not match");
    d = smoke;
    d.prim = 6;
    CHECK(!soft_particle_draw(&d), "a strip does not match");
    d = smoke;
    d.stride = 28;
    CHECK(!soft_particle_draw(&d), "another stride does not match");
    d = smoke;
    d.indexed = 0;
    CHECK(!soft_particle_draw(&d), "a non-indexed draw does not match");
    d = smoke;
    d.blend_enable = 0;
    CHECK(!soft_particle_draw(&d), "an unblended draw does not match");
    d = smoke;
    d.blend_src = 0x306;   /* DST_COLOR: multiply */
    d.blend_dst = 0;
    CHECK(!soft_particle_draw(&d), "a multiplying blend does not match");
    d = smoke;
    d.blend_eq = 0x800A;
    CHECK(!soft_particle_draw(&d), "a subtracting blend does not match");

    /* The soft quad rule, right-handed camera (w = -Z) pitched down 20 degrees. */
    const float th = 20.0f * 3.14159265f / 180.0f;
    SoftView v = {1.4f, 2.0f, -1.0f, {0, cosf(th), sinf(th)}, 1};
    const float x5[3] = {0.5f, 0, 0}, y5[3] = {0, 0.5f, 0};
    const float ahead[3] = {0, 0, -5}, side[3] = {1, 0.5f, -5};
    CHECK(one(ahead, x5, y5, &v) == 1, "a billboard ahead is soft");
    CHECK(one(side, x5, y5, &v) == 1, "a screen-aligned billboard off the centre is soft");
    const float edge[3] = {4.5f, 0, -5};   /* screen-aligned at the edge of a wide view: 42 degrees off the eye ray */
    CHECK(one(edge, x5, y5, &v) == 1, "a screen-aligned billboard at the edge of the view is soft");
    const float r1[3] = {0.35f, 0.35f, 0}, r2[3] = {-0.35f, 0.35f, 0};
    CHECK(one(ahead, r1, r2, &v) == 1, "a rotated billboard is soft");
    const float fwd[3] = {0, 0.5f * sinf(th), -0.5f * cosf(th)};   /* horizontal, away from the camera */
    const float floor_c[3] = {0, -1.5f, -6};
    CHECK(one(floor_c, x5, fwd, &v) == 0, "a ground decal seen at 20 degrees is not soft");
    const float wall_b[3] = {0.25f, 0, -0.433f}, wall_c[3] = {0, 0, -6};   /* a wall 60 degrees off the view */
    CHECK(one(wall_c, wall_b, y5, &v) == 0, "a wall decal seen obliquely is not soft");
    SoftView top = {1.4f, 2.0f, -1.0f, {0, 0, 1}, 1};   /* looking straight down */
    CHECK(one(ahead, x5, y5, &top) == 0, "a ground decal under a camera looking straight down is not soft");
    top.has_up = 0;
    CHECK(one(ahead, x5, y5, &top) == 1, "without the world up a camera-facing quad is soft");
    const float zero[3] = {0, 0, 0};
    CHECK(one(ahead, x5, zero, &v) == 0, "a degenerate quad is not soft");
    SoftView lh = {1.4f, 2.0f, 1.0f, {0, 1, 0}, 1};
    const float ahead_lh[3] = {0, 0, 5};
    CHECK(one(ahead_lh, x5, y5, &lh) == 1, "a billboard ahead of a left-handed camera is soft");
    const float tilt[3] = {0, 0.5f * sinf(0.7f), -0.5f * cosf(0.7f)};   /* tipped 50 degrees back from the view */
    const float up_c[3] = {0, 1, -5};
    CHECK(one(up_c, x5, tilt, &v) == 0, "a quad tipped 50 degrees away from the camera is not soft");

    /* A batch: soft, decal, soft, with the other index pattern; vertices with a stride. */
    {
        float x[3][16];
        quad(x[0], ahead, x5, y5, &v);
        quad(x[1], floor_c, x5, fwd, &v);
        quad(x[2], side, r1, r2, &v);
        float vb[12 * 6];
        for (int k = 0; k < 12; k++) {
            memcpy(&vb[k * 6], &x[k / 4][(k % 4) * 4], 16);
            vb[k * 6 + 4] = vb[k * 6 + 5] = 99;
        }
        uint32_t idx[18];
        for (uint32_t q = 0; q < 3; q++) {
            const uint32_t p[6] = {0, 1, 3, 3, 1, 2};
            for (int i = 0; i < 6; i++) idx[q * 6 + i] = q * 4 + p[i];
        }
        uint8_t s[3] = {7, 7, 7};
        const uint32_t n = soft_classify(vb, 6, idx, 18, &v, s);
        CHECK(n == 3 && s[0] == 1 && s[1] == 0 && s[2] == 1, "batch: %u quads, %u %u %u", n, s[0], s[1], s[2]);
        CHECK(soft_classify(vb, 6, idx, 15, &v, s) == 0, "a triangle count that is not quads is not classified");
        CHECK(soft_classify(vb, 6, idx, 0, &v, s) == 0, "an empty draw is not classified");
    }

    if (!fails) printf("ALL OK\n");
    return fails ? 1 : 0;
}
