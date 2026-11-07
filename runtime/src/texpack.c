/* Texture dumps and replacement packs: see texpack.h. */
#include "texpack.h"
#include "../third_party/stb/stb_image.h"
#include "../third_party/stb/stb_image_write.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t texpack_hash(const XTexInfo *t, const uint8_t *data) {
    uint32_t hdr[4] = {t->code, t->w, t->h, 0};
    uint64_t h = fnv(0xcbf29ce484222325ull, (const uint8_t *)hdr, sizeof hdr);
    if (!t->linear) return fnv(h, data, xtex_level_bytes(t, 0));
    uint32_t row = t->w * (uint32_t)xtex_fmt_bpp(t->code) / 8;
    for (uint32_t y = 0; y < t->h; y++) h = fnv(h, data + (size_t)y * t->pitch, row);
    return h;
}

/* 0xAARRGGBB <-> RGBA bytes: swaps the R and B bytes of each texel. */
static void swap_rb(uint32_t *px, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t c = px[i];
        px[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
    }
}

int texpack_png_write(const char *path, uint32_t w, uint32_t h, const uint32_t *bgra) {
    size_t n = (size_t)w * h;
    uint32_t *rgba = malloc(n * 4 + 4);
    if (!rgba) return -1;
    memcpy(rgba, bgra, n * 4);
    swap_rb(rgba, n);
    int ok = stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)w * 4);
    free(rgba);
    return ok ? 0 : -1;
}

uint32_t *texpack_png_read(const char *path, uint32_t *w, uint32_t *h) {
    int iw, ih, comp;
    uint8_t *p = stbi_load(path, &iw, &ih, &comp, 4);
    if (!p) return NULL;
    size_t n = (size_t)iw * ih;
    uint32_t *out = malloc(n * 4 + 4);
    if (out) {
        memcpy(out, p, n * 4);
        swap_rb(out, n);
        *w = (uint32_t)iw;
        *h = (uint32_t)ih;
    }
    stbi_image_free(p);
    return out;
}

uint32_t texpack_mip_count(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
        n++;
    }
    return n;
}

uint32_t *texpack_mips(const uint32_t *bgra, uint32_t w, uint32_t h, uint32_t *levels) {
    uint32_t n = texpack_mip_count(w, h);
    size_t total = 0;
    for (uint32_t l = 0, lw = w, lh = h; l < n; l++, lw = lw > 1 ? lw >> 1 : 1, lh = lh > 1 ? lh >> 1 : 1)
        total += (size_t)lw * lh;
    uint32_t *out = malloc(total * 4);
    if (!out) return NULL;
    memcpy(out, bgra, (size_t)w * h * 4);
    const uint32_t *src = out;
    uint32_t *dst = out + (size_t)w * h, sw = w, sh = h;
    for (uint32_t l = 1; l < n; l++) {
        uint32_t dw = sw > 1 ? sw >> 1 : 1, dh = sh > 1 ? sh >> 1 : 1;
        for (uint32_t y = 0; y < dh; y++)
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t x0 = 2 * x < sw ? 2 * x : sw - 1, x1 = 2 * x + 1 < sw ? 2 * x + 1 : sw - 1;
                uint32_t y0 = 2 * y < sh ? 2 * y : sh - 1, y1 = 2 * y + 1 < sh ? 2 * y + 1 : sh - 1;
                uint32_t a = src[y0 * sw + x0], b = src[y0 * sw + x1], c = src[y1 * sw + x0], d = src[y1 * sw + x1], o = 0;
                for (int s = 0; s < 32; s += 8)
                    o |= ((((a >> s) & 0xFF) + ((b >> s) & 0xFF) + ((c >> s) & 0xFF) + ((d >> s) & 0xFF) + 2) / 4) << s;
                dst[y * dw + x] = o;
            }
        src = dst;
        dst += (size_t)dw * dh;
        sw = dw;
        sh = dh;
    }
    *levels = n;
    return out;
}

/* ---- DDS (BC packs) ---- */
size_t texpack_level_bytes(uint32_t format, uint32_t w, uint32_t h) {
    if (format == TEXPACK_BGRA8) return (size_t)w * h * 4;
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * (format == TEXPACK_BC1 ? 8 : 16);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

#define DDS_MAX_SIDE 16384u   /* larger than any device's 2D limit the renderer accepts */

const char *texpack_dds_parse(const uint8_t *buf, size_t n, TexpackDds *out) {
    if (n < 128 || memcmp(buf, "DDS ", 4)) return "not a DDS file";
    const uint8_t *d = buf + 4;
    if (rd32(d) != 124 || rd32(d + 72) != 32) return "bad DDS header size";
    uint32_t flags = rd32(d + 4), h = rd32(d + 8), w = rd32(d + 12), depth = rd32(d + 20), mips = rd32(d + 24);
    uint32_t pf_flags = rd32(d + 76), caps2 = rd32(d + 108);
    if (!w || !h || w > DDS_MAX_SIDE || h > DDS_MAX_SIDE) return "unsupported size";
    if (caps2 & 0x200) return "cube maps are not supported";
    if ((caps2 & 0x200000) || ((flags & 0x800000) && depth > 1)) return "volume textures are not supported";
    if (!(pf_flags & 0x4)) return "not block-compressed (no FourCC)";
    size_t off = 128;
    uint32_t format;
    if (!memcmp(d + 80, "DX10", 4)) {
        if (n < 148) return "truncated DX10 header";
        const uint8_t *x = d + 124;
        uint32_t dxgi = rd32(x), dim = rd32(x + 4), misc = rd32(x + 8), array = rd32(x + 12);
        if (dim != 3) return "not a 2D texture";
        if (misc & 0x4) return "cube maps are not supported";
        if (array != 1) return "texture arrays are not supported";
        if (dxgi == 71) format = TEXPACK_BC1;
        else if (dxgi == 77) format = TEXPACK_BC3;
        else if (dxgi == 98) format = TEXPACK_BC7;
        else if (dxgi == 72 || dxgi == 78 || dxgi == 99) return "sRGB formats are not supported (use UNORM)";
        else return "unsupported DXGI format (BC1, BC3 or BC7 UNORM only)";
        off = 148;
    } else if (!memcmp(d + 80, "DXT1", 4)) format = TEXPACK_BC1;
    else if (!memcmp(d + 80, "DXT5", 4)) format = TEXPACK_BC3;
    else return "unsupported FourCC (DXT1, DXT5 or DX10 only)";
    uint32_t levels = (flags & 0x20000) && mips ? mips : 1;
    if (levels != texpack_mip_count(w, h)) return "incomplete mip chain (every level down to 1x1 is required)";
    size_t size = 0;
    for (uint32_t l = 0; l < levels; l++) size += texpack_level_bytes(format, w >> l ? w >> l : 1, h >> l ? h >> l : 1);
    if (n - off != size) return n - off < size ? "truncated payload" : "trailing bytes after the last level";
    *out = (TexpackDds){format, w, h, levels, off, size};
    return NULL;
}

/* ---- CPU decode of BC replacements (BC7 as in the D3D11 / BPTC format description) ---- */
static const uint16_t bc7_part2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
/* Three subsets: texel i's subset is digit i (row-major, texel 0 first). */
static const char *const bc7_part3[64] = {
    "0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122", "0011001100220022",
    "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222", "0000111122222222", "0012001200120012",
    "0112011201120112", "0122012201220122", "0011011211221222", "0011200122002220", "0001001101121122", "0111001120012200",
    "0000112211221122", "0022002200221111", "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210",
    "0122012200110000", "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
    "0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222", "0120012001200120",
    "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011", "0011112222000011", "0101010122222222",
    "0000000021212121", "0022112200221122", "0022001100220011", "0220122102201221", "0101222222220101", "0000212121212121",
    "0101010101012222", "0222011102220111", "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002",
    "0110011001102222", "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220"};
static const uint8_t bc7_anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
static const uint8_t bc7_anchor3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
static const uint8_t bc7_anchor3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8};
static const uint8_t bc7_w2[4] = {0, 21, 43, 64}, bc7_w3[8] = {0, 9, 18, 27, 37, 46, 55, 64},
                     bc7_w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

typedef struct {
    const uint8_t *b;
    unsigned pos;
} Bits;

static uint32_t bits_get(Bits *s, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++, s->pos++) v |= (uint32_t)(s->b[s->pos >> 3] >> (s->pos & 7) & 1) << i;
    return v;
}

static const uint8_t *bc7_weights(unsigned bits) { return bits == 2 ? bc7_w2 : bits == 3 ? bc7_w3 : bc7_w4; }

void texpack_bc7_block(const uint8_t *b, uint32_t out[16]) {
    /* per mode: subsets, partition bits, rotation bits, index-selection bit, colour bits, alpha
     * bits, endpoint p-bits, shared p-bits, index bits, second index bits */
    static const uint8_t M[8][10] = {{3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
                                     {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
                                     {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};
    unsigned mode = 0;
    while (mode < 8 && !(b[0] >> mode & 1)) mode++;
    if (mode == 8) {   /* reserved: transparent black */
        memset(out, 0, 16 * sizeof *out);
        return;
    }
    const uint8_t *m = M[mode];
    Bits s = {b, mode + 1};
    unsigned ns = m[0], part = bits_get(&s, m[1]), rot = bits_get(&s, m[2]), isb = bits_get(&s, m[3]);
    uint32_t e[6][4];   /* endpoints (subset * 2 + 0/1), RGBA */
    for (unsigned c = 0; c < 3; c++)
        for (unsigned i = 0; i < ns * 2; i++) e[i][c] = bits_get(&s, m[4]);
    for (unsigned i = 0; i < ns * 2; i++) e[i][3] = m[5] ? bits_get(&s, m[5]) : 255;
    unsigned cb = m[4], ab = m[5];
    if (m[6] || m[7]) {
        uint32_t p[6];
        if (m[6])
            for (unsigned i = 0; i < ns * 2; i++) p[i] = bits_get(&s, 1);
        else
            for (unsigned i = 0; i < ns; i++) p[2 * i] = p[2 * i + 1] = bits_get(&s, 1);
        for (unsigned i = 0; i < ns * 2; i++)
            for (unsigned c = 0; c < 4; c++)
                if (c < 3 || ab) e[i][c] = e[i][c] << 1 | p[i];
        cb++;
        if (ab) ab++;
    }
    for (unsigned i = 0; i < ns * 2; i++)
        for (unsigned c = 0; c < 4; c++) {
            unsigned n = c < 3 ? cb : ab;
            if (c == 3 && !ab) continue;
            e[i][c] = e[i][c] << (8 - n) | e[i][c] >> (2 * n - 8);
        }
    uint8_t sub[16];
    for (unsigned i = 0; i < 16; i++) sub[i] = ns == 1 ? 0 : ns == 2 ? (bc7_part2[part] >> i & 1) : (uint8_t)(bc7_part3[part][i] - '0');
    unsigned anchor[3] = {0, ns == 2 ? bc7_anchor2[part] : bc7_anchor3a[part], bc7_anchor3b[part]};
    uint32_t idx[16], idx2[16];
    for (unsigned i = 0; i < 16; i++) idx[i] = bits_get(&s, m[8] - (i == anchor[sub[i]]));
    if (m[9])
        for (unsigned i = 0; i < 16; i++) idx2[i] = bits_get(&s, m[9] - (i == 0));
    for (unsigned i = 0; i < 16; i++) {
        const uint32_t *e0 = e[sub[i] * 2], *e1 = e[sub[i] * 2 + 1];
        unsigned cbits = m[8], abits = m[8], ci = idx[i], ai = idx[i];
        if (m[9]) {
            if (isb) {
                cbits = m[9];
                ci = idx2[i];
            } else {
                abits = m[9];
                ai = idx2[i];
            }
        }
        uint32_t v[4];
        for (unsigned c = 0; c < 4; c++) {
            unsigned w = c < 3 ? bc7_weights(cbits)[ci] : bc7_weights(abits)[ai];
            v[c] = ((64 - w) * e0[c] + w * e1[c] + 32) >> 6;
        }
        if (rot) {
            uint32_t t = v[3];
            v[3] = v[rot - 1];
            v[rot - 1] = t;
        }
        out[i] = v[3] << 24 | v[0] << 16 | v[1] << 8 | v[2];
    }
}

uint32_t *texpack_image_bgra(const TexpackImage *img) {
    size_t n = (size_t)img->w * img->h;
    uint32_t *px = img->data && n ? malloc(n * 4) : NULL;
    if (!px) return NULL;
    if (img->format == TEXPACK_BGRA8) {
        memcpy(px, img->data, n * 4);
        return px;
    }
    if (img->format != TEXPACK_BC1 && img->format != TEXPACK_BC3 && img->format != TEXPACK_BC7) {
        free(px);
        return NULL;
    }
    uint32_t bw = (img->w + 3) / 4, bh = (img->h + 3) / 4, bsize = img->format == TEXPACK_BC1 ? 8 : 16, blk[16];
    const uint8_t *src = img->data;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + ((size_t)by * bw + bx) * bsize;
            if (img->format == TEXPACK_BC7) texpack_bc7_block(b, blk);
            else xtex_dxt_block(img->format == TEXPACK_BC1 ? 0x0C : 0x0F, b, blk);
            for (uint32_t y = 0; y < 4 && by * 4 + y < img->h; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < img->w; x++) px[(size_t)(by * 4 + y) * img->w + bx * 4 + x] = blk[y * 4 + x];
        }
    return px;
}

static int has_ext(const char *path, const char *ext) {
    size_t a = strlen(path);
    if (a < 4) return 0;
    for (int i = 0; i < 4; i++)
        if ((path[a - 4 + i] | 32) != ext[i]) return 0;
    return 1;
}

int texpack_load_image(const char *path, TexpackImage *img, const char **why) {
    memset(img, 0, sizeof *img);
    if (!has_ext(path, ".dds")) {
        img->data = texpack_load_file(path, &img->w, &img->h, &img->levels);
        img->format = TEXPACK_BGRA8;
        if (!img->data) *why = "cannot read the PNG";
        return img->data ? 0 : -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        *why = "cannot open the file";
        return -1;
    }
    uint8_t *buf = NULL;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (n >= 128 && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    TexpackDds d;
    *why = !buf ? (n >= 0 && n < 128 ? "not a DDS file" : "cannot read the file") : texpack_dds_parse(buf, (size_t)n, &d);
    if (*why) {
        free(buf);
        return -1;
    }
    memmove(buf, buf + d.offset, d.size);   /* the payload, levels one after the other */
    *img = (TexpackImage){d.format, d.w, d.h, d.levels, buf};
    return 0;
}

/* ---- open-addressing map uint64 -> uint64 (key 0 = empty slot) ---- */
typedef struct {
    uint64_t *k, *v;
    size_t cap, n;
} Map;

static uint64_t *map_slot(const Map *m, uint64_t key) {
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (m->cap - 1);
    while (m->k[i] && m->k[i] != key) i = (i + 1) & (m->cap - 1);
    return &m->k[i];
}
static int map_get(const Map *m, uint64_t key, uint64_t *val) {
    if (!m->cap || !key) return 0;
    uint64_t *s = map_slot(m, key);
    if (!*s) return 0;
    *val = m->v[s - m->k];
    return 1;
}
static void map_put(Map *m, uint64_t key, uint64_t val) {
    if (!key) return;
    if ((m->n + 1) * 2 > m->cap) {
        Map g = {calloc(m->cap ? m->cap * 2 : 1024, 8), calloc(m->cap ? m->cap * 2 : 1024, 8), m->cap ? m->cap * 2 : 1024, 0};
        if (!g.k || !g.v) { free(g.k); free(g.v); return; }
        for (size_t i = 0; i < m->cap; i++)
            if (m->k[i]) { uint64_t *s = map_slot(&g, m->k[i]); *s = m->k[i]; g.v[s - g.k] = m->v[i]; g.n++; }
        free(m->k);
        free(m->v);
        *m = g;
    }
    uint64_t *s = map_slot(m, key);
    if (!*s) { *s = key; m->n++; }
    m->v[s - m->k] = val;
}
static void map_free(Map *m) {
    free(m->k);
    free(m->v);
    memset(m, 0, sizeof *m);
}

/* Map keys must not be 0: a (theoretical) hash 0 is stored as 1. */
static uint64_t mkey(uint64_t hash) { return hash ? hash : 1; }

/* 16 hex digits + ".png" or ".dds" (any case): the hash and *dds, or -1. */
static int parse_name(const char *name, uint64_t *hash, int *dds) {
    uint64_t h = 0;
    for (int i = 0; i < 16; i++) {
        char c = name[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        h = h << 4 | (uint64_t)d;
    }
    const char *e = name + 16;
    if (e[0] != '.' || e[4]) return -1;
    if ((e[1] | 32) == 'p' && (e[2] | 32) == 'n' && (e[3] | 32) == 'g') *dds = 0;
    else if ((e[1] | 32) == 'd' && (e[2] | 32) == 'd' && (e[3] | 32) == 's') *dds = 1;
    else return -1;
    *hash = h;
    return 0;
}

/* ---- replacement pack (layers: the first pack that has a hash wins) ---- */
typedef struct {
    char name[2][24];     /* [0] the .png, [1] the .dds, as found (case kept); "" = none or dropped */
    uint32_t dir[2];      /* index into pack.dirs of each file */
    uint32_t first;       /* the pack that indexed the hash */
} PackEntry;
static struct {
    char **dirs;
    uint32_t ndirs;
    Map index;            /* hash -> entry + 1 */
    PackEntry *e;
    size_t n, cap;
    int dds;              /* texpack_set_dds */
} pack;

/* The file a hash offers: 1 its .dds, 0 its .png, -1 none. */
static int offered(const PackEntry *pe) {
    if (pack.dds && pe->name[1][0]) return 1;
    return pe->name[0][0] ? 0 : -1;
}

int texpack_open(const char *dir) {
    texpack_close();
    return texpack_add(dir);
}

int texpack_add(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **nd = realloc(pack.dirs, (pack.ndirs + 1) * sizeof *nd);
    char *copy = strdup(dir);
    if (!nd || !copy) {
        if (nd) pack.dirs = nd;
        free(copy);
        closedir(d);
        return -1;
    }
    pack.dirs = nd;
    uint32_t di = pack.ndirs++;
    pack.dirs[di] = copy;
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        uint64_t h, v;
        int dds;
        if (parse_name(e->d_name, &h, &dds)) continue;
        if (map_get(&pack.index, mkey(h), &v)) {
            /* the other file of a hash in this pack, or a later pack's .png under a .dds-only hash */
            PackEntry *pe = &pack.e[v - 1];
            if (!pe->name[dds][0] && (pe->first == di || (!dds && pe->dir[1] == pe->first && pe->name[1][0]))) {
                snprintf(pe->name[dds], sizeof pe->name[0], "%s", e->d_name);
                pe->dir[dds] = di;
            }
            continue;
        }
        if (pack.n == pack.cap) {
            size_t c = pack.cap ? pack.cap * 2 : 256;
            PackEntry *ne = realloc(pack.e, c * sizeof *ne);
            if (!ne) break;
            pack.e = ne;
            pack.cap = c;
        }
        PackEntry *pe = &pack.e[pack.n];
        memset(pe, 0, sizeof *pe);
        snprintf(pe->name[dds], sizeof pe->name[0], "%s", e->d_name);
        pe->dir[dds] = pe->first = di;
        map_put(&pack.index, mkey(h), ++pack.n);
        added++;
    }
    closedir(d);
    return added;
}

int texpack_active(void) { return pack.ndirs != 0; }
int texpack_pack_count(void) { return (int)pack.ndirs; }

static PackEntry *entry(uint64_t hash) {
    uint64_t v;
    return map_get(&pack.index, mkey(hash), &v) ? &pack.e[v - 1] : NULL;
}

int texpack_has(uint64_t hash) {
    const PackEntry *pe = entry(hash);
    return pe && offered(pe) >= 0;
}

void texpack_set_dds(int on) { pack.dds = on != 0; }

int texpack_dds_count(void) {
    int n = 0;
    for (size_t i = 0; i < pack.n; i++) n += offered(&pack.e[i]) == 1;
    return n;
}

void texpack_disable(uint64_t hash) {
    PackEntry *pe = entry(hash);
    if (pe) pe->name[0][0] = pe->name[1][0] = 0;
}

int texpack_drop_dds(uint64_t hash) {
    PackEntry *pe = entry(hash);
    if (!pe) return 0;
    pe->name[1][0] = 0;
    return pe->name[0][0] != 0;
}

static char *file_path(const PackEntry *pe, int which) {
    size_t len = strlen(pack.dirs[pe->dir[which]]) + 32;
    char *path = malloc(len);
    if (path) snprintf(path, len, "%s/%s", pack.dirs[pe->dir[which]], pe->name[which]);
    return path;
}

char *texpack_path(uint64_t hash) {
    const PackEntry *pe = entry(hash);
    int which = pe ? offered(pe) : -1;
    return which < 0 ? NULL : file_path(pe, which);
}

uint32_t *texpack_load_file(const char *path, uint32_t *w, uint32_t *h, uint32_t *levels) {
    uint32_t iw, ih, *img = texpack_png_read(path, &iw, &ih);
    if (!img) return NULL;
    uint32_t *m = texpack_mips(img, iw, ih, levels);
    free(img);
    if (m) { *w = iw; *h = ih; }
    return m;
}

uint32_t *texpack_load(uint64_t hash, uint32_t *w, uint32_t *h, uint32_t *levels) {
    const PackEntry *pe = entry(hash);
    char *path = pe && pe->name[0][0] ? file_path(pe, 0) : NULL;
    if (!path) return NULL;
    uint32_t *m = texpack_load_file(path, w, h, levels);
    free(path);
    return m;
}

void texpack_close(void) {
    int dds = pack.dds;
    for (uint32_t i = 0; i < pack.ndirs; i++) free(pack.dirs[i]);
    free(pack.dirs);
    free(pack.e);
    map_free(&pack.index);
    memset(&pack, 0, sizeof pack);
    pack.dds = dds;   /* a setting, not pack contents */
}

/* ---- key -> hash memo ---- */
static Map memo;
int texpack_key_hash(uint64_t key, uint64_t *hash) { return map_get(&memo, key, hash); }
void texpack_key_set(uint64_t key, uint64_t hash) { map_put(&memo, key, hash); }

/* ---- dump ---- */
typedef struct {
    uint64_t hash;
    uint32_t code, w, h, levels, linear, alpha, draws, first_frame;
} DumpEntry;
static struct {
    char *dir;
    DumpEntry *e;
    size_t n, cap;
    Map index;   /* hash -> entry + 1 */
} dump;

static DumpEntry *dump_find(uint64_t hash) {
    uint64_t v;
    return map_get(&dump.index, mkey(hash), &v) ? &dump.e[v - 1] : NULL;
}

static DumpEntry *dump_add(const DumpEntry *src) {
    if (dump.n == dump.cap) {
        size_t c = dump.cap ? dump.cap * 2 : 256;
        DumpEntry *ne = realloc(dump.e, c * sizeof *ne);
        if (!ne) return NULL;
        dump.e = ne;
        dump.cap = c;
    }
    dump.e[dump.n] = *src;
    map_put(&dump.index, mkey(src->hash), ++dump.n);
    return &dump.e[dump.n - 1];
}

static void dump_free(void) {
    free(dump.dir);
    free(dump.e);
    map_free(&dump.index);
    memset(&dump, 0, sizeof dump);
}

static char *dump_path(const char *name) {
    size_t len = strlen(dump.dir) + strlen(name) + 2;
    char *p = malloc(len);
    if (p) snprintf(p, len, "%s/%s", dump.dir, name);
    return p;
}

int texpack_dump_open(const char *dir) {
    if (dump.dir) texpack_dump_close();
    make_dir(dir);
    struct stat sb;
    if (stat(dir, &sb) || !S_ISDIR(sb.st_mode)) return -1;
    dump.dir = strdup(dir);
    char *mp = dump_path("manifest.csv");
    FILE *f = mp ? fopen(mp, "r") : NULL;
    free(mp);
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        DumpEntry d = {0};
        unsigned long long h;
        if (sscanf(line, "%16llx,0x%x,%u,%u,%u,%u,%u,%u,%u", &h, &d.code, &d.w, &d.h, &d.levels, &d.linear, &d.alpha,
                   &d.draws, &d.first_frame) == 9 && !dump_find(h)) {
            d.hash = h;
            dump_add(&d);
        }
    }
    if (f) fclose(f);
    return 0;
}

int texpack_dump_active(void) { return dump.dir != NULL; }

int texpack_dump_texture(uint64_t hash, const XTexInfo *t, const uint32_t *bgra, uint32_t frame) {
    if (!dump.dir || dump_find(hash)) return 0;
    DumpEntry d = {hash, t->code, t->w, t->h, t->levels, (uint32_t)t->linear, 0, 0, frame};
    for (size_t i = 0, n = (size_t)t->w * t->h; i < n && !d.alpha; i++) d.alpha = bgra[i] >> 24 != 0xFF;
    char name[32];
    snprintf(name, sizeof name, "%016llx.png", (unsigned long long)hash);
    char *p = dump_path(name);
    int r = p ? texpack_png_write(p, t->w, t->h, bgra) : -1;
    free(p);
    if (r) return -1;
    return dump_add(&d) ? 1 : -1;
}

void texpack_dump_draw(uint64_t hash) {
    DumpEntry *d = dump_find(hash);
    if (d) d->draws++;
}

int texpack_dump_flush(void) {
    if (!dump.dir) return -1;
    char *mp = dump_path("manifest.csv");
    FILE *f = mp ? fopen(mp, "w") : NULL;
    free(mp);
    if (!f) return -1;
    fputs("hash,format,w,h,levels,linear,has_alpha,draws,first_frame\n", f);
    for (size_t i = 0; i < dump.n; i++) {
        const DumpEntry *d = &dump.e[i];
        fprintf(f, "%016llx,0x%02X,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long long)d->hash, d->code, d->w, d->h, d->levels,
                d->linear, d->alpha, d->draws, d->first_frame);
    }
    fclose(f);
    return (int)dump.n;
}

int texpack_dump_close(void) {
    int n = texpack_dump_flush();
    dump_free();
    return n;
}
