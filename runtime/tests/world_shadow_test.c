/* Game-independent test of the --world-shadows options and light fit (src/world_shadow.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/world_shadow.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A D3D-style camera at eye looking along yaw (radians from +x, level), 60 x 34 degrees, world z up:
 * clip x = P0 view x, y = P5 view y, z = A view z + B, w = view z. */
static void camera(const double eye[3], double yaw, float m[4][4]) {
    const double fw[3] = {cos(yaw), sin(yaw), 0}, right[3] = {sin(yaw), -cos(yaw), 0}, up[3] = {0, 0, 1};
    const double p0 = 1 / tan(30 * M_PI / 180), p5 = p0 * 16 / 9, A = 1.001, B = -0.1;
    const double *ax[4] = {right, up, fw, fw};
    const double k[4] = {p0, p5, A, 1}, add[4] = {0, 0, B, 0};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) m[i][j] = (float)(k[i] * ax[i][j]);
        m[i][3] = (float)(-k[i] * (ax[i][0] * eye[0] + ax[i][1] * eye[1] + ax[i][2] * eye[2]) + add[i]);
    }
}
static void clip_of(const float m[4][4], const double p[3], double q[4]) {
    for (int i = 0; i < 4; i++) q[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3];
}
/* The map position (NDC x, y, depth) of world point p through the renderer's path: clip, then clip_to_map. */
static void map_of(const RWorldShadow *o, const float m[4][4], const double p[3], double r[3]) {
    double q[4];
    clip_of(m, p, q);
    const double v[4] = {q[0], q[1], q[3], 1};
    for (int i = 0; i < 3; i++) r[i] = o->clip_to_map[i][0] * v[0] + o->clip_to_map[i][1] * v[1] + o->clip_to_map[i][2] * v[2] + o->clip_to_map[i][3];
}

int main(void) {
    WorldShadowConfig c = WORLD_SHADOW_DEFAULTS;
    CHECK(!c.on && c.strength == 1.0f && c.size == 2048 && c.range == 45.0f, "defaults: off, strength 1, 2048, 45 (%.2f %d %.1f)", c.strength,
          c.size, c.range);
    CHECK(world_shadow_arg(&c, "--lamp-shadows", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(world_shadow_arg(&c, "--world-shadows", NULL) == 1 && c.on, "--world-shadows turns it on");
    CHECK(world_shadow_arg(&c, "--no-world-shadows", NULL) == 1 && !c.on, "--no-world-shadows turns it off (the last one wins)");
    CHECK(world_shadow_arg(&c, "--world-shadows-strength", "0.6") == 2 && c.strength == 0.6f && !c.on, "--world-shadows-strength sets the strength only");
    CHECK(world_shadow_arg(&c, "--world-shadows-size", "1024") == 2 && c.size == 1024 && !c.on, "--world-shadows-size sets the size only");
    CHECK(world_shadow_arg(&c, "--world-shadows-range", "30") == 2 && c.range == 30.0f && !c.on, "--world-shadows-range sets the range only");
    CHECK(world_shadow_arg(&c, "--world-shadows-size", "512") == 2 && world_shadow_arg(&c, "--world-shadows-size", "4096") == 2 && c.size == 4096,
          "sizes 512-4096");
    const char *bad_s[] = {"0", "0.05", "-1", "1.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 9; i++) {
        WorldShadowConfig d = WORLD_SHADOW_DEFAULTS;
        CHECK(world_shadow_arg(&d, "--world-shadows-strength", bad_s[i]) == -1 && d.strength == WORLD_SHADOW_STRENGTH_DEFAULT,
              "--world-shadows-strength '%s' is rejected", bad_s[i] ? bad_s[i] : "(missing)");
    }
    const char *bad_z[] = {"0", "256", "1000", "2047", "8192", "2048x", "", NULL};
    for (int i = 0; i < 8; i++) {
        WorldShadowConfig d = WORLD_SHADOW_DEFAULTS;
        CHECK(world_shadow_arg(&d, "--world-shadows-size", bad_z[i]) == -1 && d.size == WORLD_SHADOW_SIZE_DEFAULT,
              "--world-shadows-size '%s' is rejected", bad_z[i] ? bad_z[i] : "(missing)");
    }
    const char *bad_r[] = {"10", "121", "-5", "far", "", NULL};
    for (int i = 0; i < 6; i++) {
        WorldShadowConfig d = WORLD_SHADOW_DEFAULTS;
        CHECK(world_shadow_arg(&d, "--world-shadows-range", bad_r[i]) == -1 && d.range == WORLD_SHADOW_RANGE_DEFAULT,
              "--world-shadows-range '%s' is rejected", bad_r[i] ? bad_r[i] : "(missing)");
    }

    /* The fit. */
    WorldShadowConfig on = WORLD_SHADOW_DEFAULTS;
    on.on = 1;
    const float sun[3] = {-500, -500, -850};   /* the default xshadowdir, z down */
    const double eye[3] = {10, 20, 2};
    float m[4][4];
    camera(eye, 0.3, m);
    RWorldShadow o, keep;
    memset(&keep, 0x5A, sizeof keep);
    o = keep;
    CHECK(world_shadow_build(&c, (const float (*)[4])m, sun, NULL, 0, &o) == 0 && !memcmp(&o, &keep, sizeof o), "off: nothing built");
    const float none[3] = {0, 0, 0};
    CHECK(world_shadow_build(&on, (const float (*)[4])m, none, NULL, 0, &o) == 0 && !memcmp(&o, &keep, sizeof o), "no light (c90 = 0): nothing built");
    float flat[4][4] = {{0}};
    CHECK(world_shadow_build(&on, (const float (*)[4])flat, sun, NULL, 0, &o) == 0 && !memcmp(&o, &keep, sizeof o), "no camera: nothing built");
    CHECK(world_shadow_build(&on, (const float (*)[4])m, sun, NULL, 0, &o) == 1, "a camera and a light: built");
    CHECK(fabs(o.eye[0] - 10) < 1e-3 && fabs(o.eye[1] - 20) < 1e-3 && fabs(o.eye[2] - 2) < 1e-3, "the camera position (%.3f %.3f %.3f)", o.eye[0], o.eye[1],
          o.eye[2]);
    /* A point 20 units ahead on the ground: inside the map; the clip -> world rows give it back. */
    const double p[3] = {eye[0] + 20 * cos(0.3), eye[1] + 20 * sin(0.3), 0};
    double q[4], mp[3];
    clip_of(m, p, q);
    double w[3];
    for (int i = 0; i < 3; i++) w[i] = o.clip_to_world[i][0] * q[0] + o.clip_to_world[i][1] * q[1] + o.clip_to_world[i][2] * q[3] + o.clip_to_world[i][3];
    CHECK(fabs(w[0] - p[0]) < 1e-3 && fabs(w[1] - p[1]) < 1e-3 && fabs(w[2] - p[2]) < 1e-3, "clip -> world (%.4f %.4f %.4f)", w[0], w[1], w[2]);
    map_of(&o, m, p, mp);
    CHECK(fabs(mp[0]) < 1 && fabs(mp[1]) < 1 && mp[2] > 0 && mp[2] < 1, "a ground point ahead is in the map (%.3f %.3f %.3f)", mp[0], mp[1], mp[2]);
    /* The same ground point and one 6 units back towards the light (the top of a wall casting onto
     * it): the same map place, nearer the light. */
    const double l = sqrt(500.0 * 500 * 2 + 850.0 * 850);
    const double top[3] = {p[0] + 6 * 500 / l, p[1] + 6 * 500 / l, p[2] + 6 * 850 / l};
    double mt[3];
    map_of(&o, m, top, mt);
    CHECK(fabs(mt[0] - mp[0]) < 1e-4 && fabs(mt[1] - mp[1]) < 1e-4 && mt[2] < mp[2] - 0.01, "along the light: the same texel, nearer (%.5f vs %.5f)", mt[2],
          mp[2]);
    CHECK(o.bias > 0 && o.bias < 0.002f, "a small depth bias (%.6f)", o.bias);
    /* A point 4 units off the light's path: about 4 / r of the map's half-width away, r the view
     * sphere's radius (for 45 units of a 60-degree view, 25 to 45 units). */
    const double off[3] = {p[0] + 4 * 500 / sqrt(2.0) / 500, p[1] - 4 / sqrt(2.0), p[2]};   /* across x = -y */
    double mo[3];
    map_of(&o, m, off, mo);
    const double moved = hypot(mo[0] - mp[0], mo[1] - mp[1]);
    CHECK(moved > 4.0 / 45 && moved < 4.0 / 25, "across the light: about 4 / r of the map (%.4f)", moved);
    /* Ahead of the camera, behind it and out to the range: covered (the sphere of the view). */
    const double far_pt[3] = {eye[0] + 44 * cos(0.3), eye[1] + 44 * sin(0.3), 0};
    double mf[3];
    map_of(&o, m, far_pt, mf);
    CHECK(fabs(mf[0]) < 1 && fabs(mf[1]) < 1, "44 units ahead is still in the map (%.3f %.3f)", mf[0], mf[1]);
    CHECK(o.fade_far == 45.0f && fabsf(o.fade_near - 0.75f * 45) < 1e-4f, "fades out to the range (%.2f-%.2f)", o.fade_near, o.fade_far);
    CHECK(fabsf(o.darken - 0.3f) < 1e-5f && o.baked_lo < o.baked_hi && o.lamp_keep > 0 && o.lamp_keep <= 1,
          "full strength: the title's darkness 1 - 0.7 (%.3f)", o.darken);
    /* Texel snapping: a camera moved a little keeps a still point at the same place within its
     * texel (whole texels only), so the map does not shimmer. */
    const double eye2[3] = {eye[0] + 0.013, eye[1] - 0.021, eye[2] + 0.004};
    float m2[4][4];
    camera(eye2, 0.3, m2);
    RWorldShadow o2;
    CHECK(world_shadow_build(&on, (const float (*)[4])m2, sun, NULL, 0, &o2) == 1, "moved camera: built");
    double mp2[3];
    map_of(&o2, m2, p, mp2);
    const double tx = (mp[0] + 1) * on.size / 2, tx2 = (mp2[0] + 1) * on.size / 2, ty = (mp[1] + 1) * on.size / 2, ty2 = (mp2[1] + 1) * on.size / 2;
    CHECK(fabs((tx - tx2) - round(tx - tx2)) < 2e-3 && fabs((ty - ty2) - round(ty - ty2)) < 2e-3, "moved camera: whole texels (%.4f %.4f)", tx - tx2,
          ty - ty2);
    /* Turning the camera keeps the map's scale (the sphere's radius depends on the field of view only). */
    float m3[4][4];
    camera(eye, 1.4, m3);
    RWorldShadow o3;
    CHECK(world_shadow_build(&on, (const float (*)[4])m3, sun, NULL, 0, &o3) == 1, "turned camera: built");
    double a3[3], b3[3], a1[3], b1[3];
    const double pa[3] = {eye[0] + 2, eye[1] + 3, 0}, pb[3] = {eye[0] + 5, eye[1] - 1, 0};
    map_of(&o, m, pa, a1);
    map_of(&o, m, pb, b1);
    map_of(&o3, m3, pa, a3);
    map_of(&o3, m3, pb, b3);
    CHECK(fabs(hypot(a1[0] - b1[0], a1[1] - b1[1]) - hypot(a3[0] - b3[0], a3[1] - b3[1])) < 1e-5, "turned camera: the same map scale");
    /* Strength and the gate scale the darkening; lamps are kept up to the renderer's count. */
    on.strength = 0.5f;
    float lamps[10][4];
    for (int i = 0; i < 10; i++) {
        lamps[i][0] = (float)i;
        lamps[i][1] = 1;
        lamps[i][2] = 4;
        lamps[i][3] = i == 2 ? 0 : 6;
    }
    CHECK(world_shadow_build(&on, (const float (*)[4])m, sun, (const float (*)[4])lamps, 10, &o) == 1 && fabsf(o.darken - 0.15f) < 1e-5f,
          "strength 0.5: half the darkening (%.4f)", o.darken);
    CHECK(o.lamps == RENDERER_WORLD_SHADOW_LAMPS && o.lamp[2][0] == 3 && o.lamp[0][3] == 6, "lamps: radius 0 dropped, 8 kept (%u)", o.lamps);
    /* A light straight down still has a frame. */
    const float down[3] = {0, 0, -1};
    CHECK(world_shadow_build(&on, (const float (*)[4])m, down, NULL, 0, &o) == 1, "a light straight down: built");
    map_of(&o, m, p, mp);
    CHECK(fabs(mp[0]) < 1 && fabs(mp[1]) < 1 && mp[2] > 0 && mp[2] < 1, "straight down: in the map (%.3f %.3f %.3f)", mp[0], mp[1], mp[2]);

    /* The indoor gate's inputs: world -> map agrees with the clip path, the roof clearance is 0.5 units. */
    on.strength = 1;
    CHECK(world_shadow_build(&on, (const float (*)[4])m, sun, NULL, 0, &o) == 1 && o.dt == 0, "rebuilt (dt left to the caller)");
    map_of(&o, m, p, mp);
    double wm[3];
    for (int i = 0; i < 3; i++) wm[i] = o.world_to_map[i][0] * p[0] + o.world_to_map[i][1] * p[1] + o.world_to_map[i][2] * p[2] + o.world_to_map[i][3];
    CHECK(fabs(wm[0] - mp[0]) < 1e-4 && fabs(wm[1] - mp[1]) < 1e-4 && fabs(wm[2] - mp[2]) < 1e-5, "world -> map = clip -> map (%.5f %.5f %.5f)", wm[0], wm[1], wm[2]);
    const double up5[3] = {p[0] + 0.5 * 500 / l, p[1] + 0.5 * 500 / l, p[2] + 0.5 * 850 / l};   /* 0.5 units nearer the light along it */
    double mu[3];
    map_of(&o, m, up5, mu);
    CHECK(fabs((mp[2] - mu[2]) - o.clearance) < 1e-5, "clearance: 0.5 units along the light (%.6f vs %.6f)", mp[2] - mu[2], o.clearance);
    if (fails) {
        printf("world_shadow_test: %d FAILED\n", fails);
        return 1;
    }
    printf("world_shadow_test: ALL OK\n");
    return 0;
}
