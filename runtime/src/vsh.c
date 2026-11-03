/* NV2A vertex program interpreter (see vsh.h). Field positions and opcode semantics follow xemu
 * hw/xbox/nv2a/pgraph/vsh.c (https://github.com/xemu-project/xemu) and the D3D8 vs.1.1
 * instruction reference; the rcc clamp range is the NV2A one used by xemu and Cxbx-Reloaded. */
#include "vsh.h"
#include <limits.h>
#include <math.h>
#include <string.h>

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };

/* Bit field of an instruction: dword, position, width. */
static uint32_t field(const uint32_t *t, int dw, int pos, int bits) { return (t[dw] >> pos) & ((1u << bits) - 1); }
static void swizzle(uint32_t s, uint8_t out[4]) {
    for (int i = 0; i < 4; i++) out[i] = (s >> (6 - 2 * i)) & 3;
}

int vsh_load(const uint32_t *f, uint32_t max_dwords, VshProgram *p) {
    memset(p, 0, sizeof *p);
    if (!f || max_dwords < 1 || (f[0] & 0xFF) != 0x78) return -1;
    uint32_t n = f[0] >> 16;
    if (!n || n > VSH_MAX_INSTRUCTIONS || 1 + 4 * n > max_dwords) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t *t = f + 1 + 4 * i;
        VshIns *x = &p->ins[i];
        x->ilu = field(t, 1, 25, 3);
        x->mac = field(t, 1, 21, 4);
        x->creg = field(t, 1, 13, 8);
        x->vreg = field(t, 1, 9, 4);
        x->a.neg = field(t, 1, 8, 1);
        swizzle(field(t, 1, 0, 8), x->a.swz);
        x->a.reg = field(t, 2, 28, 4);
        x->a.mux = field(t, 2, 26, 2);
        x->b.neg = field(t, 2, 25, 1);
        swizzle(field(t, 2, 17, 8), x->b.swz);
        x->b.reg = field(t, 2, 13, 4);
        x->b.mux = field(t, 2, 11, 2);
        x->c.neg = field(t, 2, 10, 1);
        swizzle(field(t, 2, 2, 8), x->c.swz);
        x->c.reg = field(t, 2, 0, 2) << 2 | field(t, 3, 30, 2);
        x->c.mux = field(t, 3, 28, 2);
        x->mac_mask = field(t, 3, 24, 4);
        x->out_r = field(t, 3, 20, 4);
        x->ilu_mask = field(t, 3, 16, 4);
        x->o_mask = field(t, 3, 12, 4);
        x->o_reg = field(t, 3, 11, 1);    /* 1 = output register, 0 = constant */
        x->o_addr = field(t, 3, 3, 8);
        x->o_ilu = field(t, 3, 2, 1);     /* output from the ILU (1) or the MAC (0) */
        x->a0x = field(t, 3, 1, 1);
        p->n = i + 1;
        if (field(t, 3, 0, 1)) break;     /* FINAL */
    }
    return 0;
}

typedef struct {
    float r[13][4];   /* r12 = oPos (o[0]) */
    float (*o)[4];
    const float (*v)[4];
    const float (*c)[4];
    int a0;
} Regs;

static void read_src(const Regs *g, const VshIns *x, const VshSrc *s, float out[4]) {
    static const float zero[4];
    const float *base = zero;
    if (s->mux == MUX_R) base = s->reg == 12 ? g->o[VSH_OPOS] : s->reg < 12 ? g->r[s->reg] : zero;
    else if (s->mux == MUX_V) base = g->v[x->vreg];
    else if (s->mux == MUX_C) {
        int i = x->creg + (x->a0x ? g->a0 : 0);
        base = i >= 0 && i < VSH_NCONST ? g->c[i] : zero;
    }
    for (int k = 0; k < 4; k++) out[k] = s->neg ? -base[s->swz[k]] : base[s->swz[k]];
}

/* fminf/fmaxf with their NaN rule (a NaN operand yields the other), written out so the batch
 * loops vectorize; both interpreters use these. */
static inline float vmin(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static inline float vmax(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b; }
/* (int)floorf(x) without the libm call (no SSE4.1 round on the i386 baseline; floorf was 60 ns
 * per vertex): truncation, minus one below zero; out-of-range values give INT_MIN like the
 * conversion of floorf's result. */
static inline int ifloor(float x) {
    int t = (int)x;
    return t != INT_MIN && (float)t > x ? t - 1 : t;
}
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void splat(float out[4], float v) { out[0] = out[1] = out[2] = out[3] = v; }
static void write_masked(float *dst, const float *v, int mask) {
    for (int k = 0; k < 4; k++)
        if (mask & (8 >> k)) dst[k] = v[k];
}

/* MAC result; returns 0 for NOP/ARL (no register result). */
static int mac_op(Regs *g, const VshIns *x, float m[4]) {
    float a[4], b[4], c[4];
    read_src(g, x, &x->a, a);
    read_src(g, x, &x->b, b);
    read_src(g, x, &x->c, c);
    switch (x->mac) {
    case MAC_MOV: memcpy(m, a, 16); return 1;
    case MAC_MUL: for (int k = 0; k < 4; k++) m[k] = a[k] * b[k]; return 1;
    case MAC_ADD: for (int k = 0; k < 4; k++) m[k] = a[k] + c[k]; return 1;
    case MAC_MAD: for (int k = 0; k < 4; k++) m[k] = a[k] * b[k] + c[k]; return 1;
    case MAC_DP3: splat(m, dot3(a, b)); return 1;
    case MAC_DPH: splat(m, dot3(a, b) + b[3]); return 1;
    case MAC_DP4: splat(m, dot3(a, b) + a[3] * b[3]); return 1;
    case MAC_DST: m[0] = 1; m[1] = a[1] * b[1]; m[2] = a[2]; m[3] = b[3]; return 1;
    case MAC_MIN: for (int k = 0; k < 4; k++) m[k] = vmin(a[k], b[k]); return 1;
    case MAC_MAX: for (int k = 0; k < 4; k++) m[k] = vmax(a[k], b[k]); return 1;
    case MAC_SLT: for (int k = 0; k < 4; k++) m[k] = a[k] < b[k] ? 1.0f : 0.0f; return 1;
    case MAC_SGE: for (int k = 0; k < 4; k++) m[k] = a[k] >= b[k] ? 1.0f : 0.0f; return 1;
    case MAC_ARL: g->a0 = ifloor(a[0] + 0.001f); return 0;   /* xemu: floor with a small bias [I] */
    default: return 0;
    }
}

#define RCC_MIN 5.42101e-020f
#define RCC_MAX 1.84467e+019f
static int ilu_op(const Regs *g, const VshIns *x, float m[4]) {
    float c[4];
    read_src(g, x, &x->c, c);
    float s = c[0];
    switch (x->ilu) {
    case ILU_MOV: memcpy(m, c, 16); return 1;
    case ILU_RCP: splat(m, 1.0f / s); return 1;
    case ILU_RCC: {
        float r = 1.0f / s, mag = fabsf(r);
        mag = mag < RCC_MIN ? RCC_MIN : mag > RCC_MAX || isnan(mag) ? RCC_MAX : mag;
        splat(m, signbit(r) ? -mag : mag);
        return 1;
    }
    case ILU_RSQ: splat(m, 1.0f / sqrtf(fabsf(s))); return 1;
    case ILU_EXP: {
        float fl = floorf(s);
        m[0] = exp2f(fl); m[1] = s - fl; m[2] = exp2f(s); m[3] = 1;
        return 1;
    }
    case ILU_LOG: {
        float as = fabsf(s);
        if (as == 0) { m[0] = -INFINITY; m[1] = 1; m[2] = -INFINITY; m[3] = 1; return 1; }
        float e = floorf(log2f(as));
        m[0] = e; m[1] = as / exp2f(e); m[2] = log2f(as); m[3] = 1;
        return 1;
    }
    case ILU_LIT: {
        float p = c[3] < -127.9961f ? -127.9961f : c[3] > 127.9961f ? 127.9961f : c[3];
        m[0] = 1;
        m[1] = c[0] > 0 ? c[0] : 0;
        m[2] = c[0] > 0 && c[1] > 0 ? powf(c[1], p) : 0;
        m[3] = 1;
        return 1;
    }
    default: return 0;
    }
}

static float sat(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }   /* NaN -> 0 */

void vsh_run(const VshProgram *p, const float v[16][4], const float c[VSH_NCONST][4], float o[VSH_NOUT][4]) {
    Regs g;
    memset(g.r, 0, sizeof g.r);
    for (int i = 0; i < VSH_NOUT; i++) { o[i][0] = o[i][1] = o[i][2] = 0; o[i][3] = 1; }
    g.o = o;
    g.v = v;
    g.c = c;
    g.a0 = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        const VshIns *x = &p->ins[i];
        float m[4] = {0}, l[4] = {0};
        /* Both units read their operands before either writes (the instruction is one bundle). */
        int has_m = x->mac ? mac_op(&g, x, m) : 0;
        int has_l = x->ilu ? ilu_op(&g, x, l) : 0;
        if (has_m && x->mac_mask && x->out_r < 12) write_masked(g.r[x->out_r], m, x->mac_mask);
        if (has_l && x->ilu_mask) {
            int r = x->mac ? 1 : x->out_r;   /* a paired ILU writes r1 */
            if (r < 12) write_masked(g.r[r], l, x->ilu_mask);
        }
        if (x->o_mask && x->o_reg && x->o_addr < VSH_NOUT && (x->o_ilu ? has_l : has_m))
            write_masked(o[x->o_addr], x->o_ilu ? l : m, x->o_mask);
    }
    static const int colors[4] = {VSH_OD0, VSH_OD1, VSH_OB0, VSH_OB1};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) o[colors[i]][k] = sat(o[colors[i]][k]);
}

/* ---- batch interpreter: the same operations, one instruction over n vertices ----
 * Every expression mirrors the per-vertex code above in operand order, so the results are
 * bit-identical (no reassociation without -ffast-math). The registers live in thread-local
 * storage: draws are recorded on guest threads, whose stacks are the title's. */
#define NB VSH_BATCH
typedef struct {
    float r[12][4][NB];
    float (*o)[4][NB];
    const float (*v)[4][NB];
    const float (*c)[4];
    int a0[NB], n;
} BRegs;
static __thread BRegs breg;

/* Operand rows: pointers into registers/inputs when the swizzle can be served in place, else
 * into tmp (negation, constants, relative addressing, unused registers). */
#define LANES(...) for (int j = 0; j < n; j++) { __VA_ARGS__; }
typedef const float *Rows[4];
static void bread_src(const BRegs *g, const VshIns *x, const VshSrc *s, float tmp[4][NB], Rows out) {
    const float (*rows)[NB] = NULL;
    int n = g->n;
    if (s->mux == MUX_R) rows = s->reg == 12 ? (const float (*)[NB])g->o[VSH_OPOS] : s->reg < 12 ? (const float (*)[NB])g->r[s->reg] : NULL;
    else if (s->mux == MUX_V) rows = g->v[x->vreg];
    else if (s->mux == MUX_C) {
        static const float zero4[4];
        if (!x->a0x) {
            for (int k = 0; k < 4; k++) {
                float val = x->creg < VSH_NCONST ? g->c[x->creg][s->swz[k]] : 0;
                val = s->neg ? -val : val;
                for (int j = 0; j < n; j++) tmp[k][j] = val;
            }
        } else {   /* relative: one register per vertex */
            int s0 = s->swz[0], s1 = s->swz[1], s2 = s->swz[2], s3 = s->swz[3];
            for (int j = 0; j < n; j++) {
                int i = x->creg + g->a0[j];
                const float *r = i >= 0 && i < VSH_NCONST ? g->c[i] : zero4;
                tmp[0][j] = r[s0];
                tmp[1][j] = r[s1];
                tmp[2][j] = r[s2];
                tmp[3][j] = r[s3];
            }
            if (s->neg)
                for (int k = 0; k < 4; k++) LANES(tmp[k][j] = -tmp[k][j])
        }
        for (int k = 0; k < 4; k++) out[k] = tmp[k];
        return;
    }
    for (int k = 0; k < 4; k++) {
        if (!rows) {
            for (int j = 0; j < n; j++) tmp[k][j] = s->neg ? -0.0f : 0.0f;
            out[k] = tmp[k];
        } else if (s->neg) {
            const float *row = rows[s->swz[k]];
            for (int j = 0; j < n; j++) tmp[k][j] = -row[j];
            out[k] = tmp[k];
        } else {
            out[k] = rows[s->swz[k]];
        }
    }
}

/* Operands each MAC operation reads (bit 0 a, 1 b, 2 c); reads have no side effects, so the
 * unused ones are skipped. */
static const uint8_t mac_reads[] = {0, 1, 3, 5, 7, 3, 3, 3, 3, 3, 3, 3, 3, 1};
static int bmac_op(BRegs *g, const VshIns *x, float m[4][NB]) {
    float ta[4][NB], tb[4][NB], tc[4][NB];
    Rows a = {0}, b = {0}, c = {0};
    int n = g->n, rd = x->mac < sizeof mac_reads ? mac_reads[x->mac] : 0;
    if (rd & 1) bread_src(g, x, &x->a, ta, a);
    if (rd & 2) bread_src(g, x, &x->b, tb, b);
    if (rd & 4) bread_src(g, x, &x->c, tc, c);
    switch (x->mac) {
    case MAC_MOV: for (int k = 0; k < 4; k++) memcpy(m[k], a[k], (size_t)n * 4); return 1;
    case MAC_MUL: for (int k = 0; k < 4; k++) LANES(m[k][j] = a[k][j] * b[k][j]) return 1;
    case MAC_ADD: for (int k = 0; k < 4; k++) LANES(m[k][j] = a[k][j] + c[k][j]) return 1;
    case MAC_MAD: for (int k = 0; k < 4; k++) LANES(m[k][j] = a[k][j] * b[k][j] + c[k][j]) return 1;
    case MAC_DP3: LANES(m[0][j] = a[0][j] * b[0][j] + a[1][j] * b[1][j] + a[2][j] * b[2][j]) break;
    case MAC_DPH: LANES(m[0][j] = a[0][j] * b[0][j] + a[1][j] * b[1][j] + a[2][j] * b[2][j] + b[3][j]) break;
    case MAC_DP4: LANES(m[0][j] = a[0][j] * b[0][j] + a[1][j] * b[1][j] + a[2][j] * b[2][j] + a[3][j] * b[3][j]) break;
    case MAC_DST: LANES(m[0][j] = 1; m[1][j] = a[1][j] * b[1][j]; m[2][j] = a[2][j]; m[3][j] = b[3][j]) return 1;
    case MAC_MIN: for (int k = 0; k < 4; k++) LANES(m[k][j] = vmin(a[k][j], b[k][j])) return 1;
    case MAC_MAX: for (int k = 0; k < 4; k++) LANES(m[k][j] = vmax(a[k][j], b[k][j])) return 1;
    case MAC_SLT: for (int k = 0; k < 4; k++) LANES(m[k][j] = a[k][j] < b[k][j] ? 1.0f : 0.0f) return 1;
    case MAC_SGE: for (int k = 0; k < 4; k++) LANES(m[k][j] = a[k][j] >= b[k][j] ? 1.0f : 0.0f) return 1;
    case MAC_ARL: LANES(g->a0[j] = ifloor(a[0][j] + 0.001f)) return 0;
    default: return 0;
    }
    for (int k = 1; k < 4; k++) memcpy(m[k], m[0], (size_t)n * 4);   /* dot products: splat */
    return 1;
}

static int bilu_op(const BRegs *g, const VshIns *x, float m[4][NB]) {
    float tc[4][NB];
    Rows c;
    int n = g->n;
    if (x->ilu > ILU_LIT) return 0;
    bread_src(g, x, &x->c, tc, c);
    const float *s = c[0];
    switch (x->ilu) {
    case ILU_MOV: for (int k = 0; k < 4; k++) memcpy(m[k], c[k], (size_t)n * 4); return 1;
    case ILU_RCP: LANES(m[0][j] = 1.0f / s[j]) break;
    case ILU_RCC: LANES(float r = 1.0f / s[j], mag = fabsf(r);
                        mag = mag < RCC_MIN ? RCC_MIN : mag > RCC_MAX || isnan(mag) ? RCC_MAX : mag;
                        m[0][j] = signbit(r) ? -mag : mag) break;
    case ILU_RSQ: LANES(m[0][j] = 1.0f / sqrtf(fabsf(s[j]))) break;
    case ILU_EXP: LANES(float fl = floorf(s[j]); m[0][j] = exp2f(fl); m[1][j] = s[j] - fl; m[2][j] = exp2f(s[j]); m[3][j] = 1) return 1;
    case ILU_LOG:
        LANES(float as = fabsf(s[j]);
              if (as == 0) { m[0][j] = -INFINITY; m[1][j] = 1; m[2][j] = -INFINITY; m[3][j] = 1; continue; }
              float e = floorf(log2f(as));
              m[0][j] = e; m[1][j] = as / exp2f(e); m[2][j] = log2f(as); m[3][j] = 1)
        return 1;
    case ILU_LIT:
        LANES(float w = c[3][j], p = w < -127.9961f ? -127.9961f : w > 127.9961f ? 127.9961f : w;
              m[0][j] = 1;
              m[1][j] = c[0][j] > 0 ? c[0][j] : 0;
              m[2][j] = c[0][j] > 0 && c[1][j] > 0 ? powf(c[1][j], p) : 0;
              m[3][j] = 1)
        return 1;
    default: return 0;
    }
    for (int k = 1; k < 4; k++) memcpy(m[k], m[0], (size_t)n * 4);   /* scalar results: splat */
    return 1;
}

static void bwrite_masked(float dst[4][NB], float v[4][NB], int mask, int n) {
    for (int k = 0; k < 4; k++)
        if (mask & (8 >> k)) memcpy(dst[k], v[k], (size_t)n * 4);
}

void vsh_run_batch(const VshProgram *p, int n, const float v[16][4][VSH_BATCH], const float c[VSH_NCONST][4],
                   float o[VSH_NOUT][4][VSH_BATCH]) {
    BRegs *g = &breg;
    if (n < 1) return;
    if (n > NB) n = NB;
    memset(g->r, 0, sizeof g->r);
    memset(g->a0, 0, sizeof g->a0);
    for (int i = 0; i < VSH_NOUT; i++)
        for (int k = 0; k < 4; k++)
            for (int j = 0; j < n; j++) o[i][k][j] = k == 3 ? 1.0f : 0.0f;
    g->o = o;
    g->v = v;
    g->c = c;
    g->n = n;
    for (uint32_t i = 0; i < p->n; i++) {
        const VshIns *x = &p->ins[i];
        float m[4][NB], l[4][NB];
        int has_m = x->mac ? bmac_op(g, x, m) : 0;
        int has_l = x->ilu ? bilu_op(g, x, l) : 0;
        if (has_m && x->mac_mask && x->out_r < 12) bwrite_masked(g->r[x->out_r], m, x->mac_mask, n);
        if (has_l && x->ilu_mask) {
            int r = x->mac ? 1 : x->out_r;
            if (r < 12) bwrite_masked(g->r[r], l, x->ilu_mask, n);
        }
        if (x->o_mask && x->o_reg && x->o_addr < VSH_NOUT && (x->o_ilu ? has_l : has_m))
            bwrite_masked(o[x->o_addr], x->o_ilu ? l : m, x->o_mask, n);
    }
    static const int colors[4] = {VSH_OD0, VSH_OD1, VSH_OB0, VSH_OB1};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            for (int j = 0; j < n; j++) o[colors[i]][k][j] = sat(o[colors[i]][k][j]);
}

void vsh_set_screen_constants(float c[VSH_NCONST][4]) {
    for (int k = 0; k < 4; k++) {
        c[-38 + 96][k] = 1;
        c[-37 + 96][k] = 0;
    }
}

void vsh_clip(const float opos[4], float clip[4]) {
    float w = opos[3];
    clip[0] = opos[0] * w;
    clip[1] = opos[1] * w;
    clip[2] = opos[2] * w;
    clip[3] = w;
}

int vsh_bone_palette(const VshProgram *p) {
    int arl = 0, rel = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        arl |= p->ins[i].mac == MAC_ARL;
        rel |= p->ins[i].a0x;
    }
    return arl && rel;
}
