/* Game-independent --filmic test (src/filmic.c): option parsing, the tonemap (monotonic, black and
 * white points, mid-grey kept, identity at strength 0), the procedural grade and its LUT
 * (monotonic along the grey axis and each primary, mid-grey, black and white points), and the
 * .cube parser (identity round trip, rejected files). */
#include "../src/filmic.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static float luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

static void test_args(void) {
    FilmicConfig c = FILMIC_DEFAULTS;
    CHECK(!c.on && c.strength == FILMIC_STRENGTH_DEFAULT && c.grain == 0 && !c.lut_path, "defaults: off, strength 1, no grain");
    CHECK(filmic_arg(&c, "--filmic", NULL) == 1 && c.on, "--filmic turns it on");
    CHECK(filmic_arg(&c, "--no-filmic", NULL) == 1 && !c.on, "--no-filmic turns it off");
    CHECK(filmic_arg(&c, "--filmic-strength", "0.5") == 2 && c.strength == 0.5f, "--filmic-strength 0.5");
    CHECK(filmic_arg(&c, "--filmic-strength", "2.5") == -1 && c.strength == 0.5f, "--filmic-strength 2.5 rejected, unchanged");
    CHECK(filmic_arg(&c, "--filmic-strength", NULL) == -1, "--filmic-strength without a value rejected");
    CHECK(filmic_arg(&c, "--filmic-grain", "0.3") == 2 && c.grain == 0.3f, "--filmic-grain 0.3");
    CHECK(filmic_arg(&c, "--filmic-grain", "x") == -1 && c.grain == 0.3f, "--filmic-grain x rejected");
    CHECK(filmic_arg(&c, "--filmic-vignette", "0") == 2 && c.vignette == 0, "--filmic-vignette 0");
    CHECK(filmic_arg(&c, "--filmic-vignette", "1.1") == -1, "--filmic-vignette 1.1 rejected");
    CHECK(filmic_arg(&c, "--filmic-lut", "my.cube") == 2 && c.lut_path && !strcmp(c.lut_path, "my.cube"), "--filmic-lut FILE");
    CHECK(filmic_arg(&c, "--filmic-lut", "") == -1, "--filmic-lut with an empty name rejected");
    CHECK(filmic_arg(&c, "--bloom", NULL) == 0, "other options are not consumed");
}

static void test_tonemap(void) {
    RFilmic p;
    filmic_params(1.0f, 0, 0.25f, &p);
    CHECK(p.white > 1.5f && p.hdr > 0.5f && p.blend > 0 && p.blend <= 1 && p.exposure > 0, "params: headroom %.2f, blend %.2f, exposure %.3f",
          p.white, p.blend, p.exposure);
    CHECK(fabsf(filmic_tonemap(&p, 0)) < 1e-6f, "tonemap: black stays black (%g)", filmic_tonemap(&p, 0));
    CHECK(fabsf(filmic_tonemap(&p, p.white) - 1) < 1e-4f, "tonemap: the white point maps to 1 (%g)", filmic_tonemap(&p, p.white));
    CHECK(fabsf(filmic_tonemap(&p, 0.18f) - 0.18f) < 0.005f, "tonemap: linear mid-grey kept (%g)", filmic_tonemap(&p, 0.18f));
    const float w1 = filmic_tonemap(&p, 1.0f);
    CHECK(w1 < 1 && w1 > 0.8f, "tonemap: plain white rolls off gently below 1 (%g)", w1);
    int mono = 1;
    float prev = -1;
    for (int i = 0; i <= 2000; i++) {
        float y = filmic_tonemap(&p, p.white * i / 2000.0f);
        if (!(y > prev)) mono = 0;
        prev = y;
    }
    CHECK(mono, "tonemap: strictly increasing over 0..white");
    const float g = filmic_display(&p, 0.4613f, 0);   /* display mid-grey (0.18 ^ (1 / 2.2)) */
    CHECK(fabsf(g - 0.4613f) < 0.01f, "display: mid-grey kept (%g)", g);
    CHECK(filmic_display(&p, 0.1f, 0) < 0.1f, "display: shadows a little deeper (%g)", filmic_display(&p, 0.1f, 0));
    CHECK(filmic_display(&p, 0.9f, 1) > filmic_display(&p, 0.9f, 0), "display: light-masked highlights brighter than unmasked");
    CHECK(filmic_display(&p, 1, 1) > 0.99f, "display: a masked white light stays white (%g)", filmic_display(&p, 1, 1));
    CHECK(filmic_display(&p, 0.3f, 1) == filmic_display(&p, 0.3f, 0), "display: the mask leaves dark pixels alone");
    mono = 1;
    prev = -1;
    for (int m = 0; m <= 1; m++, prev = -1)
        for (int i = 0; i <= 1000; i++) {
            float y = filmic_display(&p, i / 1000.0f, (float)m);
            if (y < prev) mono = 0;
            prev = y;
        }
    CHECK(mono, "display: non-decreasing over 0..1 with and without the mask");
    RFilmic z;
    filmic_params(0, 0, 0, &z);
    float worst = 0;
    for (int i = 0; i <= 100; i++) {
        float v = i / 100.0f, d = fabsf(filmic_display(&z, v, 1) - v);
        worst = d > worst ? d : worst;
    }
    CHECK(worst < 1e-3f && z.lut_mix == 0 && z.halation == 0, "strength 0: identity (worst %g)", worst);
    RFilmic s2;
    filmic_params(2, 0, 0, &s2);
    CHECK(s2.blend <= 1 && s2.lut_mix <= 1 && fabsf(filmic_tonemap(&s2, 0.18f) - 0.18f) < 0.005f, "strength 2: blends capped, mid-grey kept");
}

static void test_grade(void) {
    float in[3], out[3], prev[3] = {-1, -1, -1};
    int mono = 1;
    for (int i = 0; i <= 255; i++) {
        in[0] = in[1] = in[2] = i / 255.0f;
        filmic_grade(in, out);
        for (int c = 0; c < 3; c++) {
            if (out[c] < prev[c] - 1e-6f) mono = 0;
            prev[c] = out[c];
        }
    }
    CHECK(mono, "grade: each channel non-decreasing along the grey axis");
    for (int axis = 0; axis < 3; axis++) {
        float last = -1;
        int ok = 1;
        for (int i = 0; i <= 255; i++) {
            in[0] = in[1] = in[2] = 0;
            in[axis] = i / 255.0f;
            filmic_grade(in, out);
            if (luma(out) < last - 1e-6f) ok = 0;
            last = luma(out);
        }
        CHECK(ok, "grade: luma non-decreasing along primary %d", axis);
    }
    in[0] = in[1] = in[2] = 0;
    filmic_grade(in, out);
    CHECK(out[0] <= 0.03f && out[1] <= 0.03f && out[2] <= 0.03f, "grade: black stays near black (%.3f %.3f %.3f)", out[0], out[1], out[2]);
    in[0] = in[1] = in[2] = 1;
    filmic_grade(in, out);
    CHECK(out[0] >= 0.9f && out[1] >= 0.9f && out[2] >= 0.9f && luma(out) >= 0.95f, "grade: white stays near white (%.3f %.3f %.3f)", out[0],
          out[1], out[2]);
    CHECK(out[0] >= out[2], "grade: warm highlights (red >= blue at white)");
    in[0] = in[1] = in[2] = 0.4613f;
    filmic_grade(in, out);
    CHECK(fabsf(luma(out) - 0.4613f) < 0.03f, "grade: mid-grey luma kept (%.3f)", luma(out));
    in[0] = in[1] = in[2] = 0.15f;
    filmic_grade(in, out);
    CHECK(out[2] > out[0], "grade: teal shadows (blue > red at 0.15: %.3f %.3f)", out[2], out[0]);
    CHECK(out[0] < 0.15f, "grade: shadows crushed a little (%.3f)", out[0]);

    const int n = FILMIC_LUT_SIZE;
    uint8_t *lut = malloc((size_t)n * n * n * 4);
    filmic_lut_build(n, lut);
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        const uint8_t *t = lut + 4 * ((size_t)i + (size_t)i * n + (size_t)i * n * n);   /* the grey diagonal */
        in[0] = in[1] = in[2] = i / (float)(n - 1);
        filmic_grade(in, out);
        for (int c = 0; c < 3; c++)   /* RGBA8: r g b a */
            if (abs((int)t[c] - (int)lrintf(out[c] * 255)) > 1) ok = 0;
        if (t[3] != 255) ok = 0;
    }
    CHECK(ok, "LUT: the grey diagonal matches filmic_grade, alpha 255");
    const uint8_t *red = lut + 4 * (size_t)(n - 1);   /* r = 1, g = b = 0: red is the fastest index */
    CHECK(red[0] > 200 && red[1] < 80 && red[2] < 80, "LUT: red fastest (%u %u %u)", red[0], red[1], red[2]);
    free(lut);
}

static void test_cube(void) {
    char text[4096], err[128];
    int n = 0;
    size_t len = (size_t)snprintf(text, sizeof text, "# identity\nTITLE \"id\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n\n");
    for (int b = 0; b < 2; b++)
        for (int g = 0; g < 2; g++)
            for (int r = 0; r < 2; r++) len += (size_t)snprintf(text + len, sizeof text - len, "%d.0 %d %d\n", r, g, b);
    uint8_t *lut = filmic_cube_parse(text, &n, err, sizeof err);
    CHECK(lut && n == 2, "cube: identity of size 2 parsed (%s)", lut ? "ok" : err);
    if (lut) {
        CHECK(lut[0] == 0 && lut[4] == 255 && lut[5] == 0 && lut[4 * 2 + 1] == 255 && lut[4 * 4 + 2] == 255 && lut[4 * 7] == 255 &&
                  lut[4 * 7 + 3] == 255,
              "cube: entries in order, red fastest");
        free(lut);
    }
    CHECK(!filmic_cube_parse("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n", &n, err, sizeof err), "cube: too few entries rejected (%s)", err);
    CHECK(!filmic_cube_parse("0 0 0\n", &n, err, sizeof err), "cube: no LUT_3D_SIZE rejected (%s)", err);
    CHECK(!filmic_cube_parse("LUT_3D_SIZE 1\n0 0 0\n", &n, err, sizeof err), "cube: size 1 rejected (%s)", err);
    CHECK(!filmic_cube_parse("LUT_3D_SIZE 2\nDOMAIN_MAX 2 2 2\n", &n, err, sizeof err), "cube: other domains rejected (%s)", err);
    CHECK(!filmic_cube_parse("LUT_1D_SIZE 16\n", &n, err, sizeof err), "cube: 1D LUT rejected (%s)", err);
    CHECK(!filmic_cube_parse("LUT_3D_SIZE 2\n0 0 x\n", &n, err, sizeof err), "cube: bad number rejected (%s)", err);
    CHECK(!filmic_cube_load("/nonexistent/x.cube", &n, err, sizeof err), "cube: missing file rejected (%s)", err);
}

int main(void) {
    test_args();
    test_tonemap();
    test_grade();
    test_cube();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
