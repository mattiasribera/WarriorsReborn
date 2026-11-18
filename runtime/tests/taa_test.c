/* Game-independent --taa test (src/taa.c): sharpness parsing, the Halton jitter, the reprojection
 * map from two world-to-clip matrices (static, turning and moving cameras, the sky at infinity) and
 * the camera-cut test. */
#include "../src/taa.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_options(void) {
    CHECK(taa_options.on == 0 && taa_options.sharpness == TAA_SHARPNESS_DEFAULT, "defaults: off, sharpness %.2f (%d, %.2f)",
          TAA_SHARPNESS_DEFAULT, taa_options.on, taa_options.sharpness);
    static const struct { const char *s; float v; } ok[] = {{"0", 0}, {"0.3", 0.3f}, {"1", 1}, {"1.0", 1}, {".5", 0.5f}};
    for (unsigned k = 0; k < sizeof ok / sizeof ok[0]; k++) {
        float v = -1;
        CHECK(taa_parse_sharpness(ok[k].s, &v) == 0 && fabsf(v - ok[k].v) < 1e-6f, "--taa-sharpness %s parses (%.3f)", ok[k].s, v);
    }
    static const char *bad[] = {"", "-0.1", "1.01", "2", "abc", "0.5x", "nan", "inf"};
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        float v = 0.7f;
        CHECK(taa_parse_sharpness(bad[k], &v) == -1 && v == 0.7f, "--taa-sharpness \"%s\" is rejected and leaves the value", bad[k]);
    }
}

static void test_jitter(void) {
    CHECK(fabsf(taa_halton(1, 2) - 0.5f) < 1e-6f && fabsf(taa_halton(2, 2) - 0.25f) < 1e-6f && fabsf(taa_halton(3, 2) - 0.75f) < 1e-6f,
          "Halton base 2: 1/2, 1/4, 3/4");
    CHECK(fabsf(taa_halton(1, 3) - 1 / 3.0f) < 1e-6f && fabsf(taa_halton(2, 3) - 2 / 3.0f) < 1e-6f && fabsf(taa_halton(3, 3) - 1 / 9.0f) < 1e-6f,
          "Halton base 3: 1/3, 2/3, 1/9");
    float sx = 0, sy = 0, j0[2], j8[2];
    int inside = 1, distinct = 1;
    float seen[TAA_JITTER_PHASES][2];
    for (uint32_t n = 0; n < TAA_JITTER_PHASES; n++) {
        float j[2];
        taa_jitter(n, j);
        inside &= j[0] > -0.5f && j[0] < 0.5f && j[1] > -0.5f && j[1] < 0.5f;
        for (uint32_t k = 0; k < n; k++) distinct &= fabsf(seen[k][0] - j[0]) > 1e-3f || fabsf(seen[k][1] - j[1]) > 1e-3f;
        seen[n][0] = j[0];
        seen[n][1] = j[1];
        sx += j[0];
        sy += j[1];
    }
    CHECK(inside && distinct, "8 distinct jitter positions inside the pixel");
    CHECK(fabsf(sx / TAA_JITTER_PHASES) < 0.07f && fabsf(sy / TAA_JITTER_PHASES) < 0.07f, "jitter mean near 0 (%.3f %.3f)",
          sx / TAA_JITTER_PHASES, sy / TAA_JITTER_PHASES);
    taa_jitter(0, j0);
    taa_jitter(TAA_JITTER_PHASES, j8);
    CHECK(j0[0] == j8[0] && j0[1] == j8[1], "the sequence repeats after %d frames", TAA_JITTER_PHASES);
}

/* A D3D-style camera as a world-to-clip matrix (column vector, rows x y z w): eye at (ex, ey, ez)
 * looking down +y turned by yaw radians about z (world up), horizontal scale px, vertical py;
 * view distance = w. */
static void camera(float m[4][4], float ex, float ey, float ez, float yaw, float px, float py) {
    const float c = cosf(yaw), s = sinf(yaw);
    const float r[3] = {c, -s, 0}, f[3] = {s, c, 0}, u[3] = {0, 0, 1}, e[3] = {ex, ey, ez};
    memset(m, 0, sizeof(float) * 16);
    for (int j = 0; j < 3; j++) {
        m[0][j] = px * r[j];
        m[1][j] = py * u[j];
        m[2][j] = 0.9f * f[j];
        m[3][j] = f[j];
    }
    for (int i = 0; i < 4; i++) m[i][3] = -(m[i][0] * e[0] + m[i][1] * e[1] + m[i][2] * e[2]);
    m[2][3] -= 0.1f;
}
/* NDC of world point p through m. */
static void project(const float m[4][4], const float p[3], float ndc[2], float *w) {
    float c[4];
    for (int i = 0; i < 4; i++) c[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3];
    ndc[0] = c[0] / c[3];
    ndc[1] = c[1] / c[3];
    *w = c[3];
}

static void test_reprojection(void) {
    float a[4][4], b[4][4], m[3][4];
    const float A = 1.0f, B = 0.1f;   /* NDC depth = 1 - 0.1 / w */
    camera(a, 0, 0, 1.7f, 0, 1.2f, 1.6f);
    CHECK(taa_reprojection(a, a, m) == 0, "a camera reprojects onto itself");
    float worst = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) worst = fmaxf(worst, fabsf(m[i][j] - (i == j ? 1.0f : 0.0f)));
    CHECK(worst < 1e-5f, "static camera: the map is the identity on (x w, y w, w) (%.2g)", worst);
    CHECK(taa_camera_motion(m, TAA_CUT_DIST) < 1e-5f && !taa_camera_cut(m), "static camera: no motion, no cut");

    /* A moving and turning camera: a world point seen in both frames lands where the previous
     * camera saw it, from its current NDC and depth alone. */
    camera(b, 0.3f, 0.5f, 1.7f, 0.05f, 1.2f, 1.6f);
    CHECK(taa_reprojection(b, a, m) == 0, "a moving camera builds the map");
    const float pts[4][3] = {{0.5f, 6, 1.2f}, {-2, 10, 0.3f}, {1, 3, 2.5f}, {4, 25, 0}};
    float err = 0;
    for (int k = 0; k < 4; k++) {
        float now[2], then[2], w, w0, got[2];
        project(b, pts[k], now, &w);
        project(a, pts[k], then, &w0);
        const float d = A - B / w;
        if (taa_reproject_point(m, A, B, now[0], now[1], d, got)) {
            err = 1e9f;
            continue;
        }
        err = fmaxf(err, fmaxf(fabsf(got[0] - then[0]), fabsf(got[1] - then[1])));
    }
    CHECK(err < 1e-4f, "moving camera: points reproject onto their previous NDC (max error %.2g)", err);

    /* The sky: depth 1 is at infinity, only the turn moves it. */
    float turn[4][4];
    camera(turn, 0, 0, 1.7f, 0.05f, 1.2f, 1.6f);
    taa_reprojection(turn, a, m);
    float dir_now[2], dir_then[2], w, w0, got[2];
    const float far[3] = {0.1f * 1e6f, 1e6f, 0.05f * 1e6f};
    project(turn, far, dir_now, &w);
    project(a, far, dir_then, &w0);
    CHECK(taa_reproject_point(m, A, B, dir_now[0], dir_now[1], 1.0f, got) == 0 && fabsf(got[0] - dir_then[0]) < 1e-4f &&
              fabsf(got[1] - dir_then[1]) < 1e-4f,
          "sky (depth 1): reprojected as a direction (%.4f %.4f, want %.4f %.4f)", got[0], got[1], dir_then[0], dir_then[1]);
    CHECK(taa_camera_motion(m, TAA_CUT_DIST) > 0.01f && !taa_camera_cut(m), "a small turn moves the screen, not a cut (%.3f)",
          taa_camera_motion(m, TAA_CUT_DIST));

    /* Cuts: a half turn, or a jump across the street. */
    float cut[4][4];
    camera(cut, 0, 0, 1.7f, 3.0f, 1.2f, 1.6f);
    taa_reprojection(cut, a, m);
    CHECK(taa_camera_cut(m), "a half turn is a cut (%.3g)", taa_camera_motion(m, TAA_CUT_DIST));
    camera(cut, 12, -3, 1.7f, 0, 1.2f, 1.6f);
    taa_reprojection(cut, a, m);
    CHECK(taa_camera_cut(m), "a 12-unit jump is a cut (%.3g)", taa_camera_motion(m, TAA_CUT_DIST));

    /* Degenerate: x, y, w rows not independent. */
    float bad[4][4];
    memcpy(bad, a, sizeof bad);
    memcpy(bad[1], bad[0], sizeof bad[0]);
    float keep[3][4];
    memset(keep, 0x55, sizeof keep);
    memcpy(m, keep, sizeof m);
    CHECK(taa_reprojection(bad, a, m) == -1 && !memcmp(m, keep, sizeof m), "a singular camera is refused and leaves the map");
}

/* --taa-upscale: the modes, the render size (FSR 2's ratios, rounded like
 * fsr_render_size), the jitter length scaled by the ratio and the texture LOD bias. */
static void test_upscale(void) {
    CHECK(taa_options.upscale == TAA_UP_OFF, "--taa-upscale defaults to off");
    static const struct { const char *s; TaaUpscale m; float k; } modes[] = {
        {"off", TAA_UP_OFF, 1.0f},           {"ultra", TAA_UP_ULTRA, 1.3f},           {"quality", TAA_UP_QUALITY, 1.5f},
        {"balanced", TAA_UP_BALANCED, 1.7f}, {"performance", TAA_UP_PERFORMANCE, 2.0f}};
    for (unsigned k = 0; k < sizeof modes / sizeof modes[0]; k++) {
        TaaUpscale m = (TaaUpscale)-1;
        float f = -1.0f;
        CHECK(taa_parse_upscale(modes[k].s, &m, &f) == 0 && m == modes[k].m && f == 0.0f && !strcmp(taa_upscale_name(m), modes[k].s) &&
                  taa_upscale_scale(m) == modes[k].k,
              "--taa-upscale %s: scale %.1f per axis", modes[k].s, modes[k].k);
    }
    /* A ratio per axis instead of a mode name: TAA_UP_SCALE_MIN..TAA_UP_SCALE_MAX, mode "custom". */
    static const struct { const char *s; float k; } nums[] = {{"1.25", 1.25f}, {"1.1", 1.1f}, {"3", 3.0f}, {"2.0", 2.0f}};
    for (unsigned k = 0; k < sizeof nums / sizeof nums[0]; k++) {
        TaaUpscale m = TAA_UP_OFF;
        float f = 0.0f;
        CHECK(taa_parse_upscale(nums[k].s, &m, &f) == 0 && m == TAA_UP_CUSTOM && fabsf(f - nums[k].k) < 1e-6f && !strcmp(taa_upscale_name(m), "custom"),
              "--taa-upscale %s: a custom ratio %.2f", nums[k].s, f);
    }
    static const char *bad[] = {"", "ultra ", "Quality", "quality ", "custom", "native", "1.0", "1.09", "3.01", "1.3x", "-2", "nan", "inf"};
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        TaaUpscale m = TAA_UP_BALANCED;
        float f = 0.5f;
        CHECK(taa_parse_upscale(bad[k], &m, &f) == -1 && m == TAA_UP_BALANCED && f == 0.5f, "--taa-upscale \"%s\" is rejected and leaves the value", bad[k]);
    }
    static const struct { TaaUpscale m; int ow, oh, rw, rh; } sizes[] = {
        {TAA_UP_OFF, 1920, 1080, 1920, 1080},      {TAA_UP_QUALITY, 1920, 1080, 1280, 720}, {TAA_UP_BALANCED, 1920, 1080, 1129, 635},
        {TAA_UP_PERFORMANCE, 1920, 1080, 960, 540}, {TAA_UP_QUALITY, 2560, 1440, 1707, 960}, {TAA_UP_PERFORMANCE, 1, 1, 1, 1},
        {TAA_UP_ULTRA, 1920, 1080, 1477, 831},      {TAA_UP_ULTRA, 2560, 1440, 1969, 1108}};
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int rw = 0, rh = 0;
        taa_upscale_size(sizes[k].m, sizes[k].ow, sizes[k].oh, &rw, &rh);
        CHECK(rw == sizes[k].rw && rh == sizes[k].rh, "%s at %dx%d renders %dx%d (%dx%d)", taa_upscale_name(sizes[k].m), sizes[k].ow, sizes[k].oh,
              sizes[k].rw, sizes[k].rh, rw, rh);
    }
    int rw = 0, rh = 0;
    taa_upscale_size_k(1.25f, 1920, 1080, &rw, &rh);
    CHECK(rw == 1536 && rh == 864, "a custom ratio 1.25 at 1920x1080 renders 1536x864 (%dx%d)", rw, rh);
    taa_upscale_size_k(1.5f, 1920, 1080, &rw, &rh);
    CHECK(rw == 1280 && rh == 720, "taa_upscale_size_k 1.5 = quality (%dx%d)", rw, rh);
    taa_upscale_size_k(0.0f, 1920, 1080, &rw, &rh);
    CHECK(rw == 1920 && rh == 1080, "taa_upscale_size_k 0 = the output size (%dx%d)", rw, rh);
    /* FSR 2's rule: 8 x (display / render)^2 phases (width ratio, rounded up), at least the native 8. */
    CHECK(taa_jitter_phases(1477, 1920) == 14, "jitter phases: ultra 14 (%u)", taa_jitter_phases(1477, 1920));
    CHECK(taa_jitter_phases(1920, 1920) == 8 && taa_jitter_phases(1280, 1920) == 18 && taa_jitter_phases(1129, 1920) == 24 &&
              taa_jitter_phases(960, 1920) == 32 && taa_jitter_phases(2000, 1920) == 8 && taa_jitter_phases(0, 1920) == 8,
          "jitter phases: native 8, quality 18, balanced 24, performance 32 (%u %u %u %u)", taa_jitter_phases(1920, 1920),
          taa_jitter_phases(1280, 1920), taa_jitter_phases(1129, 1920), taa_jitter_phases(960, 1920));
    float a[2], b[2];
    int same = 1, inside = 1;
    for (uint32_t n = 0; n < TAA_JITTER_PHASES; n++) {
        taa_jitter(n, a);
        taa_jitter_n(n, TAA_JITTER_PHASES, b);
        same &= a[0] == b[0] && a[1] == b[1];
    }
    float sx = 0, sy = 0;
    for (uint32_t n = 0; n < 32; n++) {
        taa_jitter_n(n, 32, b);
        inside &= b[0] > -0.5f && b[0] < 0.5f && b[1] > -0.5f && b[1] < 0.5f;
        sx += b[0];
        sy += b[1];
    }
    taa_jitter_n(0, 32, a);
    taa_jitter_n(32, 32, b);
    CHECK(same && inside && a[0] == b[0] && a[1] == b[1] && fabsf(sx / 32) < 0.03f && fabsf(sy / 32) < 0.03f,
          "taa_jitter_n: 8 phases = taa_jitter; 32 phases inside the pixel, repeating, mean near 0 (%.3f %.3f)", sx / 32, sy / 32);
    CHECK(taa_lod_bias(1920, 1920) == 0.0f && fabsf(taa_lod_bias(960, 1920) + 1.0f) < 1e-6f && fabsf(taa_lod_bias(1280, 1920) + 0.585f) < 1e-3f &&
              taa_lod_bias(2000, 1920) == 0.0f && taa_lod_bias(0, 1920) == 0.0f,
          "LOD bias log2(render / display): 0 native, -0.585 quality, -1 performance");
    CHECK(fabsf(taa_lod_bias(1477, 1920) + 0.378f) < 1e-3f, "LOD bias ultra -0.378 (%.3f)", taa_lod_bias(1477, 1920));
}

/* --taa-sharpness left out: the default follows the render ratio. */
static void test_auto_sharpness(void) {
    CHECK(!taa_options.sharpness_set, "--taa-sharpness not given by default");
    CHECK(taa_auto_sharpness(1.0f) == TAA_SHARPNESS_DEFAULT && taa_auto_sharpness(0.5f) == TAA_SHARPNESS_DEFAULT &&
              taa_auto_sharpness(1.3f) == TAA_SHARPNESS_DEFAULT,
          "plain TAA and ultra (1.3x) keep %.2f", TAA_SHARPNESS_DEFAULT);
    CHECK(fabsf(taa_auto_sharpness(2.0f) - TAA_SHARPNESS_LOW_RES) < 1e-6f && fabsf(taa_auto_sharpness(3.0f) - TAA_SHARPNESS_LOW_RES) < 1e-6f,
          "2x per axis and beyond: %.2f (%.3f, %.3f)", TAA_SHARPNESS_LOW_RES, taa_auto_sharpness(2.0f), taa_auto_sharpness(3.0f));
    float prev = 0;
    int mono = 1;
    for (float k = 1.0f; k <= 3.0f; k += 0.05f) {
        const float v = taa_auto_sharpness(k);
        mono &= v >= prev - 1e-6f && v <= TAA_SHARPNESS_MAX;
        prev = v;
    }
    CHECK(mono, "rises with the ratio and stays within 0..%.1f", TAA_SHARPNESS_MAX);
    const float q = taa_auto_sharpness(1.5f);
    CHECK(q > TAA_SHARPNESS_DEFAULT && q < TAA_SHARPNESS_LOW_RES, "quality (1.5x) in between (%.3f)", q);
    CHECK(taa_auto_sharpness(NAN) == TAA_SHARPNESS_DEFAULT, "not a number: the default");
}

int main(void) {
    test_auto_sharpness();
    test_options();
    test_upscale();
    test_jitter();
    test_reprojection();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
