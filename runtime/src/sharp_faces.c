/* --sharp-faces: see sharp_faces.h. */
#include "sharp_faces.h"
#include <stdlib.h>
#include <string.h>

int sharp_faces_arg(SharpFacesConfig *c, const char *opt, const char *val) {
    (void)val;
    if (!strcmp(opt, "--sharp-faces")) c->on = 1;
    else if (!strcmp(opt, "--no-sharp-faces")) c->on = 0;
    else if (!strcmp(opt, "--sharp-faces-over-pack")) c->over_pack = 1;
    else if (!strcmp(opt, "--no-sharp-faces-over-pack")) c->over_pack = 0;
    else return 0;
    return 1;
}

static int pow2_in(uint32_t v) { return v >= SHARP_FACES_MIN && v <= SHARP_FACES_MAX && !(v & (v - 1)); }

int sharp_faces_applies(const SharpFacesConfig *c, int character, int stage, int source, uint32_t code, uint32_t w, uint32_t h) {
    int source_ok = source == SHARP_FACES_SRC_GUEST || (source == SHARP_FACES_SRC_PACK && c->over_pack);
    return c->on && character && stage == 0 && source_ok && code == SHARP_FACES_DXT1 && pow2_in(w) && pow2_in(h);
}

/* ---- 1. DXT1 deblock ---- */

#define DEBLOCK_STEP 12   /* largest seam step (any colour channel) treated as an encoding seam */
#define DEBLOCK_FLAT 3    /* largest step between neighbours on each side of it */

static int cdiff(uint32_t a, uint32_t b) {
    int m = 0;
    for (int s = 0; s < 24; s += 8) {
        int d = abs((int)(a >> s & 0xFF) - (int)(b >> s & 0xFF));
        if (d > m) m = d;
    }
    return m;
}

/* One seam: the six texels p2 p1 p0 | q0 q1 q2 at src[i0 + k * step] (k = 0..5). A small step
 * between flat sides becomes a linear ramp from p2 to q2 over p1..q1 (alpha untouched). */
static void deblock_seam(const uint32_t *src, uint32_t *dst, size_t i0, size_t step) {
    uint32_t t[6];
    for (int k = 0; k < 6; k++) t[k] = src[i0 + k * step];
    int d = cdiff(t[2], t[3]);
    if (!d || d > DEBLOCK_STEP || cdiff(t[0], t[1]) > DEBLOCK_FLAT || cdiff(t[1], t[2]) > DEBLOCK_FLAT ||
        cdiff(t[3], t[4]) > DEBLOCK_FLAT || cdiff(t[4], t[5]) > DEBLOCK_FLAT)
        return;
    for (int k = 1; k <= 4; k++) {
        uint32_t o = t[k] & 0xFF000000u;
        for (int s = 0; s < 24; s += 8) {
            int a = (int)(t[0] >> s & 0xFF), b = (int)(t[5] >> s & 0xFF);
            o |= (uint32_t)((a * (5 - k) + b * k + 2) / 5) << s;
        }
        dst[i0 + k * step] = o;
    }
}

/* Vertical seams (x = 4, 8, ...) then horizontal ones, each pass reading the previous result. */
static void deblock(uint32_t *img, uint32_t *tmp, uint32_t w, uint32_t h) {
    memcpy(tmp, img, (size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 4; x + 3 <= w; x += 4) deblock_seam(tmp, img, (size_t)y * w + x - 3, 1);
    memcpy(tmp, img, (size_t)w * h * 4);
    for (uint32_t y = 4; y + 3 <= h; y += 4)
        for (uint32_t x = 0; x < w; x++) deblock_seam(tmp, img, ((size_t)y - 3) * w + x, w);
}

/* ---- 2. 2x Catmull-Rom, clamped to the 2x2 source texels ---- */

/* Output texel 2i samples the source at i - 1/4 (taps i-2..i+1), 2i+1 at i + 1/4 (taps i-1..i+2);
 * Catmull-Rom weights for t = 3/4 and 1/4, in 1/1024. */
static const int W_EVEN[4] = {-24, 232, 888, -72}, W_ODD[4] = {-72, 888, 232, -24};

static uint32_t clampi(int v, uint32_t n) { return v < 0 ? 0 : v >= (int)n ? n - 1 : (uint32_t)v; }

static uint32_t *upscale2x(const uint32_t *src, uint32_t w, uint32_t h, int binary_alpha) {
    uint32_t ow = 2 * w, oh = 2 * h;
    int32_t *hp = malloc((size_t)ow * h * 4 * sizeof *hp);   /* horizontal pass, x1024, 4 channels */
    uint32_t *out = malloc((size_t)ow * oh * 4);
    if (!hp || !out) {
        free(hp);
        free(out);
        return NULL;
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t ox = 0; ox < ow; ox++) {
            int i = (int)(ox >> 1), first = ox & 1 ? i - 1 : i - 2;
            const int *wt = ox & 1 ? W_ODD : W_EVEN;
            int32_t *o = hp + ((size_t)y * ow + ox) * 4;
            o[0] = o[1] = o[2] = o[3] = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t c = src[(size_t)y * w + clampi(first + k, w)];
                for (int s = 0; s < 4; s++) o[s] += wt[k] * (int)(c >> (8 * s) & 0xFF);
            }
        }
    for (uint32_t oy = 0; oy < oh; oy++) {
        int j = (int)(oy >> 1), first = oy & 1 ? j - 1 : j - 2;
        const int *wt = oy & 1 ? W_ODD : W_EVEN;
        uint32_t y0 = clampi(oy & 1 ? j : j - 1, h), y1 = clampi(oy & 1 ? j + 1 : j, h);
        for (uint32_t ox = 0; ox < ow; ox++) {
            int i = (int)(ox >> 1);
            uint32_t x0 = clampi(ox & 1 ? i : i - 1, w), x1 = clampi(ox & 1 ? i + 1 : i, w);
            uint32_t n[4] = {src[(size_t)y0 * w + x0], src[(size_t)y0 * w + x1], src[(size_t)y1 * w + x0], src[(size_t)y1 * w + x1]};
            uint32_t px = 0;
            for (int s = 0; s < 4; s++) {
                int64_t acc = 0;
                for (int k = 0; k < 4; k++) acc += (int64_t)wt[k] * hp[((size_t)clampi(first + k, h) * ow + ox) * 4 + s];
                int v = (int)((acc + (1 << 19)) >> 20), lo = 255, hi = 0;
                for (int q = 0; q < 4; q++) {
                    int c = (int)(n[q] >> (8 * s) & 0xFF);
                    if (c < lo) lo = c;
                    if (c > hi) hi = c;
                }
                v = v < lo ? lo : v > hi ? hi : v;
                if (s == 3 && binary_alpha) v = v >= 128 ? 255 : 0;
                px |= (uint32_t)v << (8 * s);
            }
            out[(size_t)oy * ow + ox] = px;
        }
    }
    free(hp);
    return out;
}

/* ---- 3. clamped unsharp mask ---- */

#define SHARPEN_8THS 3   /* amount: 3/8 of (texel - 3x3 blur) */

static void sharpen(uint32_t *img, uint32_t *tmp, uint32_t w, uint32_t h) {
    memcpy(tmp, img, (size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = tmp[(size_t)y * w + x], px = c & 0xFF000000u;
            for (int s = 0; s < 24; s += 8) {
                int sum = 0, lo = 255, hi = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int v = (int)(tmp[(size_t)clampi((int)y + dy, h) * w + clampi((int)x + dx, w)] >> s & 0xFF);
                        sum += v * (dx ? 1 : 2) * (dy ? 1 : 2);   /* [1 2 1] x [1 2 1] / 16 */
                        if (v < lo) lo = v;
                        if (v > hi) hi = v;
                    }
                int v = (int)(c >> s & 0xFF);
                int d = 16 * v - sum, o = v + (d * SHARPEN_8THS + (d >= 0 ? 64 : -64)) / 128;
                o = o < lo ? lo : o > hi ? hi : o;
                px |= (uint32_t)o << s;
            }
            img[(size_t)y * w + x] = px;
        }
}

uint32_t *sharp_faces_upscale(const uint32_t *bgra, uint32_t w, uint32_t h) {
    size_t n = (size_t)w * h;
    uint32_t *src = malloc(n * 4), *tmp = malloc(n * 16);
    uint32_t *out = NULL;
    if (src && tmp) {
        int binary = 1;
        for (size_t i = 0; i < n && binary; i++) binary = (bgra[i] >> 24) == 0 || (bgra[i] >> 24) == 0xFF;
        memcpy(src, bgra, n * 4);
        deblock(src, tmp, w, h);
        if ((out = upscale2x(src, w, h, binary))) sharpen(out, tmp, 2 * w, 2 * h);
    }
    free(src);
    free(tmp);
    return out;
}
