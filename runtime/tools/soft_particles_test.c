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

/* One quad with texture coordinates: six floats per vertex (clip x, y, z, w, then u, v), its uv
 * rectangle uv0-uv1; classified by soft_classify_sprites with texture tex and the sprites ws. */
static int one_sprite(const float c[3], const float a[3], const float b[3], const SoftView *v, uint64_t tex, float u0, float v0, float u1,
                      float v1, SoftSprites *ws) {
    float x[16], vb[24];
    quad(x, c, a, b, v);
    const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
    for (int k = 0; k < 4; k++) {
        memcpy(&vb[k * 6], &x[k * 4], 16);
        vb[k * 6 + 4] = uv[k][0];
        vb[k * 6 + 5] = uv[k][1];
    }
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    uint8_t s = 7;
    return soft_classify_sprites(vb, 6, 4, idx, 6, v, tex, ws, &s) == 1 ? s : -1;
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

    /* Wall decals (the graffiti tags, part_spray_tag: world-fixed quads painted on a wall) lie on
     * the surface behind them, so a soft fade erases them. Seen nearly head-on they face the
     * camera, but unlike the title's billboards they are not in the view plane. */
    SoftView level = {1.4f, 2.0f, -1.0f, {0, 1, 0}, 1};   /* level camera: world up = view y */
    for (int deg = 2; deg <= 23; deg += 7) {
        const float a = (float)deg * 3.14159265f / 180.0f;
        const float wall_x[3] = {0.5f * cosf(a), 0, 0.5f * sinf(a)};   /* turned deg degrees about the view y axis */
        const float tag_c[3] = {0, 0, -6};
        CHECK(one(tag_c, wall_x, y5, &level) == 0, "a wall tag %d degrees off head-on (level camera) is not soft", deg);
        CHECK(one(tag_c, wall_x, y5, &v) == 0, "a tag turned %d degrees from the view plane (camera pitched 20 degrees) is not soft", deg);
    }
    {   /* The tag camera of the Destroyers mural (level87 TagCam 9): 10.7 degrees up, the tag ahead. */
        const float p = 10.68f * 3.14159265f / 180.0f;
        SoftView tagcam = {1.4f, 2.0f, -1.0f, {0, cosf(p), -sinf(p)}, 1};
        const float wall_y[3] = {0, 0.5f * cosf(p), 0.5f * sinf(p)};   /* world up in view space, half length */
        const float tag_c[3] = {0.3f, 0.6f, -6.4f};
        CHECK(one(tag_c, x5, wall_y, &tagcam) == 0, "the mural tag under its tag camera is not soft");
    }
    {   /* A quad facing the eye but not in the view plane (off the centre): not a title billboard. */
        const float c[3] = {3, 0, -5}, len = sqrtf(34.0f);
        const float ax[3] = {0.5f * 5 / len, 0, 0.5f * 3 / len};   /* perpendicular to the eye ray in x-z */
        CHECK(one(c, ax, y5, &v) == 0, "a quad facing the eye ray off the view plane is not soft");
    }
    const float jit[3] = {0.5f, 0, 0.002f};   /* a billboard built from rounded floats: 0.2 degrees off */
    CHECK(one(ahead, jit, y5, &v) == 1, "a billboard 0.2 degrees off the view plane is soft");

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

    /* World-fixed sprites. The graffiti tags share the particle texture atlas with the smoke, fire
     * and dust billboards, but each sprite (texture and uv rectangle) is either always built in the
     * view plane (a billboard) or fixed in the world (a tag). A tag seen within 1.8 degrees of
     * head-on lies in the view plane like a billboard; once any quad of its sprite has been seen off
     * the view plane (and not lying level), every quad of that sprite stays hard, whatever the angle. */
    {
        static SoftSprites ws;
        const uint64_t atlas = 0x0123456789abcdefull, other = 0x0fedcba987654321ull;
        const float tag_c[3] = {0, 0, -6};
        const float a10 = 10.0f * 3.14159265f / 180.0f;
        const float wall10[3] = {0.5f * cosf(a10), 0, 0.5f * sinf(a10)};
        CHECK(one_sprite(tag_c, x5, y5, &level, atlas, 0.25f, 0.5f, 0.5f, 0.75f, &ws) == 1, "an unknown sprite in the view plane is soft");
        CHECK(one_sprite(tag_c, wall10, y5, &level, atlas, 0.25f, 0.5f, 0.5f, 0.75f, &ws) == 0, "a tag 10 degrees off head-on is not soft");
        CHECK(ws.n == 1, "the tag's sprite is learnt as world-fixed (%u)", ws.n);
        CHECK(one_sprite(tag_c, x5, y5, &level, atlas, 0.25f, 0.5f, 0.5f, 0.75f, &ws) == 0, "the same tag seen head-on is not soft");
        CHECK(one_sprite(tag_c, jit, y5, &v, atlas, 0.25f, 0.5f, 0.5f, 0.75f, &ws) == 0,
              "the same tag 0.2 degrees off head-on (pitched camera) is not soft");
        CHECK(one_sprite(tag_c, x5, y5, &level, atlas, 0.5f, 0.5f, 0.75f, 0.75f, &ws) == 1, "a smoke sprite of the same atlas is still soft");
        CHECK(one_sprite(tag_c, x5, y5, &level, other, 0.25f, 0.5f, 0.5f, 0.75f, &ws) == 1, "the same rectangle of another texture is still soft");
        for (int i = 0; i < 1000; i++) {   /* billboards within 0.8 degrees of the view plane, many frames */
            const float t = (float)(i % 7) * 0.001f;
            const float bx[3] = {0.5f, 0, t};
            one_sprite(side, bx, y5, &v, atlas, 0, 0, 0.125f, 0.125f, &ws);
        }
        CHECK(ws.n == 1 && one_sprite(ahead, x5, y5, &v, atlas, 0, 0, 0.125f, 0.125f, &ws) == 1, "billboards are never learnt (%u)", ws.n);
        CHECK(one_sprite(floor_c, x5, fwd, &v, atlas, 0.75f, 0, 1, 0.25f, &ws) == 0 && ws.n == 1, "a ground decal is not learnt (%u)", ws.n);
        CHECK(one_sprite(ahead, x5, zero, &v, atlas, 0.75f, 0.25f, 1, 0.5f, &ws) == 0 && ws.n == 1, "a degenerate quad is not learnt (%u)", ws.n);
        CHECK(one_sprite(tag_c, x5, y5, &level, atlas, 0.25f, 0.5f, 0.5f, 0.75f, NULL) == 1, "without sprites the angle rule alone decides");
        CHECK(soft_sprite_key(atlas, 0.25f, 0.5f, 0.5f, 0.75f) != soft_sprite_key(atlas, 0.5f, 0.5f, 0.75f, 0.75f) &&
                  soft_sprite_key(atlas, 0.25f, 0.5f, 0.5f, 0.75f) == soft_sprite_key(atlas, 0.25f + 1e-6f, 0.5f, 0.5f, 0.75f - 1e-6f),
              "sprite keys: rectangles differ, rounding noise does not");

        /* One draw: a tag quad head-on before a quad of the same sprite seen off the view plane
         * (two layers of a tag on a bent wall). Both stay hard. */
        float x[2][16], vb[8 * 6];
        quad(x[0], tag_c, x5, y5, &level);
        quad(x[1], tag_c, wall10, y5, &level);
        for (int k = 0; k < 8; k++) {
            memcpy(&vb[k * 6], &x[k / 4][(k % 4) * 4], 16);
            vb[k * 6 + 4] = (k % 4 == 1 || k % 4 == 2) ? 0.875f : 0.75f;
            vb[k * 6 + 5] = k % 4 >= 2 ? 0.75f : 1.0f;
        }
        const uint32_t idx[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        uint8_t s[2] = {7, 7};
        CHECK(soft_classify_sprites(vb, 6, 4, idx, 12, &level, atlas, &ws, s) == 2 && s[0] == 0 && s[1] == 0,
              "a head-on quad before an oblique one of the same sprite in one draw: %u %u", s[0], s[1]);

        static SoftSprites full;
        for (uint32_t i = 0; i < SOFT_SPRITE_SLOTS + 10; i++)
            one_sprite(tag_c, wall10, y5, &level, atlas, (float)(i % 1024) / 1024, (float)(i / 1024) / 1024, 1, 1, &full);
        CHECK(full.n <= SOFT_SPRITE_MAX, "the sprite set stops learning when full (%u)", full.n);
        CHECK(one_sprite(tag_c, x5, y5, &level, atlas, 0, 0, 1, 1, &full) == 0, "a learnt sprite stays hard in a full set");
    }

    if (!fails) printf("ALL OK\n");
    return fails ? 1 : 0;
}
