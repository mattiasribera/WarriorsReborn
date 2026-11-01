/* CPU YUV quad helpers (see yuv.h). */
#include "yuv.h"
#include <math.h>
#include <stdlib.h>

/* D3DPIXELSHADERDEF (Xbox) field offsets in dwords. */
enum { PS_FINAL_ABCD = 8, PS_FINAL_EFG = 9, PS_C0 = 10, PS_C1 = 18, PS_RGB_IN = 34, PS_RGB_OUT = 45,
       PS_COMBINER_COUNT = 53, PS_TEX_MODES = 54 };

/* The Bink YUV program's color combiner words (inputs A:B:C:D bytes, outputs
 * flags<<12 | sum<<8 | ab<<4 | cd), stage by stage:
 *   0: t0.rgb = t0 * c0(1,0,0) + t1 * c1(0,1,0)        -> (Y, plane1, 0)
 *   1: t0.rgb = t2 * c0(0,0,1) + t0 * (1 - c0)               -> (Y, plane1, plane2)
 *   2: t0.rgb = t0 - c0                                      -> subtract the offsets
 *   3: r0 = 4 * dot(t0, c0), t2 = 4 * dot(t0, c1)            -> R and B
 *   4: t1 = 2 * dot(t0, expand(c0))                          -> G (expand(x) = 2x - 1)
 *   5: r0.rgb = r0 * c0(1,0,0) + t1 * c1(0,1,0)              -> (R, G, 0)
 *   6: r0.rgb = t2 * c0(0,0,1) + r0 * (1 - c0)               -> (R, G, B)
 * and the final combiner outputs r0 * v0 (v0 = vertex diffuse). Constants vary per stage. */
static const uint32_t bink_rgb_in[7] = {0xc8c1c9c2, 0xcac1c821, 0xc820e120, 0xc8c1c8c2, 0xc841c0c0, 0xccc1c9c2, 0xcac1cc21};
static const uint32_t bink_rgb_out[7] = {0x00000800, 0x00000800, 0x00000800, 0x000e30ca, 0x00092090, 0x00000c00, 0x00000c00};
#define COMBINERCOUNT_UNIQUE_C0_C1 0x00011000u
#define TEX_MODES_PROJECT2D_0_1_2 0x00000421u   /* stages 0-2 PROJECT2D, stage 3 none */

static float chan(uint32_t c, int i) { return (float)((c >> (16 - 8 * i)) & 0xFF) / 255.f; }   /* i: 0=R 1=G 2=B */

int yuv_coeffs_from_psdef(const uint32_t *d, YuvCoeffs *c) {
    if ((d[PS_COMBINER_COUNT] & 0xFF) != 7 || (d[PS_COMBINER_COUNT] & COMBINERCOUNT_UNIQUE_C0_C1) != COMBINERCOUNT_UNIQUE_C0_C1 ||
        d[PS_TEX_MODES] != TEX_MODES_PROJECT2D_0_1_2 || d[PS_FINAL_ABCD] != 0x00000f00u ||
        (d[PS_FINAL_EFG] & 0xFFFFFF00u) != 0x0c041400u)
        return 0;
    for (int s = 0; s < 7; s++)
        if (d[PS_RGB_IN + s] != bink_rgb_in[s] || d[PS_RGB_OUT + s] != bink_rgb_out[s]) return 0;
    for (int i = 0; i < 3; i++) {
        c->off[i] = chan(d[PS_C0 + 2], i);
        c->m[0][i] = 4.f * chan(d[PS_C0 + 3], i);
        c->m[1][i] = 2.f * (2.f * chan(d[PS_C0 + 4], i) - 1.f);
        c->m[2][i] = 4.f * chan(d[PS_C1 + 3], i);
    }
    return 1;
}

void yuv_coeffs_bt601(YuvCoeffs *c) {
    static const YuvCoeffs bt601 = {{16 / 255.f, 128 / 255.f, 128 / 255.f},
                                    {{1.164f, 1.596f, 0.f}, {1.164f, -0.813f, -0.391f}, {1.164f, 0.f, 2.018f}}};
    *c = bt601;
}

static uint32_t to8(float v) { return v <= 0.f ? 0 : v >= 1.f ? 255 : (uint32_t)(v * 255.f + 0.5f); }

uint32_t yuv_to_xrgb(const YuvCoeffs *c, uint8_t p0, uint8_t p1, uint8_t p2) {
    float a = p0 / 255.f - c->off[0], b = p1 / 255.f - c->off[1], d = p2 / 255.f - c->off[2];
    uint32_t r = to8(c->m[0][0] * a + c->m[0][1] * b + c->m[0][2] * d);
    uint32_t g = to8(c->m[1][0] * a + c->m[1][1] * b + c->m[1][2] * d);
    uint32_t bl = to8(c->m[2][0] * a + c->m[2][1] * b + c->m[2][2] * d);
    return r << 16 | g << 8 | bl;
}

static uint32_t nearest(float t, uint32_t n) {
    float i = floorf(t + 0.5f);
    return i <= 0.f ? 0 : i >= (float)(n - 1) ? n - 1 : (uint32_t)i;
}

void yuv_draw_quad(uint32_t *dst, uint32_t dst_pitch, int dw, int dh, int x0, int y0, int x1, int y1,
                   const YuvPlane pl[3], const YuvCoeffs *c) {
    if (x1 <= x0 || y1 <= y0) return;
    int cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0, cx1 = x1 > dw ? dw : x1, cy1 = y1 > dh ? dh : y1;
    if (cx1 <= cx0 || cy1 <= cy0) return;
    uint32_t *col = malloc(sizeof *col * 3 * (size_t)(cx1 - cx0));
    if (!col) return;
    for (int p = 0; p < 3; p++)
        for (int x = cx0; x < cx1; x++) {
            float f = (x + 0.5f - x0) / (float)(x1 - x0);
            col[p * (cx1 - cx0) + (x - cx0)] = nearest(pl[p].u0 + f * (pl[p].u1 - pl[p].u0), pl[p].w);
        }
    for (int y = cy0; y < cy1; y++) {
        float f = (y + 0.5f - y0) / (float)(y1 - y0);
        const uint8_t *row[3];
        for (int p = 0; p < 3; p++) row[p] = pl[p].p + (size_t)nearest(pl[p].v0 + f * (pl[p].v1 - pl[p].v0), pl[p].h) * pl[p].pitch;
        uint32_t *out = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_pitch);
        for (int x = cx0, i = 0; x < cx1; x++, i++)
            out[x] = yuv_to_xrgb(c, row[0][col[i]], row[1][col[(cx1 - cx0) + i]], row[2][col[2 * (cx1 - cx0) + i]]);
    }
    free(col);
}
