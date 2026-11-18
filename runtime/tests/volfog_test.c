/* Game-independent test of the --volfog options and the helpers the renderer's shaders share
 * (src/volfog.c): grid, slice distribution, far plane and fog fade, density rules, light cap and
 * order, falloff, camera reprojection and cuts, jitter. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/geom.h"
#include "../src/volfog.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    VolfogConfig c = VOLFOG_DEFAULTS;
    CHECK(!c.on && c.quality == VOLFOG_MEDIUM && c.strength == VOLFOG_STRENGTH_DEFAULT, "defaults: off, medium, strength %.2f", c.strength);
    CHECK(volfog_arg(&c, "--light-shafts", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(volfog_arg(&c, "--volfog", NULL) == 1 && c.on, "--volfog turns it on");
    CHECK(volfog_arg(&c, "--no-volfog", NULL) == 1 && !c.on, "--no-volfog turns it off");
    CHECK(volfog_arg(&c, "--volfog-quality", "high") == 2 && c.quality == VOLFOG_HIGH && !c.on, "--volfog-quality high picks the quality only");
    CHECK(volfog_arg(&c, "--volfog-quality", "low") == 2 && c.quality == VOLFOG_LOW, "low");
    CHECK(volfog_arg(&c, "--volfog-quality", "medium") == 2 && c.quality == VOLFOG_MEDIUM, "medium");
    const char *badq[] = {"ultra", "", "HIGH", "2", NULL};
    for (int i = 0; i < 5; i++) {
        VolfogConfig d = VOLFOG_DEFAULTS;
        CHECK(volfog_arg(&d, "--volfog-quality", badq[i]) == -1 && d.quality == VOLFOG_MEDIUM, "quality '%s' is rejected", badq[i] ? badq[i] : "(missing)");
    }
    CHECK(volfog_arg(&c, "--volfog-strength", "2.5") == 2 && c.strength == 2.5f && !c.on, "strength sets the strength only");
    CHECK(volfog_arg(&c, "--volfog-strength", "0.1") == 2 && volfog_arg(&c, "--volfog-strength", "3") == 2 && c.strength == 3.0f, "strength range 0.1-3");
    const char *bad[] = {"0", "-1", "3.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        VolfogConfig d = VOLFOG_DEFAULTS;
        CHECK(volfog_arg(&d, "--volfog-strength", bad[i]) == -1 && d.strength == VOLFOG_STRENGTH_DEFAULT, "strength '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }
    CHECK(!strcmp(volfog_quality_name(VOLFOG_LOW), "low") && !strcmp(volfog_quality_name(VOLFOG_HIGH), "high") && !strcmp(volfog_quality_name(7), "medium"),
          "quality names");
}

static void test_grid(void) {
    const VolfogGrid lo = volfog_grid(VOLFOG_LOW), me = volfog_grid(VOLFOG_MEDIUM), hi = volfog_grid(VOLFOG_HIGH), bad = volfog_grid(-1);
    CHECK(lo.w == 96 && lo.h == 54 && lo.d == 32 && lo.lights == 12, "low 96x54x32, 12 lights (%dx%dx%d, %d)", lo.w, lo.h, lo.d, lo.lights);
    CHECK(me.w == 128 && me.h == 72 && me.d == 48 && me.lights == 24, "medium 128x72x48, 24 lights");
    CHECK(hi.w == 160 && hi.h == 90 && hi.d == 64 && hi.lights == VOLFOG_LIGHTS_MAX, "high 160x90x64, 48 lights");
    CHECK(bad.w == me.w && bad.d == me.d, "an invalid quality is medium");
}

static void test_slices(void) {
    const int d = 64;
    const float n = VOLFOG_NEAR, f = 80;
    CHECK(fabsf(volfog_slice_depth(0, d, n, f) - n) < 1e-5f && fabsf(volfog_slice_depth((float)d, d, n, f) - f) < 1e-3f, "slices span near to far");
    float prev = 0;
    int mono = 1;
    for (int i = 0; i <= d; i++) {
        const float z = volfog_slice_depth((float)i, d, n, f);
        if (!(z > prev)) mono = 0;
        prev = z;
        if (fabsf(volfog_depth_slice(z, d, n, f) - i) > 1e-3f) mono = 0;
    }
    CHECK(mono, "slices grow with depth and depth_slice inverts slice_depth");
    /* Exponential: equal ratios, so the near metres get many more slices than the far ones. */
    const float r0 = volfog_slice_depth(1, d, n, f) / volfog_slice_depth(0, d, n, f), r1 = volfog_slice_depth(50, d, n, f) / volfog_slice_depth(49, d, n, f);
    CHECK(fabsf(r0 - r1) < 1e-3f, "constant ratio between slices (%.4f, %.4f)", r0, r1);
    CHECK(volfog_depth_slice(5, d, n, f) > 0.4f * d, "the first 5 m take over 40 %% of the slices (%.1f)", volfog_depth_slice(5, d, n, f));
    CHECK(volfog_depth_slice(0.1f, d, n, f) == 0 && volfog_depth_slice(500, d, n, f) == (float)d, "depths outside the range clamp");
}

static void test_far_and_fog(void) {
    CHECK(volfog_far(0, GEOM_FOG_LINEAR, 0, 60, 0) == VOLFOG_FAR_MAX, "no fog: the longest range");
    CHECK(fabsf(volfog_far(1, GEOM_FOG_LINEAR, 10, 60, 0) - 42) < 1e-3f, "linear fog ending at 60: 0.7 x 60 = 42");
    CHECK(volfog_far(1, GEOM_FOG_LINEAR, 10, 400, 0) == VOLFOG_FAR_MAX, "a far fog end: capped at 80");
    CHECK(volfog_far(1, GEOM_FOG_LINEAR, 0, 5, 0) == VOLFOG_FAR_MIN, "a very near fog: at least 10");
    const float fe = volfog_far(1, GEOM_FOG_EXP, 0, 0, 0.05f);
    CHECK(fabsf(geom_fog_factor(GEOM_FOG_EXP, 0, 0, 0.05f, fe / VOLFOG_FAR_FOG) - 0.05f) < 1e-3f, "exp fog: 0.7 x where its factor is 5 %% (%.1f)", fe);
    const float f2 = volfog_far(1, GEOM_FOG_EXP2, 0, 0, 0.03f);
    CHECK(fabsf(geom_fog_factor(GEOM_FOG_EXP2, 0, 0, 0.03f, f2 / VOLFOG_FAR_FOG) - 0.05f) < 1e-3f, "exp2 fog likewise (%.1f)", f2);
    CHECK(volfog_far(1, GEOM_FOG_EXP, 0, 0, 0) == VOLFOG_FAR_MAX && volfog_far(1, 99, 0, 0, 1) == VOLFOG_FAR_MAX, "no density or an unknown mode: the longest range");
    CHECK(volfog_fog_keep(0, GEOM_FOG_LINEAR, 0, 10, 0, 50) == 1, "no fog keeps everything");
    CHECK(fabsf(volfog_fog_keep(1, GEOM_FOG_LINEAR, 0, 40, 0, 30) - 0.5f) < 1e-4f, "linear fog a quarter left: half the glow kept");
    CHECK(volfog_fog_keep(1, GEOM_FOG_LINEAR, 0, 40, 0, 50) == 0 && volfog_fog_keep(1, GEOM_FOG_LINEAR, 10, 40, 0, 5) == 1, "past the end nothing, before the start all");
}

static void test_density(void) {
    const float base = volfog_density(1, 0, 0, 0);
    CHECK(fabsf(base - VOLFOG_DENSITY) < 1e-6f, "dry outdoor air: the base density");
    CHECK(fabsf(volfog_density(2, 0, 0, 0) - 2 * base) < 1e-6f, "x strength");
    CHECK(fabsf(volfog_density(1, 1, 0, 0) / base - 2.5f) < 1e-4f, "rain: x2.5");
    CHECK(fabsf(volfog_density(1, 0, 1, 0) / base - 1.3f) < 1e-4f, "damp streets: x1.3");
    CHECK(fabsf(volfog_density(1, 0, 0, 1) / base - 0.4f) < 1e-4f, "indoors: x0.4");
    CHECK(volfog_density(1, 0.5f, 0, 0) > base && volfog_density(1, 0.5f, 0, 0) < volfog_density(1, 1, 0, 0), "eased rain in between");
}

static VolfogLight vl(float z, int kind, float intensity, float radius) {
    VolfogLight l = {{0, 2, z}, radius, {1, 0.8f, 0.6f}, intensity, kind};
    return l;
}

static void test_select(void) {
    VolfogLight in[8], out[8];
    in[0] = vl(-10, VOLFOG_LAMP, 1, 10);
    in[1] = vl(-20, VOLFOG_NEON, 1, 1);
    in[2] = vl(-5, VOLFOG_FIRE, 1, 6);
    in[3] = vl(-12, VOLFOG_LAMP, 0, 10);    /* off */
    in[4] = vl(30, VOLFOG_LAMP, 1, 10);     /* 30 behind the camera, reach 10 */
    in[5] = vl(-120, VOLFOG_LAMP, 1, 10);   /* past the far plane */
    in[6] = vl(-15, VOLFOG_LAMP, 1, 60);    /* a huge reach */
    in[7] = vl(5, VOLFOG_LAMP, 1, 10);      /* just behind, its light reaches in front */
    uint32_t n = volfog_select(in, 8, 8, 80, out);
    CHECK(n == 5, "5 lights kept (%u)", n);
    CHECK(n == 5 && out[0].kind == VOLFOG_FIRE && out[1].pos[2] == -10 && out[2].pos[2] == -15 && out[3].pos[2] == 5 && out[4].kind == VOLFOG_NEON,
          "fires first, then the lamps in order, then neon");
    CHECK(n == 5 && out[1].radius == 10 * VOLFOG_LAMP_REACH, "a lamp's reach in the air: x%.1f (%.1f)", VOLFOG_LAMP_REACH, out[1].radius);
    CHECK(n == 5 && out[2].radius == VOLFOG_RADIUS_MAX, "radius capped at %.0f (%.1f)", VOLFOG_RADIUS_MAX, out[2].radius);
    CHECK(n == 5 && out[4].radius == VOLFOG_NEON_RADIUS_MIN, "neon radius at least %.0f (%.1f)", VOLFOG_NEON_RADIUS_MIN, out[4].radius);
    n = volfog_select(in, 8, 2, 80, out);
    CHECK(n == 2 && out[0].kind == VOLFOG_FIRE && out[1].pos[2] == -10, "the cap keeps the first ones (%u)", n);
    n = volfog_select(in, 8, 100, 80, out);
    CHECK(n == 5, "a cap above VOLFOG_LIGHTS_MAX is fine");
    VolfogLight big[VOLFOG_LIGHTS_MAX + 10], bout[VOLFOG_LIGHTS_MAX + 10];
    for (int i = 0; i < VOLFOG_LIGHTS_MAX + 10; i++) big[i] = vl(-10, VOLFOG_LAMP, 1, 5);
    CHECK(volfog_select(big, VOLFOG_LIGHTS_MAX + 10, 1000, 80, bout) == VOLFOG_LIGHTS_MAX, "never more than VOLFOG_LIGHTS_MAX");
    VolfogLight small = vl(-10, VOLFOG_LAMP, 1, 2);
    CHECK(volfog_select(&small, 1, 4, 80, out) == 1 && out[0].radius == VOLFOG_REACH_MIN, "a small lamp still glows over %.0f (%.1f)", VOLFOG_REACH_MIN, out[0].radius);
    VolfogLight neon = vl(-10, VOLFOG_NEON, 1, 20);
    CHECK(volfog_select(&neon, 1, 4, 80, out) == 1 && out[0].radius == VOLFOG_NEON_RADIUS_MAX, "neon radius at most %.0f", VOLFOG_NEON_RADIUS_MAX);
    CHECK(volfog_phase_g(VOLFOG_LAMP) > volfog_phase_g(VOLFOG_NEON) && volfog_phase_g(VOLFOG_NEON) > volfog_phase_g(VOLFOG_FIRE), "phase: lamp > neon > fire");
}

static void test_falloff(void) {
    CHECK(volfog_falloff(10, 10) == 0 && volfog_falloff(12, 10) == 0, "nothing from the radius on");
    CHECK(volfog_falloff(0, 10) == volfog_falloff(VOLFOG_R_MIN, 10), "clamped at the core");
    float prev = 2;
    int dec = 1;
    for (float r = VOLFOG_R_MIN; r < 10; r += 0.25f) {
        const float f = volfog_falloff(r, 10);
        if (!(f < prev) || f < 0) dec = 0;
        prev = f;
    }
    CHECK(dec, "decreasing with distance");
    CHECK(volfog_falloff(5, 10) < 0.25f * volfog_falloff(1, 10), "most of the light near the lamp");
}

/* A world-to-view matrix (rows, v = w x V) of a camera at eye turned yaw degrees about world z. */
static void view_of(float eye[3], float yaw, float V[16]) {
    const float a = yaw * 3.14159265f / 180, c = cosf(a), s = sinf(a);
    /* rows of R: world axis i in view space */
    const float R[3][3] = {{c, 0, -s}, {s, 0, c}, {0, 1, 0}};
    memset(V, 0, 16 * sizeof(float));
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) V[4 * i + j] = R[i][j];
    for (int j = 0; j < 3; j++) V[12 + j] = -(eye[0] * R[0][j] + eye[1] * R[1][j] + eye[2] * R[2][j]);
    V[15] = 1;
}
static void to_view(const float V[16], const float w[3], float v[3]) {
    for (int j = 0; j < 3; j++) v[j] = w[0] * V[j] + w[1] * V[4 + j] + w[2] * V[8 + j] + V[12 + j];
}

static void test_reproject(void) {
    float e0[3] = {10, 5, 2}, e1[3] = {10.5f, 5.2f, 2}, P[16], C[16], M[3][4];
    view_of(e0, 20, P);
    view_of(e1, 28, C);
    CHECK(volfog_reproject(P, C, M) == 0, "a small move is not a cut");
    const float w[3] = {14, 9, 1};
    float vc[3], vp[3];
    to_view(C, w, vc);
    to_view(P, w, vp);
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = M[i][0] * vc[0] + M[i][1] * vc[1] + M[i][2] * vc[2] + M[i][3];
    CHECK(fabsf(r[0] - vp[0]) < 1e-3f && fabsf(r[1] - vp[1]) < 1e-3f && fabsf(r[2] - vp[2]) < 1e-3f, "a world point maps to its previous view position");
    float e2[3] = {16, 5, 2};
    view_of(e2, 20, C);
    CHECK(volfog_reproject(P, C, M) == 1, "a 6 m jump is a cut");
    view_of(e0, 70, C);
    CHECK(volfog_reproject(P, C, M) == 1, "a 50 degree turn is a cut");
    view_of(e0, 20, C);
    CHECK(volfog_reproject(P, C, M) == 0 && fabsf(M[0][0] - 1) < 1e-5f && fabsf(M[0][3]) < 1e-5f, "the same camera: identity");
    float Z[16] = {0};
    CHECK(volfog_reproject(P, Z, M) == 1, "a singular matrix is a cut");
}

static void test_jitter(void) {
    float a[3], b[3], sum[3] = {0, 0, 0};
    volfog_jitter(0, a);
    volfog_jitter(16, b);
    CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2], "16-frame cycle");
    int in = 1;
    for (uint32_t f = 0; f < 16; f++) {
        volfog_jitter(f, a);
        for (int i = 0; i < 3; i++) {
            if (!(a[i] > 0 && a[i] < 1)) in = 0;
            sum[i] += a[i];
        }
    }
    CHECK(in, "offsets inside the froxel");
    CHECK(fabsf(sum[0] / 16 - 0.5f) < 0.08f && fabsf(sum[1] / 16 - 0.5f) < 0.08f && fabsf(sum[2] / 16 - 0.5f) < 0.08f, "evenly spread (%.2f %.2f %.2f)",
          sum[0] / 16, sum[1] / 16, sum[2] / 16);
    volfog_jitter(1, b);
    volfog_jitter(2, a);
    CHECK(a[2] != b[2], "the depth offset changes every frame");
}

/* Stage 2: a street lamp's light in a defined cone below it, little above its head. */
static void test_lamp_shape(void) {
    const float below = volfog_lamp_shape(1.0f), above = volfog_lamp_shape(-1.0f), side = volfog_lamp_shape(0.0f);
    const float edge_in = volfog_lamp_shape(VOLFOG_CONE_COS_INNER), edge_out = volfog_lamp_shape(VOLFOG_CONE_COS_OUTER - 0.05f);
    CHECK(below >= 8 * above, "below the lamp at least 8x the light above it (%.3f, %.3f)", below, above);
    CHECK(below >= 4 * side, "in the cone at least 4x the light beside the lamp (%.3f, %.3f)", below, side);
    CHECK(edge_in >= 3 * edge_out, "the cone has a defined edge (%.3f inside, %.3f just outside)", edge_in, edge_out);
    CHECK(above > 0 && above <= 0.1f, "a faint glow above the head (%.3f)", above);
    float prev = -1;
    int mono = 1;
    for (int i = 0; i <= 40; i++) {
        const float s = volfog_lamp_shape(-1.0f + i * 0.05f);
        if (s < prev - 1e-6f) mono = 0;
        prev = s;
    }
    CHECK(mono, "the light grows from above the head to straight down");
    /* In the air: 3 units below a lamp of reach 9 far more than 3 above or beside it, and the cone
     * keeps its light half way to its reach (no bright ball round the head alone). */
    const float lb = volfog_lamp_light(1, 3, 9), la = volfog_lamp_light(-1, 3, 9), ls = volfog_lamp_light(0, 3, 9);
    CHECK(lb >= 8 * la && lb >= 4 * ls, "3 below %.3f, above %.3f, beside %.3f", lb, la, ls);
    CHECK(volfog_lamp_light(1, 4.5f, 9) >= 0.4f * volfog_lamp_light(1, VOLFOG_R_MIN, 9), "the cone reaches down (%.3f at half its reach, %.3f at the head)",
          volfog_lamp_light(1, 4.5f, 9), volfog_lamp_light(1, VOLFOG_R_MIN, 9));
    CHECK(volfog_lamp_light(1, 9, 9) == 0 && volfog_lamp_light(1, 10, 9) == 0 && volfog_lamp_light(1, 1, 0) == 0, "nothing from the reach on");
}

/* Stage 2: the ground mist thins with height over the ground and is thicker below it (a dip). */
static void test_mist(void) {
    CHECK(fabsf(volfog_mist(0) - 1) < 1e-5f, "1 at the ground (%.3f)", volfog_mist(0));
    CHECK(volfog_mist(VOLFOG_MIST_HEIGHT) < 0.4f && volfog_mist(VOLFOG_MIST_HEIGHT) > 0.3f, "1/e at its height (%.3f)", volfog_mist(VOLFOG_MIST_HEIGHT));
    CHECK(volfog_mist(4 * VOLFOG_MIST_HEIGHT) < 0.02f, "gone a few heights up (%.4f)", volfog_mist(4 * VOLFOG_MIST_HEIGHT));
    CHECK(volfog_mist(-0.5f) > 1.2f && volfog_mist(-10) <= volfog_mist(-VOLFOG_MIST_LOW) + 1e-5f && volfog_mist(-10) < 2.5f,
          "thicker in a dip below the ground plane, capped (%.2f, %.2f)", volfog_mist(-0.5f), volfog_mist(-10));
    float prev = 1e9f;
    int mono = 1;
    for (int i = 0; i <= 50; i++) {
        const float m = volfog_mist(-2 + i * 0.2f);
        if (m > prev + 1e-6f) mono = 0;
        prev = m;
    }
    CHECK(mono, "never thicker higher up");
    CHECK(fabsf(volfog_mist_density(1, 0) - VOLFOG_MIST) < 1e-5f && volfog_mist_density(1, 1) > volfog_mist_density(1, 0) &&
              fabsf(volfog_mist_density(2, 0.5f) - 2 * volfog_mist_density(1, 0.5f)) < 1e-5f && volfog_mist_density(0, 1) == 0,
          "the mist's share: x strength, thicker when wet (%.2f dry, %.2f wet)", volfog_mist_density(1, 0), volfog_mist_density(1, 1));
}

/* Stage 2: the ground under the camera, eased; the camera's height when the level has no surface there. */
static void test_ground(void) {
    CHECK(volfog_ground_step(0, 0, 1.5f, 1, 10) == 1.5f, "the first frame takes the surface");
    CHECK(volfog_ground_step(0, 0, 0, 0, 10) == 10 - VOLFOG_CAM_HEIGHT, "no surface: the camera's height below it");
    const float g = volfog_ground_step(0, 1, 1, 1, 10);
    CHECK(g > 0 && g < 0.5f, "eased towards a step (%.3f)", g);
    CHECK(volfog_ground_step(0, 1, VOLFOG_GROUND_SNAP + 1, 1, 30) == VOLFOG_GROUND_SNAP + 1, "a jump (cut, another floor) is taken at once");
    float x = 0;
    for (int i = 0; i < 120; i++) x = volfog_ground_step(x, 1, 1, 1, 10);
    CHECK(fabsf(x - 1) < 0.01f, "settles in two seconds (%.3f)", x);
}

/* Stage 2: halo lamps (sprites the level lists no light for) join the level's lamps; a halo over a
 * level light (the level lights the street from below the lamp's head) is the same lamp: the level
 * light moves up to the head (its cone hangs from it) and the halo is dropped. */
static void test_dedupe(void) {
    const float up[3] = {0, 1, 0};
    VolfogLight level[2] = {{{0, 3, -10}, 9, {1, 1, 1}, 1, VOLFOG_LAMP}, {{8, 4, -20}, 9, {1, 1, 1}, 1, VOLFOG_LAMP}};
    const VolfogLight halo[3] = {{{0.5f, 7.5f, -11}, 5, {1, .8f, .5f}, 1, VOLFOG_LAMP},   /* the first level light's head, 4.5 above it */
                                 {{-6, 3, -8}, 5, {1, .8f, .5f}, 1, VOLFOG_LAMP},          /* a lamp the level has no light for */
                                 {{8 + VOLFOG_DEDUPE + 0.5f, 4, -20}, 5, {1, .8f, .5f}, 1, VOLFOG_LAMP}};
    VolfogLight out[4];
    const uint32_t k = volfog_dedupe(halo, 3, level, 2, up, out, 4);
    CHECK(k == 2 && out[0].pos[0] == -6 && out[1].pos[0] == 8 + VOLFOG_DEDUPE + 0.5f, "two halo lamps without a level light kept (%u)", k);
    CHECK(level[0].pos[0] == 0.5f && level[0].pos[1] == 7.5f && level[0].pos[2] == -11 && level[0].radius == 9 && level[0].rgb[1] == 1,
          "the level light under a halo hangs from the lamp's head, its own light kept (%.1f %.1f %.1f)", level[0].pos[0], level[0].pos[1], level[0].pos[2]);
    CHECK(level[1].pos[0] == 8 && level[1].pos[1] == 4, "a level light without a halo stays");
    VolfogLight high[1] = {{{0, -6, -10}, 9, {1, 1, 1}, 1, VOLFOG_LAMP}};   /* far below the halo (another floor) */
    CHECK(volfog_dedupe(halo, 1, high, 1, up, out, 4) == 1 && high[0].pos[1] == -6, "a light far below the halo is another lamp");
    CHECK(volfog_dedupe(halo, 3, NULL, 0, up, out, 4) == 3, "no level light: every halo lamp kept");
    CHECK(volfog_dedupe(halo + 1, 2, NULL, 0, up, out, 1) == 1 && out[0].pos[0] == -6, "at most cap");
    /* Fed to the selection with the level's lamps: the halo-only lamp lights the air. */
    VolfogLight in[4] = {level[0], level[1]}, sel[4];
    (void)volfog_dedupe(halo, 3, in, 2, up, &in[2], 2);
    const uint32_t n = volfog_select(in, 4, 48, 80, sel);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) found |= sel[i].pos[0] == -6;
    CHECK(n == 4 && found, "the halo-only lamp is among the lights in the air (%u)", n);
}

/* Stage 2: a lamp's cone reaches the ground under it. */
static void test_ground_reach(void) {
    CHECK(volfog_lamp_reach(5, 7.7f) >= 7.7f * 1.2f && volfog_lamp_reach(5, 7.7f) <= VOLFOG_RADIUS_MAX, "a high head reaches the ground (%.1f)", volfog_lamp_reach(5, 7.7f));
    CHECK(volfog_lamp_reach(9, 2) == 9, "a low lamp keeps its reach");
    CHECK(volfog_lamp_reach(5, 40) == VOLFOG_RADIUS_MAX && volfog_lamp_reach(5, -3) == 5, "capped; under the ground nothing changes");
}

/* Stage 2: rain adds scattering near the lights (not the extinction) and a forward glow. */
static void test_rain(void) {
    CHECK(volfog_rain_scatter(0) == 1 && volfog_rain_scatter(1) >= 1.8f, "the rain's in-scatter x%.2f", volfog_rain_scatter(1));
    CHECK(volfog_phase_g_rain(VOLFOG_LAMP, 0) == volfog_phase_g(VOLFOG_LAMP) && volfog_phase_g_rain(VOLFOG_LAMP, 1) > volfog_phase_g(VOLFOG_LAMP) &&
              volfog_phase_g_rain(VOLFOG_LAMP, 1) < 0.9f && volfog_phase_g_rain(VOLFOG_FIRE, 1) == volfog_phase_g(VOLFOG_FIRE),
          "lamps glow forward in the rain (g %.2f -> %.2f), fires unchanged", volfog_phase_g(VOLFOG_LAMP), volfog_phase_g_rain(VOLFOG_LAMP, 1));
}

int main(void) {
    test_lamp_shape();
    test_mist();
    test_ground();
    test_dedupe();
    test_ground_reach();
    test_rain();
    test_options();
    test_grid();
    test_slices();
    test_far_and_fog();
    test_density();
    test_select();
    test_falloff();
    test_reproject();
    test_jitter();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
