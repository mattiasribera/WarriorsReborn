/* Game-independent test of the Xbox texture helpers (src/xtex.c): D3DPixelContainer Format/Size
 * parsing, level sizes and offsets, texel conversion to B8G8R8A8 for the 8/16/32-bit formats,
 * Morton unswizzling of whole levels, linear pitches, CPU DXT1/3/5 decoding (blocks stored in
 * row-major order) and the texture cache key. Usage: xtex_test */
#include "../src/xtex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* Format dword of a 2D texture: dimensionality 2, code, levels, log2 sizes. */
static uint32_t fmt2d(uint32_t code, uint32_t levels, uint32_t ulog, uint32_t vlog) {
    return (2u << 4) | (code << 8) | (levels << 16) | (ulog << 20) | (vlog << 24);
}

static void test_info(void) {
    XTexInfo t;
    int r = xtex_info(fmt2d(0x0C, 9, 8, 7), 0, &t);
    CHECK(r == 0 && t.code == 0x0C && t.w == 256 && t.h == 128 && t.levels == 9 && !t.linear && t.dxt,
          "DXT1 256x128 9 levels: r %d code %02x %ux%u levels %u linear %d dxt %d", r, t.code, t.w, t.h, t.levels, t.linear, t.dxt);
    CHECK(xtex_level_bytes(&t, 0) == 64 * 32 * 8 && xtex_level_offset(&t, 1) == 16384 && xtex_level_offset(&t, 2) == 16384 + 4096,
          "DXT1 level bytes %u, offsets %u %u", xtex_level_bytes(&t, 0), xtex_level_offset(&t, 1), xtex_level_offset(&t, 2));
    CHECK(xtex_level_bytes(&t, 8) == 8, "DXT1 1x1 level (index 8) is one block: %u bytes", xtex_level_bytes(&t, 8));
    r = xtex_info(fmt2d(0x06, 1, 3, 2), 0, &t);
    CHECK(r == 0 && t.w == 8 && t.h == 4 && t.levels == 1 && xtex_level_bytes(&t, 0) == 128 && t.pitch == 32,
          "A8R8G8B8 8x4: %ux%u levels %u bytes %u pitch %u", t.w, t.h, t.levels, xtex_level_bytes(&t, 0), t.pitch);
    r = xtex_info(fmt2d(0x05, 0, 2, 2), 0, &t);
    CHECK(r == 0 && t.levels == 1, "levels field 0 counts as one level (%u)", t.levels);
    /* Linear: Size = width-1 | height-1 << 12 | pitch/64-1 << 24; levels forced to 1. */
    r = xtex_info((2u << 4) | (0x12u << 8) | (1u << 16), 99 | (49u << 12) | (6u << 24), &t);
    CHECK(r == 0 && t.linear && t.w == 100 && t.h == 50 && t.pitch == 448 && t.levels == 1 && xtex_level_bytes(&t, 0) == 448 * 50,
          "LIN_A8R8G8B8 100x50: linear %d %ux%u pitch %u levels %u bytes %u", t.linear, t.w, t.h, t.pitch, t.levels,
          xtex_level_bytes(&t, 0));
    CHECK(xtex_info(fmt2d(0x0C, 1, 4, 4) | 4u, 0, &t) < 0, "cube maps are rejected");
    CHECK(xtex_info((3u << 4) | (0x06u << 8) | (1u << 16) | (2u << 20) | (2u << 24) | (2u << 28), 0, &t) < 0, "volume textures are rejected");
    CHECK(xtex_info(fmt2d(0x0B, 1, 4, 4), 0, &t) < 0, "P8 (needs a palette) is rejected");
}

static void test_texels(void) {
    static const struct { uint32_t code, in, want; const char *name; } cases[] = {
        {0x00, 0x7F, 0xFF7F7F7Fu, "L8"},
        {0x13, 0x10, 0xFF101010u, "LIN_L8"},
        {0x01, 0x40, 0x40404040u, "AL8"},
        {0x19, 0x80, 0x80FFFFFFu, "A8 (white, alpha)"},
        {0x1A, 0x8040, 0x80404040u, "A8L8"},
        {0x02, 0x8000 | 0x7C00, 0xFFFF0000u, "A1R5G5B5 opaque red"},
        {0x02, 0x001F, 0x000000FFu, "A1R5G5B5 transparent blue"},
        {0x03, 0x03E0, 0xFF00FF00u, "X1R5G5B5 green, alpha forced"},
        {0x10, 0xFFFF, 0xFFFFFFFFu, "LIN_A1R5G5B5 white"},
        {0x04, 0xF0F0, 0xFF00FF00u, "A4R4G4B4"},
        {0x04, 0x1234, 0x11223344u, "A4R4G4B4 nibbles"},
        {0x05, 0x07E0, 0xFF00FF00u, "R5G6B5 green"},
        {0x11, 0x8410, 0xFF848284u, "LIN_R5G6B5 mid grey"},
        {0x06, 0x80123456, 0x80123456u, "A8R8G8B8"},
        {0x07, 0x00123456, 0xFF123456u, "X8R8G8B8 alpha forced"},
        {0x1E, 0x7F123456, 0xFF123456u, "LIN_X8R8G8B8 alpha forced"},
        {0x3A, 0x80112233, 0x80332211u, "A8B8G8R8"},
        {0x3B, 0x11223344, 0x44332211u, "B8G8R8A8"},
        {0x3C, 0x11223344, 0x44112233u, "R8G8B8A8"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint8_t b[4];
        memcpy(b, &cases[i].in, 4);
        uint32_t got = 0;
        int r = xtex_texel(cases[i].code, b, &got);
        CHECK(r == 0 && got == cases[i].want, "%s %x -> %08x (want %08x)", cases[i].name, cases[i].in, got, cases[i].want);
    }
    uint32_t got;
    CHECK(xtex_texel(0x0C, (const uint8_t *)"\0\0\0\0", &got) < 0, "DXT1 is not a per-texel format");
}

/* Swizzles a w x h image of bpt-byte texels from a pattern, decodes it and compares. */
static void check_unswizzle(uint32_t code, uint32_t bpt, uint32_t w, uint32_t h, const char *name) {
    uint8_t *swz = calloc(w * h, bpt);
    uint32_t *want = calloc(w * h, 4), *out = calloc(w * h, 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = (x * 37 + y * 101 + 5) * 0x01010101u ^ (y << 4);
            uint8_t *p = swz + (size_t)xbox_swizzle_offset(x, y, w, h) * bpt;
            memcpy(p, &v, bpt);
            xtex_texel(code, p, &want[y * w + x]);
        }
    XTexInfo t;
    xtex_info(fmt2d(code, 1, __builtin_ctz(w), __builtin_ctz(h)), 0, &t);
    int r = xtex_decode_level(&t, 0, swz, out);
    CHECK(r == 0 && !memcmp(out, want, (size_t)w * h * 4), "%s %ux%u unswizzled (r %d, texel(1,0) %08x want %08x)", name, w, h, r,
          out[1], want[1]);
    free(swz);
    free(want);
    free(out);
}

static void test_unswizzle(void) {
    check_unswizzle(0x00, 1, 16, 8, "L8");
    check_unswizzle(0x05, 2, 8, 32, "R5G6B5");
    check_unswizzle(0x04, 2, 4, 4, "A4R4G4B4");
    check_unswizzle(0x06, 4, 32, 4, "A8R8G8B8");
    /* Second mip level of a 4x4 A8R8G8B8: 2x2 at offset 64, swizzled by its own size. */
    XTexInfo t;
    xtex_info(fmt2d(0x06, 2, 2, 2), 0, &t);
    uint32_t data[16 + 4] = {0}, out[4];
    data[16 + 0] = 0xFF000001u;
    data[16 + 1] = 0xFF000002u;   /* (1,0) */
    data[16 + 2] = 0xFF000003u;   /* (0,1) */
    data[16 + 3] = 0xFF000004u;
    int r = xtex_decode_level(&t, 1, (uint8_t *)data, out);
    CHECK(r == 0 && out[0] == 0xFF000001u && out[1] == 0xFF000002u && out[2] == 0xFF000003u && out[3] == 0xFF000004u,
          "mip level 1 (2x2) read at its offset: %08x %08x %08x %08x", out[0], out[1], out[2], out[3]);
    /* Linear L8 3x2 with a 64-byte pitch. */
    uint8_t lin[128];
    memset(lin, 0xEE, sizeof lin);
    lin[0] = 1; lin[1] = 2; lin[2] = 3; lin[64] = 4; lin[65] = 5; lin[66] = 6;
    xtex_info((2u << 4) | (0x13u << 8) | (1u << 16), 2 | (1u << 12) | (0u << 24), &t);
    uint32_t lo[6];
    r = xtex_decode_level(&t, 0, lin, lo);
    CHECK(r == 0 && lo[0] == 0xFF010101u && lo[2] == 0xFF030303u && lo[3] == 0xFF040404u && lo[5] == 0xFF060606u,
          "LIN_L8 3x2 pitch 64: %08x %08x %08x %08x", lo[0], lo[2], lo[3], lo[5]);
}

/* One DXT1 color block: c0, c1 (R5G6B5) and 2-bit indices, texel 0 in the low bits. */
static void dxt1_block(uint8_t *b, uint16_t c0, uint16_t c1, uint32_t idx) {
    memcpy(b, &c0, 2);
    memcpy(b + 2, &c1, 2);
    memcpy(b + 4, &idx, 4);
}

static void test_dxt(void) {
    XTexInfo t;
    uint8_t blk[16];
    uint32_t out[16];
    /* 4-color mode (c0 > c1): red, blue, 2/3 red + 1/3 blue, 1/3 red + 2/3 blue. */
    dxt1_block(blk, 0xF800, 0x001F, 0x0u | 1u << 2 | 2u << 4 | 3u << 6);
    xtex_info(fmt2d(0x0C, 1, 2, 2), 0, &t);
    int r = xtex_decode_level(&t, 0, blk, out);
    CHECK(r == 0 && out[0] == 0xFFFF0000u && out[1] == 0xFF0000FFu && out[2] == 0xFFAA0055u && out[3] == 0xFF5500AAu && out[4] == 0xFFFF0000u,
          "DXT1 4-color: %08x %08x %08x %08x %08x", out[0], out[1], out[2], out[3], out[4]);
    /* 3-color mode (c0 <= c1): index 2 = midpoint, index 3 = transparent black. */
    dxt1_block(blk, 0x001F, 0xF800, 2u | 3u << 2);
    r = xtex_decode_level(&t, 0, blk, out);
    CHECK(r == 0 && out[0] == 0xFF7F007Fu && out[1] == 0x00000000u, "DXT1 3-color: %08x %08x", out[0], out[1]);
    /* DXT3: explicit 4-bit alpha (texel 0 = 0xF, texel 1 = 0x0, texel 2 = 0x8), color always 4-color. */
    memset(blk, 0, 8);
    blk[0] = 0x0F;
    blk[1] = 0x08;
    dxt1_block(blk + 8, 0x001F, 0xF800, 0);   /* c0 < c1 would be 3-color in DXT1 */
    xtex_info(fmt2d(0x0E, 1, 2, 2), 0, &t);
    r = xtex_decode_level(&t, 0, blk, out);
    CHECK(r == 0 && out[0] == 0xFF0000FFu && out[1] == 0x000000FFu && out[2] == 0x880000FFu, "DXT3 alpha: %08x %08x %08x", out[0], out[1],
          out[2]);
    dxt1_block(blk + 8, 0x001F, 0xF800, 3u);
    xtex_decode_level(&t, 0, blk, out);
    CHECK((out[0] & 0xFFFFFFu) == 0xAA0055u, "DXT3 index 3 interpolates (no transparent mode): %08x", out[0]);
    /* DXT5: a0 = 255 > a1 = 0: indices 0, 1, 2 (= (6*a0 + a1)/7 = 218), 7 (= (a0 + 6*a1)/7 = 36). */
    memset(blk, 0, 16);
    blk[0] = 255;
    blk[1] = 0;
    uint64_t ai = 0u | 1u << 3 | 2u << 6 | 7u << 9;
    memcpy(blk + 2, &ai, 6);
    dxt1_block(blk + 8, 0xFFFF, 0x0000, 0);
    xtex_info(fmt2d(0x0F, 1, 2, 2), 0, &t);
    r = xtex_decode_level(&t, 0, blk, out);
    CHECK(r == 0 && out[0] == 0xFFFFFFFFu && out[1] >> 24 == 0 && out[2] >> 24 == 218 && out[3] >> 24 == 36,
          "DXT5 8-alpha: %02x %02x %02x %02x", out[0] >> 24, out[1] >> 24, out[2] >> 24, out[3] >> 24);
    /* a0 <= a1: 6-alpha mode, index 6 = 0, index 7 = 255. */
    blk[0] = 0;
    blk[1] = 100;
    ai = 6u | 7u << 3 | 2u << 6;
    memcpy(blk + 2, &ai, 6);
    xtex_decode_level(&t, 0, blk, out);
    CHECK(out[0] >> 24 == 0 && out[1] >> 24 == 255 && out[2] >> 24 == 20, "DXT5 6-alpha: %u %u %u", out[0] >> 24, out[1] >> 24,
          out[2] >> 24);
    /* 8x8 DXT1: blocks in row-major order, block 1 is the top-right 4x4. */
    uint8_t four[32];
    dxt1_block(four, 0xF800, 0, 0);
    dxt1_block(four + 8, 0x001F, 0, 0);
    dxt1_block(four + 16, 0x07E0, 0, 0);
    dxt1_block(four + 24, 0xFFFF, 0, 0);
    uint32_t big[64];
    xtex_info(fmt2d(0x0C, 1, 3, 3), 0, &t);
    r = xtex_decode_level(&t, 0, four, big);
    CHECK(r == 0 && big[0] == 0xFFFF0000u && big[4] == 0xFF0000FFu && big[4 * 8] == 0xFF00FF00u && big[63] == 0xFFFFFFFFu,
          "DXT1 8x8 block order: %08x %08x %08x %08x", big[0], big[4], big[32], big[63]);
    /* 2x2 mip level of a DXT1 texture: one block, only the top-left texels used. */
    xtex_info(fmt2d(0x0C, 2, 2, 2), 0, &t);
    uint8_t two[16];
    dxt1_block(two, 0, 0, 0);
    dxt1_block(two + 8, 0x001F, 0, 0);
    uint32_t small[4];
    r = xtex_decode_level(&t, 1, two, small);
    CHECK(r == 0 && small[0] == 0xFF0000FFu && small[3] == 0xFF0000FFu, "DXT1 2x2 level: %08x %08x", small[0], small[3]);
}

static void test_key(void) {
    uint8_t data[4096];
    for (int i = 0; i < 4096; i++) data[i] = (uint8_t)(i * 13);
    uint32_t f = fmt2d(0x0C, 1, 6, 6);   /* DXT1 64x64 = 2048 bytes */
    uint64_t k = xtex_key(0x123000, f, 0, data, 2048);
    CHECK(k == xtex_key(0x123000, f, 0, data, 2048), "key is deterministic");
    CHECK(k != xtex_key(0x124000, f, 0, data, 2048), "key depends on the data address");
    CHECK(k != xtex_key(0x123000, fmt2d(0x0F, 1, 6, 6), 0, data, 2048), "key depends on the format");
    CHECK(k != xtex_key(0x123000, f, 1, data, 2048), "key depends on the size dword");
    data[0] ^= 1;
    CHECK(k != xtex_key(0x123000, f, 0, data, 2048), "key changes when the first bytes change");
    data[0] ^= 1;
    data[2047] ^= 1;
    CHECK(k != xtex_key(0x123000, f, 0, data, 2048), "key changes when the last bytes change");
    data[2047] ^= 1;
    data[3000] ^= 1;
    CHECK(k == xtex_key(0x123000, f, 0, data, 2048), "bytes past the texture do not affect the key");
}

int main(void) {
    test_info();
    test_texels();
    test_unswizzle();
    test_dxt();
    test_key();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
