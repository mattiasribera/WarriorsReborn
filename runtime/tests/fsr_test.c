/* Game-independent --fsr test (src/fsr.c): mode and sharpness parsing, the presets' scales, the
 * internal render size rounding, the conflict rules with --supersample and --sharpen, and the
 * EASU / RCAS shader constants. */
#include "../src/fsr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static float f_of(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static void test_modes(void) {
    static const struct { const char *s; FsrMode m; float scale; } ok[] = {
        {"off", FSR_OFF, 1.0f}, {"ultra", FSR_ULTRA, 1.3f}, {"quality", FSR_QUALITY, 1.5f},
        {"balanced", FSR_BALANCED, 1.7f}, {"performance", FSR_PERFORMANCE, 2.0f}};
    for (unsigned k = 0; k < sizeof ok / sizeof ok[0]; k++) {
        FsrMode m = (FsrMode)-1;
        CHECK(fsr_parse_mode(ok[k].s, &m) == 0 && m == ok[k].m, "--fsr %s parses (%d)", ok[k].s, (int)m);
        CHECK(!strcmp(fsr_mode_name(ok[k].m), ok[k].s), "mode %d is named %s (%s)", (int)ok[k].m, ok[k].s, fsr_mode_name(ok[k].m));
        CHECK(fabsf(fsr_mode_scale(ok[k].m) - ok[k].scale) < 1e-6f, "%s scales by %.1f (%.3f)", ok[k].s, ok[k].scale,
              fsr_mode_scale(ok[k].m));
    }
    static const char *bad[] = {"", "Quality", "QUALITY", "ultra-quality", "ultra_quality", "2", "on", "quality "};
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        FsrMode m = FSR_BALANCED;
        CHECK(fsr_parse_mode(bad[k], &m) == -1 && m == FSR_BALANCED, "--fsr \"%s\" is rejected and leaves the mode", bad[k]);
    }
    CHECK(!strcmp(fsr_mode_name((FsrMode)42), "off"), "an unknown mode is named off");
    CHECK(fsr_mode_scale((FsrMode)42) == 1.0f, "an unknown mode scales by 1");
    CHECK(fsr_options.mode == FSR_OFF && fsr_options.sharpness == FSR_SHARPNESS_DEFAULT,
          "defaults: off, sharpness %.1f (%d, %.2f)", FSR_SHARPNESS_DEFAULT, (int)fsr_options.mode, fsr_options.sharpness);
}

static void test_sharpness(void) {
    static const struct { const char *s; float v; } ok[] = {{"0", 0.0f}, {"0.2", 0.2f}, {"1", 1.0f}, {"2", 2.0f}, {"2.0", 2.0f}};
    for (unsigned k = 0; k < sizeof ok / sizeof ok[0]; k++) {
        float v = -1;
        CHECK(fsr_parse_sharpness(ok[k].s, &v) == 0 && fabsf(v - ok[k].v) < 1e-6f, "--fsr-sharpness %s parses (%.3f)", ok[k].s, v);
    }
    static const char *bad[] = {"", "-0.1", "2.01", "abc", "0.2x", "nan", "inf", " "};
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        float v = 0.7f;
        CHECK(fsr_parse_sharpness(bad[k], &v) == -1 && v == 0.7f, "--fsr-sharpness \"%s\" is rejected and leaves the value", bad[k]);
    }
}

static void test_render_size(void) {
    static const struct { FsrMode m; int ow, oh, rw, rh; } t[] = {
        {FSR_OFF, 1920, 1080, 1920, 1080},
        {FSR_ULTRA, 1920, 1080, 1477, 831},        /* 1476.9 x 830.8 */
        {FSR_QUALITY, 1920, 1080, 1280, 720},
        {FSR_BALANCED, 1920, 1080, 1129, 635},     /* 1129.4 x 635.3 */
        {FSR_PERFORMANCE, 1920, 1080, 960, 540},
        {FSR_QUALITY, 1440, 1080, 960, 720},       /* --resolution 1920x1080 (4:3 inside) */
        {FSR_PERFORMANCE, 2560, 1440, 1280, 720},
        {FSR_PERFORMANCE, 3, 5, 2, 3},             /* halves round up */
        {FSR_PERFORMANCE, 1, 1, 1, 1},             /* at least 1 */
        {FSR_BALANCED, 1, 1, 1, 1},
    };
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
        int rw = -1, rh = -1;
        fsr_render_size(t[k].m, t[k].ow, t[k].oh, &rw, &rh);
        CHECK(rw == t[k].rw && rh == t[k].rh, "%s: %dx%d renders at %dx%d (%dx%d)", fsr_mode_name(t[k].m), t[k].ow, t[k].oh, t[k].rw,
              t[k].rh, rw, rh);
    }
}

static void test_resolve(void) {
    int ss = 2, sh = 50;
    CHECK(fsr_resolve(FSR_QUALITY, &ss, &sh) == (FSR_DROPPED_SUPERSAMPLE | FSR_DROPPED_SHARPEN) && ss == 0 && sh == 0,
          "fsr drops --supersample 2 and --sharpen 50 (ss %d, sharpen %d)", ss, sh);
    ss = 4, sh = 0;
    CHECK(fsr_resolve(FSR_PERFORMANCE, &ss, &sh) == FSR_DROPPED_SUPERSAMPLE && ss == 0 && sh == 0, "fsr drops --supersample 4 alone");
    ss = 1, sh = 30;
    CHECK(fsr_resolve(FSR_ULTRA, &ss, &sh) == FSR_DROPPED_SHARPEN && sh == 0 && ss == 1,
          "fsr drops --sharpen alone; --supersample 1 is not a conflict (ss %d)", ss);
    ss = 0, sh = 0;
    CHECK(fsr_resolve(FSR_BALANCED, &ss, &sh) == 0 && ss == 0 && sh == 0, "fsr with neither: nothing dropped");
    ss = 2, sh = 50;
    CHECK(fsr_resolve(FSR_OFF, &ss, &sh) == 0 && ss == 2 && sh == 50, "off keeps --supersample and --sharpen");
}

static void test_constants(void) {
    uint32_t c[16];
    memset(c, 0xab, sizeof c);
    fsr_easu_con(c, 960, 540, 1920, 1080);
    CHECK(f_of(c[0]) == 0.5f && f_of(c[1]) == 0.5f, "easu con0.xy: input / output (%g, %g)", f_of(c[0]), f_of(c[1]));
    CHECK(f_of(c[2]) == -0.25f && f_of(c[3]) == -0.25f, "easu con0.zw: 0.5 * in / out - 0.5 (%g, %g)", f_of(c[2]), f_of(c[3]));
    CHECK(f_of(c[4]) == 1.0f / 960 && f_of(c[5]) == 1.0f / 540, "easu con1.xy: 1 / input size");
    CHECK(f_of(c[6]) == 1.0f / 960 && f_of(c[7]) == -1.0f / 540, "easu con1.zw: (1, -1) texel");
    CHECK(f_of(c[8]) == -1.0f / 960 && f_of(c[9]) == 2.0f / 540 && f_of(c[10]) == 1.0f / 960 && f_of(c[11]) == 2.0f / 540,
          "easu con2: (-1, 2) and (1, 2) texels");
    CHECK(f_of(c[12]) == 0.0f && f_of(c[13]) == 4.0f / 540 && c[14] == 0 && c[15] == 0, "easu con3: (0, 4) texels, then zero");
    fsr_easu_con(c, 1280, 720, 1920, 1080);
    CHECK(fabsf(f_of(c[0]) - 2.0f / 3.0f) < 1e-6f && fabsf(f_of(c[2]) + 1.0f / 6.0f) < 1e-6f, "easu quality: 2/3 step, -1/6 offset");

    uint32_t r[4];
    memset(r, 0xab, sizeof r);
    fsr_rcas_con(r, 0.0f);
    CHECK(f_of(r[0]) == 1.0f && r[1] == 0x3C003C00u && r[2] == 0 && r[3] == 0, "rcas 0 stops: 1.0, half pair 0x%08x", r[1]);
    fsr_rcas_con(r, 1.0f);
    CHECK(f_of(r[0]) == 0.5f && r[1] == 0x38003800u, "rcas 1 stop: 0.5 (%g), half pair 0x%08x", f_of(r[0]), r[1]);
    fsr_rcas_con(r, FSR_SHARPNESS_DEFAULT);
    CHECK(fabsf(f_of(r[0]) - 0.870551f) < 1e-5f, "rcas default 0.2 stops: 2^-0.2 (%g)", f_of(r[0]));
    fsr_rcas_con(r, 5.0f);
    CHECK(f_of(r[0]) == 0.25f, "rcas sharpness clamps to %.0f stops (%g)", FSR_SHARPNESS_MAX, f_of(r[0]));
    fsr_rcas_con(r, -1.0f);
    CHECK(f_of(r[0]) == 1.0f, "rcas negative sharpness clamps to 0 (%g)", f_of(r[0]));
}

int main(void) {
    test_modes();
    test_sharpness();
    test_render_size();
    test_resolve();
    test_constants();
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
