/* Sharp fonts: the font atlases upscaled with a rebuilt letter edge (see sharp_font.h). */
#include "sharp_font.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Ink test thresholds. Steepness is the largest alpha change across two texels (central
 * difference) around a texel: an anti-aliased letter edge ramps 0 -> 1 within one or two texels
 * (>= 0.66), a soft glow far slower (a gaussian of sigma 3 texels: <= 0.41). Whiteness is the
 * alpha-weighted mean of the smallest colour channel around it: the letters are pure white ink
 * tinted by the vertex colour; icons are coloured or grey. */
#define STEEP_LO 0.50f
#define STEEP_HI 0.65f
#define WHITE_LO 0.80f
#define WHITE_HI 0.92f
/* Half width of the rebuilt edge ramp, in output texels: about one output texel in total. */
#define EDGE_HALF 0.5f

int sharp_font_atlas(uint64_t hash) {
    return hash == SHARP_FONT_MAIN_HASH || hash == SHARP_FONT_BIG_HASH;
}

int sharp_font_arg(int *on, const char *opt) {
    if (!strcmp(opt, "--sharp-fonts")) *on = 1;
    else if (!strcmp(opt, "--no-sharp-fonts")) *on = 0;
    else return 0;
    return 1;
}

static float smooth(float lo, float hi, float x) {
    float t = (x - lo) / (hi - lo);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float alpha_at(const uint32_t *p, int w, int h, int x, int y) {
    return (float)(p[clampi(y, 0, h - 1) * w + clampi(x, 0, w - 1)] >> 24) / 255.0f;
}

float sharp_font_ink(const uint32_t *p, uint32_t uw, uint32_t uh, int x, int y) {
    int w = (int)uw, h = (int)uh;
    float steep = 0, wsum = 0, white = 0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            int xx = x + i, yy = y + j;
            float gx = fabsf(alpha_at(p, w, h, xx + 1, yy) - alpha_at(p, w, h, xx - 1, yy));
            float gy = fabsf(alpha_at(p, w, h, xx, yy + 1) - alpha_at(p, w, h, xx, yy - 1));
            if (gx > steep) steep = gx;
            if (gy > steep) steep = gy;
            uint32_t c = p[clampi(yy, 0, h - 1) * w + clampi(xx, 0, w - 1)];
            float a = (float)(c >> 24) / 255.0f;
            uint32_t m = c & 255;
            if ((c >> 8 & 255) < m) m = c >> 8 & 255;
            if ((c >> 16 & 255) < m) m = c >> 16 & 255;
            wsum += a;
            white += a * (float)m / 255.0f;
        }
    if (wsum <= 0) return 0;
    return smooth(STEEP_LO, STEEP_HI, steep) * smooth(WHITE_LO, WHITE_HI, white / wsum);
}

/* Catmull-Rom weights of fraction t for the taps at -1, 0, 1, 2. */
static void cr_weights(float t, float k[4]) {
    float t2 = t * t, t3 = t2 * t;
    k[0] = 0.5f * (-t3 + 2 * t2 - t);
    k[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
    k[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
    k[3] = 0.5f * (t3 - t2);
}

/* Per-texel ink weights of the whole image (sharp_font_ink for every texel, in one pass): the
 * central differences, the smallest channel and the alpha are computed once per texel. */
static void ink_map(const uint32_t *in, int w, int h, const float *al, int ap, float *ink) {
    float *steep = malloc((size_t)w * h * sizeof(float)), *wa = malloc((size_t)w * h * 2 * sizeof(float));
    if (!steep || !wa) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) ink[y * w + x] = sharp_font_ink(in, (uint32_t)w, (uint32_t)h, x, y);
        free(steep), free(wa);
        return;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float *a = al + (y + 2) * ap + x + 2;
            float gx = fabsf(a[1] - a[-1]), gy = fabsf(a[ap] - a[-ap]);
            steep[y * w + x] = gx > gy ? gx : gy;
            uint32_t c = in[y * w + x], m = c & 255;
            if ((c >> 8 & 255) < m) m = c >> 8 & 255;
            if ((c >> 16 & 255) < m) m = c >> 16 & 255;
            wa[2 * (y * w + x)] = a[0];
            wa[2 * (y * w + x) + 1] = a[0] * (float)m / 255.0f;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float st = 0, ws = 0, wh = 0;
            for (int j = -1; j <= 1; j++) {
                const int yy = clampi(y + j, 0, h - 1);
                for (int i = -1; i <= 1; i++) {
                    int k = yy * w + clampi(x + i, 0, w - 1);
                    if (steep[k] > st) st = steep[k];
                    ws += wa[2 * k];
                    wh += wa[2 * k + 1];
                }
            }
            ink[y * w + x] = ws > 0 ? smooth(STEEP_LO, STEEP_HI, st) * smooth(WHITE_LO, WHITE_HI, wh / ws) : 0;
        }
    free(steep), free(wa);
}

uint32_t *sharp_font_upscale(const uint32_t *in, uint32_t uw, uint32_t uh, int s) {
    if (!in || !uw || !uh || s < 1 || s > 8 || uw > 8192 || uh > 8192) return NULL;
    int w = (int)uw, h = (int)uh, ow = w * s, oh = h * s, ap = w + 4;
    uint32_t *out = malloc((size_t)ow * oh * 4);
    float *ink = malloc((size_t)w * h * sizeof(float)), *al = malloc((size_t)ap * (h + 4) * sizeof(float));
    if (!out || !ink || !al) {
        free(out), free(ink), free(al);
        return NULL;
    }
    /* alpha 0..1 with a 2-texel clamped border, so the 4 x 4 taps need no clamping */
    for (int y = -2; y < h + 2; y++)
        for (int x = -2; x < w + 2; x++) al[(y + 2) * ap + x + 2] = alpha_at(in, w, h, x, y);
    ink_map(in, w, h, al, ap, ink);
    /* the fraction and its Catmull-Rom weights repeat every s output texels; the first tap of
     * each output column and row is tabulated (no division or floor in the loops) */
    float fr[8], kr[8][4];
    for (int p = 0; p < s; p++) {
        float u = (p + 0.5f) / s - 0.5f;
        fr[p] = u - floorf(u);
        cr_weights(fr[p], kr[p]);
    }
    int *first = malloc((size_t)(ow > oh ? ow : oh) * sizeof(int));
    if (!first) {
        free(out), free(ink), free(al);
        return NULL;
    }
    for (int o = 0; o < (ow > oh ? ow : oh); o++) first[o] = (int)floorf((o + 0.5f) / s - 0.5f);

    for (int oy = 0, py = 0; oy < oh; oy++, py = py + 1 == s ? 0 : py + 1) {
        int y0 = first[oy];
        float fy = fr[py];
        const float *ky = kr[py];
        int ya = clampi(y0, 0, h - 1), yb = clampi(y0 + 1, 0, h - 1);
        for (int ox = 0, px = 0; ox < ow; ox++, px = px + 1 == s ? 0 : px + 1) {
            int x0 = first[ox];
            float fx = fr[px];
            int xa = clampi(x0, 0, w - 1), xb = clampi(x0 + 1, 0, w - 1);
            uint32_t c00 = in[ya * w + xa], c10 = in[ya * w + xb], c01 = in[yb * w + xa], c11 = in[yb * w + xb];
            float i00 = ink[ya * w + xa], i10 = ink[ya * w + xb], i01 = ink[yb * w + xa], i11 = ink[yb * w + xb];
            if (c00 == c10 && c00 == c01 && c00 == c11 && i00 + i10 + i01 + i11 <= 0) {
                out[oy * ow + ox] = c00;   /* flat area: every interpolation gives the texel itself */
                continue;
            }
            float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
            /* bilinear, straight channels: what the GPU samples from the original */
            float bil[4];
            for (int k = 0; k < 4; k++) {
                int sh = 8 * k;
                bil[k] = (float)(c00 >> sh & 255) * w00 + (float)(c10 >> sh & 255) * w10 + (float)(c01 >> sh & 255) * w01 +
                         (float)(c11 >> sh & 255) * w11;
            }
            float g = i00 * w00 + i10 * w10 + i01 * w01 + i11 * w11;
            if (g > 0) {
                /* rebuilt edge: Catmull-Rom alpha cut at half the local ink level */
                const float *kx = kr[px];
                float a = 0, m = 0;
                for (int j = 0; j < 4; j++) {
                    const float *row = al + (y0 + 1 + j) * ap + x0 + 1;
                    float r = 0;
                    for (int i = 0; i < 4; i++) {
                        r += kx[i] * row[i];
                        if (row[i] > m) m = row[i];
                    }
                    a += ky[j] * r;
                }
                float half = EDGE_HALF * m / (float)s;
                float sa = m * smooth(0.5f * m - half, 0.5f * m + half, a);
                /* straight ink colour: the alpha-weighted mean of the bilinear taps */
                float a00 = (float)(c00 >> 24) * w00, a10 = (float)(c10 >> 24) * w10, a01 = (float)(c01 >> 24) * w01,
                      a11 = (float)(c11 >> 24) * w11, aw = a00 + a10 + a01 + a11;
                for (int k = 0; k < 3; k++) {
                    int sh = 8 * k;
                    float sc = aw > 0 ? ((float)(c00 >> sh & 255) * a00 + (float)(c10 >> sh & 255) * a10 +
                                         (float)(c01 >> sh & 255) * a01 + (float)(c11 >> sh & 255) * a11) / aw
                                      : bil[k];
                    bil[k] += g * (sc - bil[k]);
                }
                bil[3] += g * (sa * 255.0f - bil[3]);
            }
            uint32_t c = 0;
            for (int k = 0; k < 4; k++) {
                float q = bil[k] + 0.5f;
                c |= (uint32_t)(q < 0 ? 0 : q >= 255 ? 255 : (int)q) << (8 * k);
            }
            out[oy * ow + ox] = c;
        }
    }
    free(ink), free(al), free(first);
    return out;
}
