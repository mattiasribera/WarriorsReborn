/* D3DPIXELSHADERDEF conversion (see psh.h). Constant resolution as the Xbox D3D library loads
 * the NV2A combiner factors (Cxbx-Reloaded XbPixelShader.cpp, xemu pgraph psh state). */
#include "psh.h"
#include <string.h>

enum {
    D_ALPHA_IN = 0x00 / 4, D_FINAL_ABCD = 0x20 / 4, D_FINAL_EFG = 0x24 / 4, D_C0 = 0x28 / 4, D_C1 = 0x48 / 4,
    D_ALPHA_OUT = 0x68 / 4, D_RGB_IN = 0x88 / 4, D_FINAL_C0 = 0xAC / 4, D_FINAL_C1 = 0xB0 / 4, D_RGB_OUT = 0xB4 / 4,
    D_COUNT = 0xD4 / 4, D_TEX_MODES = 0xD8 / 4, D_C0_MAP = 0xE4 / 4, D_C1_MAP = 0xE8 / 4, D_FINAL_CONSTS = 0xEC / 4,
};
#define UNIQUE_C0 0x1000u
#define UNIQUE_C1 0x10000u

uint32_t psh_argb(const float c[4]) {
    uint32_t out = 0;
    for (int k = 0; k < 4; k++) {
        float v = c[k];
        uint32_t b = v > 0 ? (v < 1 ? (uint32_t)(v * 255.0f + 0.5f) : 255u) : 0u;   /* NaN -> 0 */
        out |= b << (k == 3 ? 24 : 16 - 8 * k);
    }
    return out;
}

/* Constant of a combiner slot: mapped D3D constant, or the definition's own. */
static uint32_t slot(uint32_t mapping_nibble, uint32_t own, const uint32_t d3d[PSH_CONSTANTS]) {
    return mapping_nibble != 0xF ? d3d[mapping_nibble] : own;
}

void psh_from_def(const uint32_t d[PSH_DEF_DWORDS], const uint32_t d3d[PSH_CONSTANTS], uint32_t fog, RPixelShader *ps) {
    memset(ps, 0, sizeof *ps);
    uint32_t count = d[D_COUNT];
    ps->combiners = count & 0x1FFu;
    for (int i = 0; i < 8; i++) {
        ps->rgb_in[i] = d[D_RGB_IN + i];
        ps->alpha_in[i] = d[D_ALPHA_IN + i];
        ps->rgb_out[i] = d[D_RGB_OUT + i];
        ps->alpha_out[i] = d[D_ALPHA_OUT + i];
    }
    uint32_t c0[8], c1[8];
    for (int i = 0; i < 8; i++) {
        c0[i] = slot((d[D_C0_MAP] >> (4 * i)) & 0xF, d[D_C0 + i], d3d);
        c1[i] = slot((d[D_C1_MAP] >> (4 * i)) & 0xF, d[D_C1 + i], d3d);
    }
    for (int i = 0; i < 8; i++) {
        ps->c0[i] = c0[count & UNIQUE_C0 ? i : 0];
        ps->c1[i] = c1[count & UNIQUE_C1 ? i : 0];
    }
    ps->final_abcd = d[D_FINAL_ABCD];
    ps->final_efg = d[D_FINAL_EFG];
    ps->final_c0 = slot(d[D_FINAL_CONSTS] & 0xF, d[D_FINAL_C0], d3d);
    ps->final_c1 = slot((d[D_FINAL_CONSTS] >> 4) & 0xF, d[D_FINAL_C1], d3d);
    ps->tex_modes = d[D_TEX_MODES];
    ps->fog = fog;
}

uint32_t psh_tex_mode(const RPixelShader *ps, int stage) { return (ps->tex_modes >> (5 * stage)) & 31u; }
