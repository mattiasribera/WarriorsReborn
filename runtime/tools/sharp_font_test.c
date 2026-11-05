/* Game-independent test of the sharp font upscale (src/sharp_font.c) on synthetic atlases: white
 * anti-aliased letter shapes come out closer to the same shape rendered at 4x than a bilinear
 * upscale and with a narrow edge, their ink keeps its area and stays white, a faint accent stroke
 * survives, while coloured icons, grey art and a soft white glow come out exactly as the bilinear
 * upscale (what the GPU samples today). Also the atlas hashes, the option and argument checks.
 * Usage: sharp_font_test */
#include "../src/sharp_font.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define W 64
#define S SHARP_FONT_SCALE

typedef int (*Shape)(float x, float y);   /* inside test in input texel units */

/* A ring with a vertical bar through it and a slanted stroke: curves, straight edges, corners. */
static int letter(float x, float y) {
    float dx = x - 24, dy = y - 30, r = sqrtf(dx * dx + dy * dy);
    if (r > 9 && r < 15) return 1;
    if (x > 38 && x < 43 && y > 12 && y < 48) return 1;
    float d = (x - 48) - 0.5f * (y - 14);      /* slanted stroke, 3.5 texels wide */
    return y > 14 && y < 46 && d > 0 && d < 3.5f;
}
/* An accent: a short slanted stroke 1.2 texels wide (a Spanish acute) above a small block. */
static int accent(float x, float y) {
    float d = (x - 20) + 0.8f * (y - 10);
    if (y > 6 && y < 12 && d > 0 && d < 1.2f) return 1;
    return x > 14 && x < 26 && y > 16 && y < 30;
}

/* Coverage of shape over [x0, x0 + size) x [y0, y0 + size) with 8 x 8 samples. */
static float coverage(Shape f, float x0, float y0, float size) {
    int in = 0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) in += f(x0 + (i + 0.5f) * size / 8, y0 + (j + 0.5f) * size / 8);
    return in / 64.0f;
}

static uint32_t argb(float a, uint32_t rgb) {
    int ai = (int)lroundf(a * 255);
    return (uint32_t)ai << 24 | rgb;
}

/* The shape at 1x (input) or at S x (reference), alpha = coverage, colour rgb. */
static uint32_t *render(Shape f, int scale, uint32_t rgb, float alpha_scale) {
    int n = W * scale;
    uint32_t *p = malloc((size_t)n * n * 4);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) p[y * n + x] = argb(alpha_scale * coverage(f, (float)x / scale, (float)y / scale, 1.0f / scale), rgb);
    return p;
}

static float ch(uint32_t c, int s) { return (float)(c >> s & 255); }

/* What the GPU samples: straight bilinear at texel centres, clamped at the border. */
static uint32_t *bilinear(const uint32_t *in, int w, int h, int s) {
    uint32_t *o = malloc((size_t)w * s * h * s * 4);
    for (int y = 0; y < h * s; y++)
        for (int x = 0; x < w * s; x++) {
            float u = (x + 0.5f) / s - 0.5f, v = (y + 0.5f) / s - 0.5f;
            int x0 = (int)floorf(u), y0 = (int)floorf(v);
            float fx = u - x0, fy = v - y0;
            int xa = x0 < 0 ? 0 : x0, xb = x0 + 1 > w - 1 ? w - 1 : x0 + 1;
            int ya = y0 < 0 ? 0 : y0, yb = y0 + 1 > h - 1 ? h - 1 : y0 + 1;
            if (xa > w - 1) xa = w - 1;
            if (ya > h - 1) ya = h - 1;
            uint32_t c = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                float t = ch(in[ya * w + xa], sh) * (1 - fx) * (1 - fy) + ch(in[ya * w + xb], sh) * fx * (1 - fy) +
                          ch(in[yb * w + xa], sh) * (1 - fx) * fy + ch(in[yb * w + xb], sh) * fx * fy;
                c |= (uint32_t)lroundf(t) << sh;
            }
            o[y * w * s + x] = c;
        }
    return o;
}

/* Mean |alpha difference| (0..255) and the number of partly covered texels (alpha 26..229). */
static double alpha_err(const uint32_t *a, const uint32_t *b, int n) {
    double e = 0;
    for (int i = 0; i < n; i++) e += fabs(ch(a[i], 24) - ch(b[i], 24));
    return e / n;
}
static int soft_texels(const uint32_t *a, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) k += ch(a[i], 24) > 25 && ch(a[i], 24) < 230;
    return k;
}
static double alpha_sum(const uint32_t *a, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += ch(a[i], 24);
    return s;
}
/* Largest channel difference between two images. */
static int max_diff(const uint32_t *a, const uint32_t *b, int n) {
    int m = 0;
    for (int i = 0; i < n; i++)
        for (int s = 0; s < 32; s += 8) {
            int d = abs((int)(a[i] >> s & 255) - (int)(b[i] >> s & 255));
            if (d > m) m = d;
        }
    return m;
}

static void test_letter(void) {
    int n = W * S * W * S;
    uint32_t *in = render(letter, 1, 0xffffff, 1), *ref = render(letter, S, 0xffffff, 1);
    uint32_t *sharp = sharp_font_upscale(in, W, W, S), *bil = bilinear(in, W, W, S);
    CHECK(sharp != NULL, "letter: upscaled");
    if (!sharp) return;
    double es = alpha_err(sharp, ref, n), eb = alpha_err(bil, ref, n);
    CHECK(es < 0.7 * eb, "letter: closer to the 4x shape than bilinear (mean |da| %.2f vs %.2f)", es, eb);
    int ss = soft_texels(sharp, n), sb = soft_texels(bil, n), sr = soft_texels(ref, n);
    CHECK(ss < 0.5 * sb, "letter: narrow edge (%d partly covered texels, bilinear %d, 4x shape %d)", ss, sb, sr);
    double as = alpha_sum(sharp, n), ar = alpha_sum(ref, n);
    CHECK(fabs(as / ar - 1) < 0.04, "letter: ink area kept (%.3f of the 4x shape)", as / ar);
    int white = 1;
    for (int i = 0; i < n; i++)
        if (ch(sharp[i], 24) > 128 && (ch(sharp[i], 0) < 250 || ch(sharp[i], 8) < 250 || ch(sharp[i], 16) < 250)) white = 0;
    CHECK(white, "letter: ink stays white");
    uint32_t *again = sharp_font_upscale(in, W, W, S);
    CHECK(again && !memcmp(again, sharp, (size_t)n * 4), "letter: deterministic");
    CHECK(sharp_font_ink(in, W, W, 24, 15) > 0.9f, "letter: ink weight on the ring edge %.2f", sharp_font_ink(in, W, W, 24, 15));
    CHECK(sharp_font_ink(in, W, W, 2, 2) == 0.0f, "letter: no ink weight in empty space");
    free(again);
    free(in), free(ref), free(sharp), free(bil);
}

static void test_accent(void) {
    int n = W * S * W * S;
    uint32_t *in = render(accent, 1, 0xffffff, 1), *ref = render(accent, S, 0xffffff, 1);
    uint32_t *sharp = sharp_font_upscale(in, W, W, S);
    if (!sharp) {
        CHECK(0, "accent: upscaled");
        return;
    }
    /* the acute stroke alone: rows 6..12 of the input */
    double as = 0, ar = 0;
    int peak = 0;
    for (int y = 6 * S; y < 12 * S; y++)
        for (int x = 0; x < W * S; x++) {
            as += ch(sharp[y * W * S + x], 24), ar += ch(ref[y * W * S + x], 24);
            if ((int)ch(sharp[y * W * S + x], 24) > peak) peak = (int)ch(sharp[y * W * S + x], 24);
        }
    int in_peak = 0;
    for (int y = 6; y < 12; y++)
        for (int x = 0; x < W; x++)
            if ((int)ch(in[y * W + x], 24) > in_peak) in_peak = (int)ch(in[y * W + x], 24);
    CHECK(peak >= in_peak, "accent: the thin acute stroke does not fade (peak alpha %d, input %d)", peak, in_peak);
    CHECK(fabs(as / ar - 1) < 0.15, "accent: the acute keeps its ink (%.3f of the 4x shape)", as / ar);
    free(in), free(ref), free(sharp);
    (void)n;
}

/* A faint stroke (alpha 0.45 at most) keeps its level instead of vanishing under a fixed cut. */
static void test_faint(void) {
    int n = W * S * W * S;
    uint32_t *in = render(letter, 1, 0xffffff, 0.45f);
    uint32_t *sharp = sharp_font_upscale(in, W, W, S);
    if (!sharp) {
        CHECK(0, "faint: upscaled");
        return;
    }
    int peak = 0;
    for (int i = 0; i < n; i++)
        if ((int)ch(sharp[i], 24) > peak) peak = (int)ch(sharp[i], 24);
    double ratio = alpha_sum(sharp, n) / alpha_sum(render(letter, S, 0xffffff, 0.45f), n);
    CHECK(peak >= 110 && peak <= 120, "faint: peak alpha kept (%d, input %d)", peak, (int)lroundf(0.45f * 255));
    CHECK(fabs(ratio - 1) < 0.06, "faint: ink kept (%.3f)", ratio);
    free(in), free(sharp);
}

/* Art that is not letter ink comes out exactly as the bilinear upscale. */
static void test_unchanged(const char *what, uint32_t *in) {
    int n = W * S * W * S;
    uint32_t *sharp = sharp_font_upscale(in, W, W, S), *bil = bilinear(in, W, W, S);
    int d = sharp ? max_diff(sharp, bil, n) : 999;
    CHECK(d <= 1, "%s: as bilinear (largest channel difference %d)", what, d);
    free(in), free(sharp), free(bil);
}

static void test_art(void) {
    test_unchanged("coloured icon", render(letter, 1, 0x30c040, 1));   /* a green button */
    test_unchanged("grey art", render(letter, 1, 0x808080, 1));
    uint32_t *glow = malloc(W * W * 4);   /* a soft white glow (gaussian, sigma 3 texels) */
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            float r2 = (float)((x - 32) * (x - 32) + (y - 32) * (y - 32));
            glow[y * W + x] = argb(expf(-r2 / 18), 0xffffff);
        }
    test_unchanged("soft white glow", glow);
    uint32_t *photo = malloc(W * W * 4);   /* an opaque portrait-like block of varied colour */
    for (int i = 0; i < W * W; i++) photo[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8 & 0xffffff);
    test_unchanged("opaque picture", photo);
    test_unchanged("transparent", calloc(W * W, 4));
}

static void test_args(void) {
    uint32_t px[4] = {0};
    CHECK(!sharp_font_upscale(px, 2, 2, 0) && !sharp_font_upscale(px, 0, 2, 4) && !sharp_font_upscale(NULL, 2, 2, 4) &&
              !sharp_font_upscale(px, 2, 2, 9),
          "args: invalid size or scale rejected");
    uint32_t one = 0x80ffffffu, *o = sharp_font_upscale(&one, 1, 1, 4);
    int same = o != NULL;
    for (int i = 0; o && i < 16; i++) same &= o[i] == one;
    CHECK(same, "args: a 1x1 image fills its 4x4 copy");
    free(o);
    CHECK(sharp_font_atlas(SHARP_FONT_MAIN_HASH) && sharp_font_atlas(SHARP_FONT_BIG_HASH) && !sharp_font_atlas(0x1234),
          "atlas: both font atlases, nothing else");
    int on = 0;
    CHECK(sharp_font_arg(&on, "--sharp-fonts") == 1 && on == 1, "arg: --sharp-fonts");
    CHECK(sharp_font_arg(&on, "--no-sharp-fonts") == 1 && on == 0, "arg: --no-sharp-fonts");
    CHECK(sharp_font_arg(&on, "--sharp") == 0 && on == 0, "arg: other options ignored");
}

int main(void) {
    test_letter();
    test_accent();
    test_faint();
    test_art();
    test_args();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
