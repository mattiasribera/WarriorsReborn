/* Xbox texture helpers (see xtex.h). */
#include "xtex.h"
#include <string.h>

int xtex_fmt_linear(uint32_t c) {
    return (c >= 0x10 && c <= 0x18) || (c >= 0x1B && c <= 0x20) || c == 0x24 || c == 0x25 ||
           (c >= 0x2E && c <= 0x31) || (c >= 0x35 && c <= 0x37) || c >= 0x3F;
}
int xtex_fmt_dxt(uint32_t c) { return c == 0x0C || c == 0x0E || c == 0x0F; }
int xtex_fmt_bpp(uint32_t c) {
    switch (c) {
    case 0x0C: return 4;                                   /* DXT1 */
    case 0x0E: case 0x0F:                                  /* DXT3/DXT5 */
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F: return 8;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x16: case 0x17: case 0x1A:
    case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25: case 0x27: case 0x28: case 0x29: case 0x2C:
    case 0x2D: case 0x30: case 0x31: case 0x32: case 0x35: case 0x37: case 0x38: return 16;
    default: return 32;
    }
}
uint32_t xtex_level_pitch(uint32_t code, uint32_t w) {
    if (xtex_fmt_dxt(code)) return ((w + 3) / 4) * (code == 0x0C ? 8 : 16);
    uint32_t p = (w * xtex_fmt_bpp(code) + 7) / 8;
    return xtex_fmt_linear(code) ? (p + 63) & ~63u : p;
}
uint32_t xtex_level_size(uint32_t code, uint32_t w, uint32_t h) {
    uint32_t rows = xtex_fmt_dxt(code) ? (h + 3) / 4 : h;
    return xtex_level_pitch(code, w) * (rows ? rows : 1);
}

/* Formats xtex_texel converts (bytes per texel), DXT handled separately. */
static int texel_bytes(uint32_t c) {
    switch (c) {
    case 0x00: case 0x01: case 0x13: case 0x19: case 0x1B: case 0x1F: return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A: case 0x1C: case 0x1D: case 0x20: return 2;
    case 0x06: case 0x07: case 0x12: case 0x1E: case 0x3A: case 0x3B: case 0x3C: case 0x3F: case 0x40: case 0x41: return 4;
    default: return 0;
    }
}

int xtex_info(uint32_t f, uint32_t size, XTexInfo *t) {
    memset(t, 0, sizeof *t);
    t->code = XTEX_CODE(f);
    if ((f & XTEX_CUBEMAP) || XTEX_DIM(f) != 2 || (!xtex_fmt_dxt(t->code) && !texel_bytes(t->code))) return -1;
    t->dxt = xtex_fmt_dxt(t->code);
    t->linear = size != 0;
    if (t->linear) {
        t->w = (size & 0xFFF) + 1;
        t->h = ((size >> 12) & 0xFFF) + 1;
        t->pitch = ((size >> 24) + 1) * 64;
        t->levels = 1;
    } else {
        t->w = 1u << XTEX_ULOG(f);
        t->h = 1u << XTEX_VLOG(f);
        t->pitch = xtex_level_pitch(t->code, t->w);
        t->levels = XTEX_LEVELS(f) ? XTEX_LEVELS(f) : 1;
    }
    return 0;
}
uint32_t xtex_level_w(const XTexInfo *t, uint32_t l) { return t->w >> l ? t->w >> l : 1; }
uint32_t xtex_level_h(const XTexInfo *t, uint32_t l) { return t->h >> l ? t->h >> l : 1; }
uint32_t xtex_level_bytes(const XTexInfo *t, uint32_t l) {
    if (t->linear) return t->pitch * t->h;
    return xtex_level_size(t->code, xtex_level_w(t, l), xtex_level_h(t, l));
}
uint32_t xtex_level_offset(const XTexInfo *t, uint32_t level) {
    uint32_t off = 0;
    for (uint32_t l = 0; l < level; l++) off += xtex_level_bytes(t, l);
    return off;
}
uint32_t xtex_total_bytes(const XTexInfo *t) { return xtex_level_offset(t, t->levels); }

uint32_t xbox_swizzle_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t off = 0, bit = 0;
    for (uint32_t m = 1; m < w || m < h; m <<= 1) {
        if (m < w) off |= (x & m ? 1u : 0u) << bit++;
        if (m < h) off |= (y & m ? 1u : 0u) << bit++;
    }
    return off;
}

void xbox_unswizzle(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h, uint32_t bpt, uint32_t dst_pitch) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = src + (size_t)xbox_swizzle_offset(x, y, w, h) * bpt;
            uint8_t *o = dst + (size_t)y * dst_pitch + (size_t)x * bpt;
            for (uint32_t b = 0; b < bpt; b++) o[b] = s[b];
        }
}

/* n-bit channel to 8 bits by bit replication. */
static uint32_t x5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t x6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t x4(uint32_t v) { return v * 0x11; }
static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }

int xtex_texel(uint32_t c, const uint8_t *p, uint32_t *out) {
    uint32_t v = p[0];
    if (texel_bytes(c) >= 2) v |= (uint32_t)p[1] << 8;
    if (texel_bytes(c) == 4) v |= (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    switch (c) {
    case 0x00: case 0x13: *out = argb(255, v, v, v); return 0;                 /* L8 */
    case 0x01: case 0x1B: *out = argb(v, v, v, v); return 0;                   /* AL8 */
    case 0x19: case 0x1F: *out = argb(v, 255, 255, 255); return 0;             /* A8 */
    case 0x1A: case 0x20: *out = argb(v >> 8, v & 0xFF, v & 0xFF, v & 0xFF); return 0;   /* A8L8 */
    case 0x02: case 0x10:                                                      /* A1R5G5B5 */
        *out = argb(v & 0x8000 ? 255 : 0, x5(v >> 10 & 31), x5(v >> 5 & 31), x5(v & 31));
        return 0;
    case 0x03: case 0x1C: *out = argb(255, x5(v >> 10 & 31), x5(v >> 5 & 31), x5(v & 31)); return 0;   /* X1R5G5B5 */
    case 0x04: case 0x1D: *out = argb(x4(v >> 12), x4(v >> 8 & 15), x4(v >> 4 & 15), x4(v & 15)); return 0;
    case 0x05: case 0x11: *out = argb(255, x5(v >> 11), x6(v >> 5 & 63), x5(v & 31)); return 0;        /* R5G6B5 */
    case 0x06: case 0x12: *out = v; return 0;                                  /* A8R8G8B8 */
    case 0x07: case 0x1E: *out = v | 0xFF000000u; return 0;                    /* X8R8G8B8 */
    case 0x3A: case 0x3F: *out = argb(v >> 24, v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF); return 0;   /* A8B8G8R8 */
    case 0x3B: case 0x40: *out = argb(v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24); return 0;   /* B8G8R8A8 */
    case 0x3C: case 0x41: *out = argb(v & 0xFF, v >> 24, v >> 16 & 0xFF, v >> 8 & 0xFF); return 0;   /* R8G8B8A8 */
    default: return -1;
    }
}

/* DXT color block (c0, c1, 2-bit indices); four_color forces the 4-color mode (DXT3/5). */
static void color_block(const uint8_t *b, int four_color, uint32_t out[16]) {
    uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8, idx = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    uint32_t r[4], g[4], bl[4], a[4] = {255, 255, 255, 255};
    r[0] = x5(c0 >> 11); g[0] = x6(c0 >> 5 & 63); bl[0] = x5(c0 & 31);
    r[1] = x5(c1 >> 11); g[1] = x6(c1 >> 5 & 63); bl[1] = x5(c1 & 31);
    if (four_color || c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; bl[2] = (2 * bl[0] + bl[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; bl[3] = (bl[0] + 2 * bl[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; bl[2] = (bl[0] + bl[1]) / 2;
        r[3] = g[3] = bl[3] = a[3] = 0;   /* transparent black */
    }
    for (int i = 0; i < 16; i++) {
        uint32_t k = idx >> (2 * i) & 3;
        out[i] = argb(a[k], r[k], g[k], bl[k]);
    }
}

void xtex_dxt_block(uint32_t code, const uint8_t *b, uint32_t out[16]) {
    if (code == 0x0C) { color_block(b, 0, out); return; }
    color_block(b + 8, 1, out);
    uint32_t a[16];
    if (code == 0x0E) {   /* DXT3: explicit 4-bit alpha */
        for (int i = 0; i < 16; i++) a[i] = x4(b[i / 2] >> (4 * (i & 1)) & 15);
    } else {              /* DXT5: interpolated alpha, 3-bit indices */
        uint32_t a0 = b[0], a1 = b[1], pal[8] = {a0, a1};
        if (a0 > a1) {
            for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        } else {
            for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
            pal[6] = 0;
            pal[7] = 255;
        }
        uint64_t idx = 0;
        for (int i = 0; i < 6; i++) idx |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) a[i] = pal[idx >> (3 * i) & 7];
    }
    for (int i = 0; i < 16; i++) out[i] = (out[i] & 0xFFFFFFu) | a[i] << 24;
}

int xtex_decode_level(const XTexInfo *t, uint32_t level, const uint8_t *data, uint32_t *out) {
    uint32_t w = xtex_level_w(t, level), h = xtex_level_h(t, level);
    const uint8_t *src = data + xtex_level_offset(t, level);
    if (t->dxt) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = t->code == 0x0C ? 8 : 16, blk[16];
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                xtex_dxt_block(t->code, src + (size_t)(by * bw + bx) * bsize, blk);
                for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++) out[(size_t)(by * 4 + y) * w + bx * 4 + x] = blk[y * 4 + x];
            }
        return 0;
    }
    uint32_t bpt = (uint32_t)texel_bytes(t->code);
    if (!bpt) return -1;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t off = t->linear ? (size_t)y * t->pitch + (size_t)x * bpt : (size_t)xbox_swizzle_offset(x, y, w, h) * bpt;
            xtex_texel(t->code, src + off, &out[(size_t)y * w + x]);
        }
    return 0;
}

/* FNV-1a, 64 bits. */
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

uint64_t xtex_key(uint32_t data, uint32_t format, uint32_t size, const uint8_t *bytes, uint32_t len) {
    uint32_t hdr[3] = {data, format, size};
    uint64_t h = fnv(0xcbf29ce484222325ull, hdr, sizeof hdr);
    if (!bytes || !len) return h;
    uint32_t edge = len < 16 ? len : 16;
    h = fnv(h, bytes, edge);
    h = fnv(h, bytes + len - edge, edge);
    if (len >= 64 * 4)
        for (uint32_t i = 0; i < 64; i++) h = fnv(h, bytes + (size_t)(len / 4 / 64) * 4 * i, 4);
    return h;
}
