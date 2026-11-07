/* Game-independent test of --world-lighting (src/world_light.c): the option parser, the
 * vertex-program classifier (programs encoded here with the xemu field table, as tools/vsh_test.c
 * does, in the shapes of the title's world, object, character and shadow-caster programs), the
 * view normals and bounding box of a recorded draw, and the per-draw light mask. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/geom.h"
#include "../src/world_light.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- encoder (xemu hw/xbox/nv2a/pgraph/vsh.c field table; constants in D3D numbering) ---- */
enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ };
enum { O_POS = 0, O_D0 = 3, O_D1 = 4, O_FOG = 5, O_T0 = 9, O_T1 = 10 };
#define XYZW 0x1B
typedef struct { int mux, reg, neg, swz; } Src;
typedef struct {
    int mac, ilu;
    Src a, b, c;
    int mac_mask, out_r, ilu_mask;
    int o_mask, o_reg, o_addr, o_ilu;
    int a0x;
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
}
static uint32_t words[1 + 4 * 64];
static int nins;
static void begin(void) { nins = 0; memset(words, 0, sizeof words); }
static void add(Ins i) { encode(&i, &words[1 + 4 * nins]); nins++; }
static int finish(VshProgram *p) {
    words[4 * nins] |= 1;   /* FINAL */
    words[0] = 0x2078u | (uint32_t)nins << 16;
    return vsh_load(words, 1 + 4 * nins, p);
}
#define R(n) ((Src){MUX_R, n, 0, XYZW})
#define V(n) ((Src){MUX_V, n, 0, XYZW})
#define C(n) ((Src){MUX_C, n, 0, XYZW})
#define NEG(s) ((Src){(s).mux, (s).reg, 1, (s).swz})
/* MAC op into a temporary (mask: 8 x, 4 y, 2 z, 1 w). */
static void tmp(int mac, int r, int mask, Src a, Src b, Src c) { add((Ins){.mac = mac, .a = a, .b = b, .c = c, .mac_mask = mask, .out_r = r}); }
/* MAC op into an output register. */
static void out(int mac, int o, int mask, Src a, Src b, Src c) {
    add((Ins){.mac = mac, .a = a, .b = b, .c = c, .o_mask = mask, .o_reg = 1, .o_addr = o, .out_r = 7});
}
static const Src Z = {0, 0, 0, XYZW};

/* vsWorldTransform: oPos = dp4 v0 with c69-c72, oFog = -r12.w. */
static void world_transform(void) {
    for (int k = 0; k < 4; k++) out(MAC_DP4, O_POS, 8 >> k, V(0), C(69 + k), Z);
    out(MAC_MOV, O_FOG, 8, NEG(R(12)), Z, Z);
}
/* vsWorldLight0: r7 = max(-v1 . c75, 0) x c78. */
static void world_light0(void) {
    tmp(MAC_DP3, 1, 8, NEG(V(1)), C(75), Z);
    tmp(MAC_MAX, 1, 8, R(1), C(95), Z);
    tmp(MAC_MUL, 7, 15, R(1), C(78), Z);
}
/* vsWorldFinal: oD0 = r7 x c93 + c94 + v2, oD0.w, oT0, oT1 (texture projection), oD1.w. */
static void world_final(int fold, int v2_first) {
    if (fold) tmp(MAC_MAD, 7, 15, R(7), C(93), C(94));   /* the assembler may fold mul + add */
    else {
        tmp(MAC_MUL, 7, 15, R(7), C(93), Z);
        tmp(MAC_ADD, 7, 15, R(7), Z, C(94));
    }
    if (v2_first) out(MAC_ADD, O_D0, 14, V(2), Z, R(7));
    else out(MAC_ADD, O_D0, 14, R(7), Z, V(2));
    out(MAC_MUL, O_D0, 1, V(2), C(93), Z);
    out(MAC_MOV, O_T0, 15, V(3), Z, Z);
    for (int k = 0; k < 3; k++) out(MAC_DP4, O_T1, 8 >> k, V(0), C(65 + k), Z);
    tmp(MAC_DP3, 1, 8, V(1), C(90), Z);
    out(MAC_SGE, O_D1, 1, R(1), C(95), Z);
}

static void test_options(void) {
    WorldLightConfig c = WORLD_LIGHT_DEFAULTS;
    CHECK(!c.on && c.strength == WORLD_LIGHT_STRENGTH_DEFAULT, "off by default, strength 1");
    CHECK(world_light_arg(&c, "--lamp-lights", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(world_light_arg(&c, "--world-lighting", NULL) == 1 && c.on, "--world-lighting turns it on");
    CHECK(world_light_arg(&c, "--world-lighting-strength", "1.5") == 2 && c.strength == 1.5f, "strength 1.5");
    CHECK(world_light_arg(&c, "--world-lighting-strength", "9") == -1 && c.strength == 1.5f, "out of range: unchanged");
    CHECK(world_light_arg(&c, "--world-lighting-strength", NULL) == -1, "missing value");
    CHECK(world_light_arg(&c, "--world-lighting-strength", "x") == -1, "not a number");
    CHECK(world_light_arg(&c, "--no-world-lighting", NULL) == 1 && !c.on, "--no-world-lighting turns it off");
}

static void test_classifier(void) {
    VshProgram p;
    begin();
    world_transform();
    world_light0();
    world_final(0, 0);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_WORLD, "vsWorldShadowL0: world");

    begin();
    world_transform();
    world_light0();
    world_final(1, 1);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_WORLD, "folded mad, v2 first: world");

    begin();   /* vsWorldTransform + vsWorldFinal without a light block (r7 from the ambient only) */
    world_transform();
    tmp(MAC_MOV, 7, 15, C(94), Z, Z);
    world_final(0, 0);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_WORLD, "no light block: world");

    begin();   /* vsObjectShadow: the normal through c62-c64; v2 added into r7, then oD0 = r7 + c94 */
    world_transform();
    for (int k = 0; k < 3; k++) tmp(MAC_DP3, 6, 8 >> k, V(1), C(62 + k), Z);
    tmp(MAC_DP3, 1, 8, NEG(R(6)), C(75), Z);
    tmp(MAC_MAX, 1, 8, R(1), C(95), Z);
    tmp(MAC_MUL, 7, 15, R(1), C(78), Z);
    tmp(MAC_MUL, 7, 15, R(7), C(93), Z);
    tmp(MAC_ADD, 7, 15, R(7), Z, V(2));
    out(MAC_ADD, O_D0, 15, R(7), Z, C(94));
    out(MAC_MOV, O_T0, 15, V(3), Z, Z);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_OBJECT, "vsObjectShadow: object");

    begin();   /* vsCharacter: bone palette (arl, relative reads), dph oPos from the skinned r5 */
    add((Ins){.mac = MAC_ARL, .a = V(4)});
    add((Ins){.mac = MAC_MUL, .a = V(5), .b = C(-36), .mac_mask = 15, .out_r = 2, .a0x = 1});
    tmp(MAC_DP4, 5, 8, V(0), R(2), Z);
    for (int k = 0; k < 4; k++) out(MAC_DPH, O_POS, 8 >> k, R(5), C(69 + k), Z);
    tmp(MAC_MUL, 7, 15, R(7), C(93), Z);
    out(MAC_ADD, O_D0, 15, R(7), Z, C(94));
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_NONE, "vsCharacter: none");

    begin();   /* the world program but one relative constant read (a0.x): not lit */
    world_transform();
    world_light0();
    add((Ins){.mac = MAC_MUL, .a = R(7), .b = C(93), .mac_mask = 15, .out_r = 7, .a0x = 1});
    out(MAC_ADD, O_D0, 14, R(7), Z, V(2));
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_NONE, "relative read: none");

    begin();   /* vsObjectShadowCast: oPos only (and a constant colour) */
    world_transform();
    out(MAC_MOV, O_D0, 15, C(94), Z, Z);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_NONE, "shadow caster: none");

    begin();   /* the baked colour only moved into oD0 (no light sum): none */
    world_transform();
    out(MAC_MOV, O_D0, 15, V(2), Z, Z);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_NONE, "mov oD0, v2: none");

    begin();   /* oPos from the texture projection rows c65-c68: none */
    for (int k = 0; k < 4; k++) out(MAC_DP4, O_POS, 8 >> k, V(0), C(65 + k), Z);
    world_light0();
    world_final(0, 0);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_NONE, "other rows: none");

    begin();   /* oPos.w missing */
    for (int k = 0; k < 3; k++) out(MAC_DP4, O_POS, 8 >> k, V(0), C(69 + k), Z);
    world_light0();
    world_final(0, 0);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_NONE, "oPos.w missing: none");

    begin();   /* oPos from a transformed position (r5), not the raw v0 */
    tmp(MAC_MOV, 5, 15, V(0), Z, Z);
    for (int k = 0; k < 4; k++) out(MAC_DP4, O_POS, 8 >> k, R(5), C(69 + k), Z);
    world_light0();
    world_final(0, 0);
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_NONE, "oPos from r5: none");

    begin();   /* v2 into r7, then r7 overwritten before oD0: v2 does not reach oD0 */
    world_transform();
    tmp(MAC_ADD, 7, 15, R(7), Z, V(2));
    tmp(MAC_MOV, 7, 15, C(94), Z, Z);
    out(MAC_ADD, O_D0, 15, R(7), Z, C(93));
    CHECK(!finish(&p) && world_light_program(&p) == WORLD_PROG_NONE, "v2 overwritten: none");
}

/* A camera: view = R p + t (R rotates by a about y), clip x = px view x, y = py view y, w = -view z. */
static void camera(float a, const float t[3], float px, float py, float wvp[4][4], float R[3][3]) {
    const float c = cosf(a), s = sinf(a);
    const float r[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
    memcpy(R, r, sizeof r);
    for (int k = 0; k < 3; k++) {
        wvp[0][k] = px * r[0][k];
        wvp[1][k] = py * r[1][k];
        wvp[2][k] = 0.5f * -r[2][k];
        wvp[3][k] = -r[2][k];
    }
    wvp[0][3] = px * t[0];
    wvp[1][3] = py * t[1];
    wvp[2][3] = 0.5f * -t[2];
    wvp[3][3] = -t[2];
}

static void test_record(void) {
    const float t[3] = {1.0f, -2.0f, -10.0f}, px = 1.5f, py = 2.0f;
    float wvp[4][4], R[3][3];
    camera(0.5f, t, px, py, wvp, R);
    /* Three vertices: position (float3), a few bytes of padding, normal (float3); the second normal is
     * scaled, the third zero (both kept as transformed: the shader renormalises). */
    struct { float p[3]; uint32_t pad; float n[3]; } v[3] = {
        {{0, 0, 0}, 0, {0, 1, 0}}, {{2, 1, -1}, 0, {3, 0, 0}}, {{-1, 3, 2}, 0, {0, 0, 0}}};
    WorldLitStore s = {0};
    WorldLitOp op = {0};
    CHECK(world_light_record(&s, &op, (const float (*)[4])wvp, GEOM_FLOAT3, (const uint8_t *)v[0].p, sizeof v[0], GEOM_FLOAT3,
                             (const uint8_t *)v[0].n, sizeof v[0], 3) == 0, "record: usable");
    CHECK(op.first == 1 && s.nn == 3, "first draw at normal 0 (first %u, nn %u)", op.first, s.nn);
    for (int i = 0; i < 3 && s.n; i++)   /* the normal through the view rotation, at its own length */
        for (int r = 0; r < 3; r++) {
            const float e = R[r][0] * v[i].n[0] + R[r][1] * v[i].n[1] + R[r][2] * v[i].n[2];
            CHECK(fabsf(s.n[3 * i + r] - e) < 1e-5f, "vertex %d normal %d: %f vs %f", i, r, s.n[3 * i + r], e);
        }
    /* The box contains every view position, and is exact without rotation. */
    for (int i = 0; i < 3; i++)
        for (int r = 0; r < 3; r++) {
            const float q = R[r][0] * v[i].p[0] + R[r][1] * v[i].p[1] + R[r][2] * v[i].p[2] + t[r];
            CHECK(q >= op.box[r] - 1e-4f && q <= op.box[3 + r] + 1e-4f, "vertex %d axis %d inside the box: %f in %f..%f", i, r, q, op.box[r], op.box[3 + r]);
        }
    {
        float w0[4][4], R0[3][3];
        camera(0, t, px, py, w0, R0);
        WorldLitOp o0 = {0};
        CHECK(world_light_record(&s, &o0, (const float (*)[4])w0, GEOM_FLOAT3, (const uint8_t *)v[0].p, sizeof v[0], GEOM_FLOAT3,
                                 (const uint8_t *)v[0].n, sizeof v[0], 3) == 0, "unrotated: usable");
        const float want[6] = {-1 + t[0], 0 + t[1], -1 + t[2], 2 + t[0], 3 + t[1], 2 + t[2]};
        for (int k = 0; k < 6; k++) CHECK(fabsf(o0.box[k] - want[k]) < 1e-4f, "unrotated box %d: %f vs %f", k, o0.box[k], want[k]);
        s.nn -= 3;   /* drop it again */
    }
    /* A second draw appends after the first. */
    WorldLitOp op2 = {0};
    CHECK(world_light_record(&s, &op2, (const float (*)[4])wvp, GEOM_FLOAT3, (const uint8_t *)v[0].p, sizeof v[0], GEOM_FLOAT3,
                             (const uint8_t *)v[0].n, sizeof v[0], 2) == 0 && op2.first == 4 && s.nn == 5, "second draw follows (first %u)", op2.first);
    /* An unusable projection (c72 = 0): nothing recorded. */
    float bad[4][4];
    memcpy(bad, wvp, sizeof bad);
    memset(bad[3], 0, sizeof bad[3]);
    WorldLitOp op3 = {7, {0}};
    CHECK(world_light_record(&s, &op3, (const float (*)[4])bad, GEOM_FLOAT3, (const uint8_t *)v[0].p, sizeof v[0], GEOM_FLOAT3,
                             (const uint8_t *)v[0].n, sizeof v[0], 3) == -1 && op3.first == 0 && s.nn == 5, "unusable projection");
    CHECK(world_light_record(&s, &op3, (const float (*)[4])wvp, GEOM_FLOAT3, (const uint8_t *)v[0].p, sizeof v[0], GEOM_FLOAT3,
                             (const uint8_t *)v[0].n, sizeof v[0], 0) == -1 && op3.first == 0, "no vertex");
    world_lit_store_free(&s);
    CHECK(!s.n && !s.nn && !s.cap, "store freed");
}

static RLamp lamp(float x, float y, float z, float r, float intensity) { return (RLamp){{x, y, z}, r, {1, 0.8f, 0.6f}, intensity}; }

static void test_mask(void) {
    const float box[6] = {-1, -1, -6, 1, 1, -4};
    RLampLights p;
    memset(&p, 0, sizeof p);
    p.strength = 1;
    p.l[0] = lamp(0, 0, -5, 0.5f, 1);      /* inside */
    p.l[1] = lamp(3, 0, -5, 2.5f, 1);      /* 2 from the +x face, radius 2.5: reaches */
    p.l[2] = lamp(3, 0, -5, 1.9f, 1);      /* radius 1.9: does not */
    p.l[3] = lamp(2, 2, -3, 1.8f, 1);      /* corner distance sqrt(3) = 1.73: reaches */
    p.l[4] = lamp(2, 2, -3, 1.7f, 1);      /* does not */
    p.l[5] = lamp(0, 0, -5, 5, 0);         /* no intensity */
    p.n = 6;
    CHECK(world_light_mask(box, &p) == 0x0Bu, "mask 0x%x", world_light_mask(box, &p));
    p.n = 0;
    CHECK(world_light_mask(box, &p) == 0, "no lights");
    p.n = 40;   /* more than 32: the first 32 only */
    for (int i = 0; i < 32; i++) p.l[i] = lamp(0, 0, -5, 1, 1);
    CHECK(world_light_mask(box, &p) == 0xFFFFFFFFu, "32 lights");
    p.n = 32;
    p.strength = 0;
    CHECK(world_light_mask(box, &p) == 0, "strength 0");
}

int main(void) {
    test_options();
    test_classifier();
    test_record();
    test_mask();
    if (fails) {
        printf("world_light_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
