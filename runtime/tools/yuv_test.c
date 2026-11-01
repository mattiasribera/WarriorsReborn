/* Game-independent test of the YUV quad helpers (src/yuv.c): Morton swizzle offsets and
 * unswizzling, coefficient extraction from a synthetic Bink-style pixel shader definition, the
 * BT.601 fallback, and the textured quad with scaling and clipping. Usage: yuv_test */
#include "../src/yuv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define R(px) (((px) >> 16) & 0xFF)
#define Gc(px) (((px) >> 8) & 0xFF)
#define B(px) ((px) & 0xFF)

static void test_swizzle(void) {
    static const uint32_t sq[][3] = {{0, 0, 0}, {1, 0, 1}, {0, 1, 2}, {1, 1, 3}, {2, 0, 4}, {0, 2, 8}, {3, 3, 15}, {2, 1, 6}};
    for (unsigned i = 0; i < sizeof sq / sizeof *sq; i++) {
        uint32_t o = xbox_swizzle_offset(sq[i][0], sq[i][1], 4, 4);
        CHECK(o == sq[i][2], "4x4 swizzle (%u,%u) -> %u (want %u)", sq[i][0], sq[i][1], o, sq[i][2]);
    }
    /* 8x2: x0, y0, x1, x2 (y has one bit) */
    static const uint32_t wide[][3] = {{1, 0, 1}, {0, 1, 2}, {2, 0, 4}, {4, 0, 8}, {2, 1, 6}, {7, 1, 15}};
    for (unsigned i = 0; i < sizeof wide / sizeof *wide; i++) {
        uint32_t o = xbox_swizzle_offset(wide[i][0], wide[i][1], 8, 2);
        CHECK(o == wide[i][2], "8x2 swizzle (%u,%u) -> %u (want %u)", wide[i][0], wide[i][1], o, wide[i][2]);
    }
    /* 2x8: x0, y0, y1, y2 */
    CHECK(xbox_swizzle_offset(0, 4, 2, 8) == 8, "2x8 swizzle (0,4) -> %u (want 8)", xbox_swizzle_offset(0, 4, 2, 8));
    CHECK(xbox_swizzle_offset(1, 2, 2, 8) == 5, "2x8 swizzle (1,2) -> %u (want 5)", xbox_swizzle_offset(1, 2, 2, 8));

    /* Round trip: swizzle a 16x8 pattern by offsets, unswizzle into a padded pitch. */
    enum { W = 16, H = 8, P = 20 };
    uint8_t lin[W * H], swz[W * H], out[P * H];
    for (int i = 0; i < W * H; i++) lin[i] = (uint8_t)(i * 7 + 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) swz[xbox_swizzle_offset(x, y, W, H)] = lin[y * W + x];
    memset(out, 0xEE, sizeof out);
    xbox_unswizzle(swz, out, W, H, 1, P);
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) bad += out[y * P + x] != lin[y * W + x];
    CHECK(!bad && out[W] == 0xEE, "16x8 unswizzle round trip (%d mismatches, padding kept)", bad);

    /* 32-bit texels move as units. */
    uint32_t s32[4 * 4], l32[4 * 4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) s32[xbox_swizzle_offset(x, y, 4, 4)] = 0x01000000u * x + y;
    xbox_unswizzle((uint8_t *)s32, (uint8_t *)l32, 4, 4, 4, 16);
    CHECK(l32[2 * 4 + 3] == 0x03000002u, "4x4 32-bit unswizzle texel (3,2) = %08x", l32[2 * 4 + 3]);
}

/* Synthetic D3DPIXELSHADERDEF with Bink's YUV program (fields in Xbox order). */
enum { ALPHA_IN = 0, FINAL_ABCD = 8, FINAL_EFG = 9, C0 = 10, C1 = 18, ALPHA_OUT = 26, RGB_IN = 34, COMPARE = 42,
       FC0 = 43, FC1 = 44, RGB_OUT = 45, COMBINER_COUNT = 53, TEX_MODES = 54 };
static void bink_psdef(uint32_t *d) {
    static const uint32_t rgb_in[7] = {0xc8c1c9c2, 0xcac1c821, 0xc820e120, 0xc8c1c8c2, 0xc841c0c0, 0xccc1c9c2, 0xcac1cc21};
    static const uint32_t rgb_out[7] = {0x800, 0x800, 0x800, 0x000e30ca, 0x00092090, 0xc00, 0xc00};
    static const uint32_t c0[7] = {0x00ff0000, 0xff0000ff, 0x00108080, 0x004a6600, 0x00ca4a67, 0x00ff0000, 0xff0000ff};
    static const uint32_t c1[7] = {0x0000ff00, 0, 0, 0x004a0081, 0, 0x0000ff00, 0};
    memset(d, 0, PSDEF_DWORDS * 4);
    for (int i = 0; i < 7; i++) {
        d[RGB_IN + i] = rgb_in[i];
        d[RGB_OUT + i] = rgb_out[i];
        d[C0 + i] = c0[i];
        d[C1 + i] = c1[i];
    }
    d[FINAL_ABCD] = 0x00000f00;
    d[FINAL_EFG] = 0x0c041480;
    d[COMBINER_COUNT] = 0x00011107;
    d[TEX_MODES] = 0x00000421;
}

static void check_colors(const char *what, const YuvCoeffs *c) {
    uint32_t white = yuv_to_xrgb(c, 235, 128, 128), black = yuv_to_xrgb(c, 16, 128, 128);
    uint32_t gray = yuv_to_xrgb(c, 126, 128, 128);
    CHECK(R(white) >= 250 && Gc(white) >= 250 && B(white) >= 250, "%s: Y=235 neutral -> white (%06x)", what, white);
    CHECK(black == 0, "%s: Y=16 neutral -> black (%06x)", what, black);
    CHECK(abs((int)R(gray) - 128) <= 2 && abs((int)R(gray) - (int)Gc(gray)) <= 1 && abs((int)Gc(gray) - (int)B(gray)) <= 1, "%s: Y=126 neutral -> mid gray (%06x)", what, gray);
    /* BT.601 red (255,0,0) is Y=81 Cb=90 Cr=240; plane 1 = Cr, plane 2 = Cb. */
    uint32_t red = yuv_to_xrgb(c, 81, 240, 90), blue = yuv_to_xrgb(c, 41, 110, 240);
    CHECK(R(red) >= 245 && Gc(red) <= 10 && B(red) <= 10, "%s: (81, Cr 240, Cb 90) -> red (%06x)", what, red);
    CHECK(R(blue) <= 10 && Gc(blue) <= 10 && B(blue) >= 245, "%s: (41, Cr 110, Cb 240) -> blue (%06x)", what, blue);
    uint32_t green = yuv_to_xrgb(c, 145, 34, 54);
    CHECK(R(green) <= 10 && Gc(green) >= 245 && B(green) <= 10, "%s: (145, Cr 34, Cb 54) -> green (%06x)", what, green);
}

static void test_coeffs(void) {
    uint32_t d[PSDEF_DWORDS];
    YuvCoeffs c;
    bink_psdef(d);
    CHECK(yuv_coeffs_from_psdef(d, &c) == 1, "Bink pixel shader definition recognized");
    CHECK(fabsf(c.off[0] - 16 / 255.f) < 1e-6f && fabsf(c.off[1] - 128 / 255.f) < 1e-6f, "offsets from stage 2 c0 (%f %f %f)",
          c.off[0], c.off[1], c.off[2]);
    CHECK(fabsf(c.m[0][0] - 4 * 74 / 255.f) < 1e-5f && fabsf(c.m[0][1] - 4 * 102 / 255.f) < 1e-5f && c.m[0][2] == 0,
          "R row from stage 3 c0 x4 (%f %f %f)", c.m[0][0], c.m[0][1], c.m[0][2]);
    CHECK(fabsf(c.m[1][1] - 2 * (2 * 74 / 255.f - 1)) < 1e-5f, "G row from stage 4 c0 expanded x2 (%f %f %f)", c.m[1][0],
          c.m[1][1], c.m[1][2]);
    CHECK(fabsf(c.m[2][2] - 4 * 129 / 255.f) < 1e-5f && c.m[2][1] == 0, "B row from stage 3 c1 x4 (%f %f %f)", c.m[2][0],
          c.m[2][1], c.m[2][2]);
    check_colors("shader", &c);
    /* The coefficients come from the definition, not from a table. */
    d[C0 + 2] = 0x00008080;
    yuv_coeffs_from_psdef(d, &c);
    CHECK(c.off[0] == 0, "stage 2 c0 luma offset 0 is honored");
    bink_psdef(d);
    d[RGB_OUT + 3] = 0x800;
    CHECK(yuv_coeffs_from_psdef(d, &c) == 0, "different program rejected");
    bink_psdef(d);
    d[COMBINER_COUNT] = 0x00011106;
    CHECK(yuv_coeffs_from_psdef(d, &c) == 0, "different combiner count rejected");
    yuv_coeffs_bt601(&c);
    check_colors("bt601", &c);
}

static void test_quad(void) {
    /* Y 4x2 with distinct grays, chroma 2x1 neutral; drawn 2x scaled at (1,1) in a 10x6 target. */
    uint8_t y[2 * 8] = {16, 72, 128, 235, 0, 0, 0, 0, 235, 128, 72, 16, 0, 0, 0, 0};   /* pitch 8 */
    uint8_t cr[2] = {128, 128}, cb[2] = {128, 128};
    YuvPlane pl[3] = {{y, 4, 2, 8, -0.5f, -0.5f, 3.5f, 1.5f}, {cr, 2, 1, 2, -0.5f, -0.5f, 1.5f, 0.5f}, {cb, 2, 1, 2, -0.5f, -0.5f, 1.5f, 0.5f}};
    YuvCoeffs c;
    yuv_coeffs_bt601(&c);
    uint32_t dst[10 * 6];
    for (int i = 0; i < 60; i++) dst[i] = 0x00ABCDEF;
    yuv_draw_quad(dst, 40, 10, 6, 1, 1, 9, 5, pl, &c);
    CHECK(dst[0] == 0x00ABCDEF && dst[9] == 0x00ABCDEF && dst[5 * 10 + 4] == 0x00ABCDEF, "pixels outside the quad untouched");
    CHECK(dst[1 * 10 + 1] == 0 && dst[1 * 10 + 2] == 0, "top-left 2x2 block = texel (0,0) black (%06x %06x)", dst[11], dst[12]);
    CHECK(R(dst[1 * 10 + 7]) >= 250 && dst[1 * 10 + 7] == dst[2 * 10 + 8], "top-right block = texel (3,0) white (%06x)", dst[17]);
    CHECK(dst[3 * 10 + 8] == 0 && R(dst[4 * 10 + 1]) >= 250, "bottom row mirrored (%06x %06x)", dst[38], dst[41]);
    uint32_t g = dst[1 * 10 + 5];
    CHECK(R(g) > 120 && R(g) < 135, "texel (2,0) Y=128 -> mid gray (%06x)", g);
    /* Clipping: a quad hanging off the target writes only inside it. */
    for (int i = 0; i < 60; i++) dst[i] = 0x00ABCDEF;
    yuv_draw_quad(dst, 40, 10, 6, -4, -2, 4, 2, pl, &c);
    CHECK(dst[0] != 0x00ABCDEF && dst[4] == 0x00ABCDEF && dst[2 * 10] == 0x00ABCDEF, "clipped quad stays inside the target");
}

int main(void) {
    test_swizzle();
    test_coeffs();
    test_quad();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL OK\n");
    return 0;
}
