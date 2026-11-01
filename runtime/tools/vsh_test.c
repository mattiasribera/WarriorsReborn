/* Game-independent test of src/vsh.c, the CPU interpreter of NV2A vertex program microcode.
 * Instructions are encoded here with the field table of xemu hw/xbox/nv2a/pgraph/vsh.c (dword,
 * bit position, width), independently of the decoder's own tables: header, MAC and ILU opcodes,
 * swizzles, negation, write masks, paired MAC + ILU (ILU writes r1), o-register and constant
 * outputs, relative addressing, r12 = oPos, output defaults and color saturation, and the
 * screen-space tail the D3D library appends to every program. The batch interpreter
 * (vsh_run_batch) must give the per-vertex results bit for bit on random programs. */
#include "../src/vsh.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int near4(const float *a, float x, float y, float z, float w) {
    return fabsf(a[0] - x) < 1e-4f && fabsf(a[1] - y) < 1e-4f && fabsf(a[2] - z) < 1e-4f && fabsf(a[3] - w) < 1e-4f;
}

/* ---- encoder (xemu field table) ---- */
enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
#define XYZW 0x1B   /* swizzle x y z w: 2 bits each, x in the high bits */
#define SW(a, b, c, d) ((a) << 6 | (b) << 4 | (c) << 2 | (d))
typedef struct { int mux, reg, neg, swz; } Src;   /* reg: temp register, or the v/c index */
typedef struct {
    int mac, ilu;
    Src a, b, c;
    int mac_mask, out_r, ilu_mask;
    int o_mask, o_reg, o_addr, o_ilu;   /* output: o_reg 1 = o-register, 0 = constant; o_ilu = from the ILU */
    int a0x, final;
} Ins;
static void put(uint32_t *t, int dw, int pos, int bits, uint32_t v) { t[dw] |= (v & ((1u << bits) - 1)) << pos; }
static void encode(const Ins *i, uint32_t t[4]) {
    memset(t, 0, 16);
    int v = i->a.mux == MUX_V ? i->a.reg : i->b.mux == MUX_V ? i->b.reg : i->c.mux == MUX_V ? i->c.reg : 0;
    int k = i->a.mux == MUX_C ? i->a.reg : i->b.mux == MUX_C ? i->b.reg : i->c.mux == MUX_C ? i->c.reg : 0;
    put(t, 1, 25, 3, i->ilu);
    put(t, 1, 21, 4, i->mac);
    put(t, 1, 13, 8, k + 96);
    put(t, 1, 9, 4, v);
    put(t, 1, 8, 1, i->a.neg);
    put(t, 1, 0, 8, i->a.swz);
    put(t, 2, 28, 4, i->a.mux == MUX_R ? i->a.reg : 0);
    put(t, 2, 26, 2, i->a.mux);
    put(t, 2, 25, 1, i->b.neg);
    put(t, 2, 17, 8, i->b.swz);
    put(t, 2, 13, 4, i->b.mux == MUX_R ? i->b.reg : 0);
    put(t, 2, 11, 2, i->b.mux);
    put(t, 2, 10, 1, i->c.neg);
    put(t, 2, 2, 8, i->c.swz);
    int cr = i->c.mux == MUX_R ? i->c.reg : 0;
    put(t, 2, 0, 2, cr >> 2);
    put(t, 3, 30, 2, cr & 3);
    put(t, 3, 28, 2, i->c.mux);
    put(t, 3, 24, 4, i->mac_mask);
    put(t, 3, 20, 4, i->out_r);
    put(t, 3, 16, 4, i->ilu_mask);
    put(t, 3, 12, 4, i->o_mask);
    put(t, 3, 11, 1, i->o_reg);
    put(t, 3, 3, 8, i->o_reg ? i->o_addr : i->o_addr + 96);
    put(t, 3, 2, 1, i->o_ilu);
    put(t, 3, 1, 1, i->a0x);
    put(t, 3, 0, 1, i->final);
}
static uint32_t prog_words[1 + 4 * 32];
static int nins;
static void begin(void) { nins = 0; memset(prog_words, 0, sizeof prog_words); }
static void add(Ins i) { encode(&i, &prog_words[1 + 4 * nins]); nins++; }
static int finish(VshProgram *p) {
    prog_words[4 * nins] |= 1;   /* FINAL on the last instruction */
    prog_words[0] = 0x2078u | (uint32_t)nins << 16;
    return vsh_load(prog_words, 1 + 4 * nins, p);
}
#define R(n) ((Src){MUX_R, n, 0, XYZW})
#define V(n) ((Src){MUX_V, n, 0, XYZW})
#define C(n) ((Src){MUX_C, n, 0, XYZW})
static Src swz(Src s, int sw) { s.swz = sw; return s; }
static Src neg(Src s) { s.neg = 1; return s; }
#define X 0
#define Y 1
#define Z 2
#define W 3
#define MASK_X 8
#define MASK_Y 4
#define MASK_Z 2
#define MASK_W 1
#define MASK_ALL 15

static float in[16][4], cs[192][4], out[VSH_NOUT][4];
static float *cr(int reg) { return cs[reg + 96]; }
static void set4(float *d, float x, float y, float z, float w) { d[0] = x; d[1] = y; d[2] = z; d[3] = w; }

static void test_header(void) {
    VshProgram p;
    uint32_t bad[5] = {0x00012073u, 0, 0, 0, 1};
    CHECK(vsh_load(bad, 5, &p) != 0, "a state program header (type 0x73) is rejected");
    uint32_t too_short[3] = {0x00022078u, 0, 0};
    CHECK(vsh_load(too_short, 3, &p) != 0, "an instruction count beyond the buffer is rejected");
    begin();
    add((Ins){.mac = MAC_MOV, .a = V(0), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OPOS});
    add((Ins){.mac = MAC_MOV, .a = V(1), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OD0});
    CHECK(finish(&p) == 0 && p.n == 2, "header 0x2078 | 2 << 16 loads two instructions (%u)", p.n);
}

static void test_transform(void) {
    VshProgram p;
    begin();
    for (int r = 0; r < 4; r++)
        add((Ins){.mac = MAC_DP4, .a = V(0), .b = C(69 + r), .o_mask = 8 >> r, .o_reg = 1, .o_addr = VSH_OPOS});
    finish(&p);
    memset(cs, 0, sizeof cs);
    set4(in[0], 1, 2, 3, 1);
    set4(cr(69), 2, 0, 0, 1);
    set4(cr(70), 0, 3, 0, 0);
    set4(cr(71), 0, 0, 1, -1);
    set4(cr(72), 0, 0, 1, 0);
    vsh_run(&p, (const float (*)[4])in, (const float (*)[4])cs, out);
    CHECK(near4(out[VSH_OPOS], 3, 6, 2, 3), "dp4 oPos.x..w with c69..c72 (%g %g %g %g)", out[0][0], out[0][1], out[0][2], out[0][3]);
    CHECK(near4(out[VSH_OD0], 0, 0, 0, 1) && near4(out[VSH_OT0 + 2], 0, 0, 0, 1) && near4(out[VSH_OD1], 0, 0, 0, 1),
          "unwritten outputs default to (0, 0, 0, 1)");
}

static void test_alu(void) {
    VshProgram p;
    memset(cs, 0, sizeof cs);
    set4(in[0], 1, 2, 3, 4);
    set4(in[1], 0.5f, -1, 2, 3);
    set4(cr(0), 2, 3, 4, 5);
    set4(cr(1), 10, 20, 30, 40);
    begin();
    /* One v and one c register per instruction (single operand fields): v1 is copied to r1 first. */
    add((Ins){.mac = MAC_MOV, .a = V(1), .mac_mask = MASK_ALL, .out_r = 1});
    /* add r0, v0.wzyx, -c0 */
    add((Ins){.mac = MAC_ADD, .a = swz(V(0), SW(W, Z, Y, X)), .c = neg(C(0)), .mac_mask = MASK_ALL, .out_r = 0});
    add((Ins){.mac = MAC_MOV, .a = R(0), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0});
    /* mul r2.xz, v0, c0.yyyy  (y and w keep their previous value 0) */
    add((Ins){.mac = MAC_MUL, .a = V(0), .b = swz(C(0), SW(Y, Y, Y, Y)), .mac_mask = MASK_X | MASK_Z, .out_r = 2});
    add((Ins){.mac = MAC_MOV, .a = R(2), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0 + 1});
    /* mad oT2, v0, r1, c1 */
    add((Ins){.mac = MAC_MAD, .a = V(0), .b = R(1), .c = C(1), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0 + 2});
    /* dp3 oT3.x, v0, r1; dph oT3.y, v0, c0; min oT3.z, v0.x, r1.x; max oT3.w, v0.x, r1.y */
    add((Ins){.mac = MAC_DP3, .a = V(0), .b = R(1), .o_mask = MASK_X, .o_reg = 1, .o_addr = VSH_OT0 + 3});
    add((Ins){.mac = MAC_DPH, .a = V(0), .b = C(0), .o_mask = MASK_Y, .o_reg = 1, .o_addr = VSH_OT0 + 3});
    add((Ins){.mac = MAC_MIN, .a = swz(V(0), 0), .b = swz(R(1), 0), .o_mask = MASK_Z, .o_reg = 1, .o_addr = VSH_OT0 + 3});
    add((Ins){.mac = MAC_MAX, .a = swz(V(0), 0), .b = swz(R(1), SW(Y, Y, Y, Y)), .o_mask = MASK_W, .o_reg = 1, .o_addr = VSH_OT0 + 3});
    /* slt oB0, r1, c0.xxxx-ish; sge oB1, r1, v0 (written to the back colors, which saturate like oD0) */
    add((Ins){.mac = MAC_SLT, .a = R(1), .b = swz(C(0), 0), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OB0});
    add((Ins){.mac = MAC_SGE, .a = R(1), .b = V(0), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OB1});
    /* dst oFog, v0, r1 = (1, v0.y * r1.y, v0.z, r1.w) */
    add((Ins){.mac = MAC_DST, .a = V(0), .b = R(1), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OFOG});
    CHECK(finish(&p) == 0, "ALU program loads");
    vsh_run(&p, (const float (*)[4])in, (const float (*)[4])cs, out);
    CHECK(near4(out[VSH_OT0], 2, 0, -2, -4), "add with swizzle wzyx and negation (%g %g %g %g)", out[VSH_OT0][0], out[VSH_OT0][1],
          out[VSH_OT0][2], out[VSH_OT0][3]);
    CHECK(near4(out[VSH_OT0 + 1], 3, 0, 9, 0), "write mask xz keeps y and w (%g %g %g %g)", out[VSH_OT0 + 1][0], out[VSH_OT0 + 1][1],
          out[VSH_OT0 + 1][2], out[VSH_OT0 + 1][3]);
    CHECK(near4(out[VSH_OT0 + 2], 10.5f, 18, 36, 52), "mad");
    CHECK(near4(out[VSH_OT0 + 3], 4.5f, 25, 0.5f, 1), "dp3, dph, min, max (%g %g %g %g)", out[VSH_OT0 + 3][0], out[VSH_OT0 + 3][1],
          out[VSH_OT0 + 3][2], out[VSH_OT0 + 3][3]);
    CHECK(near4(out[VSH_OB0], 1, 1, 0, 0) && near4(out[VSH_OB1], 0, 0, 0, 0), "slt and sge (%g %g %g %g / %g %g %g %g)", out[VSH_OB0][0],
          out[VSH_OB0][1], out[VSH_OB0][2], out[VSH_OB0][3], out[VSH_OB1][0], out[VSH_OB1][1], out[VSH_OB1][2], out[VSH_OB1][3]);
    CHECK(near4(out[VSH_OFOG], 1, -2, 3, 3), "dst");
}

static void test_ilu(void) {
    VshProgram p;
    memset(cs, 0, sizeof cs);
    set4(in[0], 4, 0.25f, 0, -2);
    set4(in[1], 0.5f, 0.8f, 0, 2);   /* lit: n.l, n.h, -, power */
    set4(in[2], 3.5f, 0, 0, 0);
    begin();
    /* Paired: mov r5, v0 (MAC) with rcp r1.x, v0.x (ILU); the ILU of a paired instruction writes r1. */
    add((Ins){.mac = MAC_MOV, .ilu = ILU_RCP, .a = V(0), .c = swz(V(0), 0), .mac_mask = MASK_ALL, .out_r = 5, .ilu_mask = MASK_X});
    add((Ins){.mac = MAC_MOV, .a = R(1), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0});
    add((Ins){.mac = MAC_MOV, .a = R(5), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0 + 1});
    /* Unpaired ILU writes out_r: rsq r3.y, v0.y; rcc r3.z, v0.z (1/0 clamped); output from the ILU: mov oT2, -v0 */
    add((Ins){.ilu = ILU_RSQ, .c = swz(V(0), SW(Y, Y, Y, Y)), .ilu_mask = MASK_Y, .out_r = 3});
    add((Ins){.ilu = ILU_RCC, .c = swz(V(0), SW(Z, Z, Z, Z)), .ilu_mask = MASK_Z, .out_r = 3});
    add((Ins){.ilu = ILU_RSQ, .c = swz(V(0), SW(W, W, W, W)), .ilu_mask = MASK_W, .out_r = 3});
    add((Ins){.mac = MAC_MOV, .a = R(3), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0 + 3});
    add((Ins){.ilu = ILU_MOV, .c = neg(V(0)), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0 + 2, .o_ilu = 1});
    add((Ins){.ilu = ILU_LIT, .c = V(1), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OFOG, .o_ilu = 1});
    add((Ins){.ilu = ILU_EXP, .c = swz(V(2), 0), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OPTS, .o_ilu = 1});
    add((Ins){.ilu = ILU_LOG, .c = swz(V(2), 0), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OB0, .o_ilu = 1});
    CHECK(finish(&p) == 0, "ILU program loads");
    vsh_run(&p, (const float (*)[4])in, (const float (*)[4])cs, out);
    CHECK(near4(out[VSH_OT0], 0.25f, 0, 0, 0), "paired ILU rcp writes r1 (%g %g %g %g)", out[VSH_OT0][0], out[VSH_OT0][1], out[VSH_OT0][2],
          out[VSH_OT0][3]);
    CHECK(near4(out[VSH_OT0 + 1], 4, 0.25f, 0, -2), "paired MAC writes its own register");
    CHECK(out[VSH_OT0 + 3][1] == 2.0f && out[VSH_OT0 + 3][2] > 1e18f && out[VSH_OT0 + 3][2] < 2e19f &&
              fabsf(out[VSH_OT0 + 3][3] - 0.70710678f) < 1e-5f,
          "rsq 0.25 = 2, rcc 0 clamps to 1.84e19, rsq uses |x| (%g %g %g)", out[VSH_OT0 + 3][1], out[VSH_OT0 + 3][2], out[VSH_OT0 + 3][3]);
    CHECK(near4(out[VSH_OT0 + 2], -4, -0.25f, 0, 2), "ILU result routed to an o-register");
    CHECK(near4(out[VSH_OFOG], 1, 0.5f, 0.64f, 1), "lit (%g %g %g %g)", out[VSH_OFOG][0], out[VSH_OFOG][1], out[VSH_OFOG][2], out[VSH_OFOG][3]);
    CHECK(near4(out[VSH_OPTS], 8, 0.5f, 11.3137085f, 1), "exp: 2^floor, fraction, 2^x (%g %g %g %g)", out[VSH_OPTS][0], out[VSH_OPTS][1],
          out[VSH_OPTS][2], out[VSH_OPTS][3]);
    /* oB0 saturates like a color: log 3.5 = (1, 1.75, 1.807, 1) -> (1, 1, 1, 1) */
    CHECK(near4(out[VSH_OB0], 1, 1, 1, 1), "back color output saturates (log 3.5)");
}

static void test_addressing(void) {
    VshProgram p;
    memset(cs, 0, sizeof cs);
    set4(in[4], 40.0f, 0, 0, 0);
    set4(in[0], 1, 1, 1, 1);
    set4(cr(4), 1, 2, 3, 4);     /* c[a0.x - 36] with a0.x = 40 */
    set4(cr(5), 5, 6, 7, 8);
    begin();
    add((Ins){.mac = MAC_ARL, .a = swz(V(4), 0)});
    add((Ins){.mac = MAC_MOV, .a = C(-36), .a0x = 1, .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0});
    add((Ins){.mac = MAC_MOV, .a = C(-35), .a0x = 1, .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0 + 1});
    /* mov oPos, v0; then r12 reads oPos: add oT2, r12, r12 */
    add((Ins){.mac = MAC_MOV, .a = V(0), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OPOS});
    add((Ins){.mac = MAC_ADD, .a = R(12), .c = R(12), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0 + 2});
    /* A constant write (output to the constant file) is ignored by this interpreter. */
    add((Ins){.mac = MAC_MOV, .a = V(0), .o_mask = MASK_ALL, .o_reg = 0, .o_addr = 4});
    add((Ins){.mac = MAC_MOV, .a = C(4), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OT0 + 3});
    /* oD0 saturates: mov oD0, c5 = (5, 6, 7, 8) -> (1, 1, 1, 1); oD1 = -c5 -> 0 */
    add((Ins){.mac = MAC_MOV, .a = C(5), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OD0});
    add((Ins){.mac = MAC_MOV, .a = neg(C(5)), .o_mask = MASK_ALL, .o_reg = 1, .o_addr = VSH_OD1});
    CHECK(finish(&p) == 0, "addressing program loads");
    vsh_run(&p, (const float (*)[4])in, (const float (*)[4])cs, out);
    CHECK(near4(out[VSH_OT0], 1, 2, 3, 4) && near4(out[VSH_OT0 + 1], 5, 6, 7, 8), "arl + c[a0.x - 36] and c[a0.x - 35]");
    CHECK(near4(out[VSH_OT0 + 2], 2, 2, 2, 2), "r12 reads oPos");
    CHECK(near4(out[VSH_OT0 + 3], 1, 2, 3, 4), "constant writes do not change the constants");
    CHECK(near4(out[VSH_OD0], 1, 1, 1, 1) && near4(out[VSH_OD1], 0, 0, 0, 0), "oD0/oD1 saturate to [0, 1]");
    /* Out-of-range relative index reads zero. */
    set4(in[4], 500.0f, 0, 0, 0);
    vsh_run(&p, (const float (*)[4])in, (const float (*)[4])cs, out);
    CHECK(near4(out[VSH_OT0], 0, 0, 0, 0), "relative index beyond c95 reads 0");
}

/* The tail the D3D library appends (observed in every title program):
 * mul oPos.xyz, r12, c[-38]; rcc r1.x, r12.w; mad oPos.xyz, r12, r1.x, c[-37]. */
static void test_screen_tail(void) {
    VshProgram p;
    memset(cs, 0, sizeof cs);
    set4(in[0], 2, -4, 6, 1);
    set4(cr(69), 1, 0, 0, 0);
    set4(cr(70), 0, 1, 0, 0);
    set4(cr(71), 0, 0, 1, 0);
    set4(cr(72), 0, 0, 0.5f, 0);   /* w = 3 */
    begin();
    for (int r = 0; r < 4; r++)
        add((Ins){.mac = MAC_DP4, .a = V(0), .b = C(69 + r), .o_mask = 8 >> r, .o_reg = 1, .o_addr = VSH_OPOS});
    add((Ins){.mac = MAC_MUL, .a = R(12), .b = C(-38), .o_mask = MASK_X | MASK_Y | MASK_Z, .o_reg = 1, .o_addr = VSH_OPOS});
    add((Ins){.ilu = ILU_RCC, .c = swz(R(12), SW(W, W, W, W)), .ilu_mask = MASK_X, .out_r = 11});
    add((Ins){.mac = MAC_MAD, .a = R(12), .b = swz(R(11), 0), .c = C(-37), .o_mask = MASK_X | MASK_Y | MASK_Z, .o_reg = 1,
              .o_addr = VSH_OPOS});
    finish(&p);
    vsh_set_screen_constants(cs);
    vsh_run(&p, (const float (*)[4])in, (const float (*)[4])cs, out);
    float clip[4];
    vsh_clip(out[VSH_OPOS], clip);
    CHECK(near4(clip, 2, -4, 6, 3), "identity screen constants: clip space recovered (%g %g %g %g)", clip[0], clip[1], clip[2], clip[3]);
}

/* Random microcode (any field value, relative addressing, undefined opcodes) over random inputs:
 * vsh_run_batch must match vsh_run per vertex exactly (NaNs of either sign count as equal). */
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float rndf(void) { return ((int)(rnd() % 2001) - 1000) / 250.0f; }
static int same(float a, float b) { return (isnan(a) && isnan(b)) || !memcmp(&a, &b, 4); }
static void test_batch(void) {
    enum { NV = VSH_BATCH + 5, PROGS = 300 };
    static float vin[NV][16][4], ref[NV][VSH_NOUT][4], bin[16][4][VSH_BATCH], bout[VSH_NOUT][4][VSH_BATCH];
    static float cst[192][4];
    int bad = 0, ops = 0;
    for (int t = 0; t < PROGS; t++) {
        uint32_t words[1 + 4 * 24] = {0};
        int n = 1 + rnd() % 24;
        words[0] = 0x2078u | (uint32_t)n << 16;
        for (int i = 0; i < n; i++)
            for (int d = 1; d < 4; d++) words[1 + 4 * i + d] = rnd() & (d == 3 ? ~1u : ~0u);
        if (t % 3 == 0) {   /* an ARL somewhere, so later relative constant reads vary per vertex */
            uint32_t *w1 = &words[1 + 4 * (rnd() % n) + 1];
            *w1 = (*w1 & ~(15u << 21)) | 13u << 21;
        }
        VshProgram p;
        if (vsh_load(words, 1 + 4 * n, &p)) { bad++; continue; }
        ops += p.n;
        for (int i = 0; i < 192; i++) for (int k = 0; k < 4; k++) cst[i][k] = rndf();
        for (int j = 0; j < NV; j++) for (int r = 0; r < 16; r++) for (int k = 0; k < 4; k++) vin[j][r][k] = rndf();
        for (int j = 0; j < NV; j++) vsh_run(&p, (const float (*)[4])vin[j], (const float (*)[4])cst, ref[j]);
        for (int j0 = 0; j0 < NV; j0 += VSH_BATCH) {
            int cnt = NV - j0 < VSH_BATCH ? NV - j0 : VSH_BATCH;
            for (int j = 0; j < cnt; j++) for (int r = 0; r < 16; r++) for (int k = 0; k < 4; k++) bin[r][k][j] = vin[j0 + j][r][k];
            vsh_run_batch(&p, cnt, (const float (*)[4][VSH_BATCH])bin, (const float (*)[4])cst, bout);
            for (int j = 0; j < cnt; j++)
                for (int r = 0; r < VSH_NOUT; r++)
                    for (int k = 0; k < 4; k++)
                        if (!same(bout[r][k][j], ref[j0 + j][r][k])) {
                            if (bad++ < 3) printf("     program %d vertex %d o%d.%d: batch %g, per vertex %g\n", t, j0 + j, r, k, bout[r][k][j], ref[j0 + j][r][k]);
                        }
        }
    }
    CHECK(!bad, "batch interpreter matches per-vertex results on %d random programs (%d instructions, %d vertices each): %d mismatches",
          PROGS, ops, NV, bad);
}

int main(void) {
    test_header();
    test_transform();
    test_alu();
    test_ilu();
    test_addressing();
    test_screen_tail();
    test_batch();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
