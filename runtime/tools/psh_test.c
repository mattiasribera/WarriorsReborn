/* Game-independent test of src/psh.c: D3DPIXELSHADERDEF field offsets, combiner constant
 * resolution (definition constants, SAME/UNIQUE C0/C1, PSC0/PSC1 mappings to
 * SetPixelShaderConstant values, final combiner constants) and float -> ARGB conversion. The
 * definitions are synthetic. */
#include "../src/psh.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* Dword indices of the definition fields (byte offset / 4). */
enum { ALPHA_IN = 0, FINAL_ABCD = 8, FINAL_EFG = 9, C0 = 10, C1 = 18, ALPHA_OUT = 26, RGB_IN = 34, COMPARE = 42,
       FINAL_C0 = 43, FINAL_C1 = 44, RGB_OUT = 45, COUNT = 53, TEX_MODES = 54, DOT_MAP = 55, INPUT_TEX = 56, C0_MAP = 57,
       C1_MAP = 58, FINAL_CONSTS = 59 };

int main(void) {
    float c[4] = {1, 0.5f, -2, 0.7f};
    CHECK(psh_argb(c) == 0xB3FF8000u, "float constant -> ARGB, clamped (%08x)", psh_argb(c));

    uint32_t def[PSH_DEF_DWORDS], d3d[PSH_CONSTANTS];
    memset(def, 0, sizeof def);
    for (int i = 0; i < PSH_CONSTANTS; i++) d3d[i] = 0x01010101u * (uint32_t)(i + 1);
    for (int i = 0; i < 8; i++) {
        def[C0 + i] = 0xC0000000u | (uint32_t)i;
        def[C1 + i] = 0xC1000000u | (uint32_t)i;
        def[RGB_IN + i] = 0x100 + i;
        def[ALPHA_IN + i] = 0x200 + i;
        def[RGB_OUT + i] = 0x300 + i;
        def[ALPHA_OUT + i] = 0x400 + i;
    }
    def[FINAL_ABCD] = 0x130c0300u;
    def[FINAL_EFG] = 0x00001c80u;
    def[FINAL_C0] = 0xF0F0F0F0u;
    def[FINAL_C1] = 0xF1F1F1F1u;
    def[TEX_MODES] = 1 | 2 << 5 | 4 << 10 | 5 << 15;
    def[COUNT] = 0x00000104u;   /* 4 stages, MUX_MSB, SAME_C0, SAME_C1 */
    def[C0_MAP] = 0xFFFFFFFFu;
    def[C1_MAP] = 0xFFFFFFFFu;
    def[FINAL_CONSTS] = 0x1FFu;

    RPixelShader ps;
    psh_from_def(def, d3d, 0xFF102030u, &ps);
    CHECK(ps.combiners == 0x104u, "combiner count and MUX_MSB kept (%x)", ps.combiners);
    CHECK(ps.rgb_in[3] == 0x103 && ps.alpha_in[7] == 0x207 && ps.rgb_out[0] == 0x300 && ps.alpha_out[5] == 0x405,
          "stage inputs/outputs read from their offsets");
    CHECK(ps.final_abcd == 0x130c0300u && ps.final_efg == 0x00001c80u, "final combiner inputs");
    CHECK(ps.c0[0] == 0xC0000000u && ps.c0[3] == 0xC0000000u && ps.c1[2] == 0xC1000000u,
          "SAME_C0/C1: every stage uses stage 0's definition constant (%08x %08x)", ps.c0[3], ps.c1[2]);
    CHECK(ps.final_c0 == 0xF0F0F0F0u && ps.final_c1 == 0xF1F1F1F1u, "unmapped final constants come from the definition");
    CHECK(psh_tex_mode(&ps, 0) == 1 && psh_tex_mode(&ps, 1) == 2 && psh_tex_mode(&ps, 2) == 4 && psh_tex_mode(&ps, 3) == 5,
          "texture modes per stage");
    CHECK(ps.fog == 0xFF102030u, "fog register");

    /* The title's lit/shadowed shader shape: UNIQUE_C0/C1, stage 1 mapped to D3D constants 0/1. */
    def[COUNT] = 0x00011104u;
    def[C0_MAP] = 0xFFFFFF0Fu;
    def[C1_MAP] = 0xFFFFFF1Fu;
    def[FINAL_CONSTS] = 0x132u;   /* final C0 <- D3D 2, final C1 <- D3D 3 */
    psh_from_def(def, d3d, 0, &ps);
    CHECK(ps.c0[1] == d3d[0] && ps.c1[1] == d3d[1], "PSC0/PSC1 mapping: stage 1 takes D3D constants 0 and 1 (%08x %08x)", ps.c0[1],
          ps.c1[1]);
    CHECK(ps.c0[0] == 0xC0000000u && ps.c0[2] == 0xC0000002u && ps.c1[3] == 0xC1000003u,
          "UNIQUE: unmapped stages keep their own definition constants (%08x %08x %08x)", ps.c0[0], ps.c0[2], ps.c1[3]);
    CHECK(ps.final_c0 == d3d[2] && ps.final_c1 == d3d[3], "final constants mapped to D3D constants (%08x %08x)", ps.final_c0, ps.final_c1);
    CHECK(ps.combiners == 0x104u, "flags beyond MUX_MSB are not passed to the shader (%x)", ps.combiners);

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
