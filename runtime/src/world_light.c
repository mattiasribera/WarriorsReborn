/* --world-lighting: option, program classifier, view normals and light mask (world_light.h). */
#include "world_light.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "geom.h"   /* geom_fetch */
#include "ssao.h"   /* ssao_proj_scale */

static int parse_float(const char *s, float lo, float hi, float *out) {
    if (!s || !*s) return -1;
    char *end;
    const float v = strtof(s, &end);
    if (*end || !(v >= lo && v <= hi)) return -1;
    *out = v;
    return 0;
}

int world_light_arg(WorldLightConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--world-lighting") || !strcmp(opt, "--no-world-lighting")) {
        c->on = opt[2] == 'w';
        return 1;
    }
    if (!strcmp(opt, "--world-lighting-strength"))
        return parse_float(val, WORLD_LIGHT_STRENGTH_MIN, WORLD_LIGHT_STRENGTH_MAX, &c->strength) ? -1 : 2;
    return 0;
}

/* vsh.c's numbering of the MAC opcodes, operand sources and output registers. */
enum { MAC_ADD = 3, MAC_MAD = 4, MAC_DP4 = 7, MAC_ARL = 13 };
enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
#define CREG(d3d) ((d3d) + 96)   /* constant index of a D3D register */

/* Operand s of x reads input register v / temporary r. */
static int reads_v(const VshIns *x, const VshSrc *s, int v) { return s->mux == MUX_V && x->vreg == v; }
static int reads_c(const VshSrc *s) { return s->mux == MUX_C; }

int world_light_program(const VshProgram *p) {
    uint32_t pos_mask = 0;
    int d0_v2 = 0, object = 0;
    uint16_t tainted = 0;   /* temporaries holding a value that the input v2 (the baked colour) reached */
    for (uint32_t i = 0; i < p->n; i++) {
        const VshIns *x = &p->ins[i];
        if (x->mac == MAC_ARL || x->a0x) return WORLD_PROG_NONE;
        const VshSrc *src[3] = {&x->a, &x->b, &x->c};
        int any_c = 0, t_mac = 0;
        for (int k = 0; k < 3; k++) {
            any_c |= reads_c(src[k]);
            t_mac |= reads_v(x, src[k], 2) || (src[k]->mux == MUX_R && src[k]->reg < 12 && (tainted >> src[k]->reg & 1));
        }
        if (any_c && x->creg >= CREG(62) && x->creg <= CREG(64)) object = 1;
        const int t_ilu = reads_v(x, &x->c, 2) || (x->c.mux == MUX_R && x->c.reg < 12 && (tainted >> x->c.reg & 1));
        const int mac_out = x->o_mask && x->o_reg && !x->o_ilu;
        /* oPos component k = dp4 of the raw v0 with c69 + k. */
        if (x->mac == MAC_DP4 && mac_out && x->o_addr == 0) {
            for (int k = 0; k < 4; k++) {
                if (!(x->o_mask & (8 >> k))) continue;
                const int raw = (reads_v(x, &x->a, 0) && reads_c(&x->b)) || (reads_v(x, &x->b, 0) && reads_c(&x->a));
                if (raw && x->creg == CREG(69 + k) && x->o_mask == (8 >> k)) pos_mask |= 8u >> k;
            }
        }
        /* oD0.rgb = a sum (add, mad) the baked colour reached. */
        if ((x->mac == MAC_ADD || x->mac == MAC_MAD) && mac_out && x->o_addr == 3 && (x->o_mask & 14) == 14 && t_mac) d0_v2 = 1;
        /* The temporaries written: a full xyz write replaces the taint, a partial one adds to it. */
        if (x->mac && x->mac != MAC_ARL && x->mac_mask && x->out_r < 12) {
            const uint16_t b = (uint16_t)(1u << x->out_r);
            tainted = (x->mac_mask & 14) == 14 ? (uint16_t)((tainted & ~b) | (t_mac ? b : 0)) : (uint16_t)(tainted | (t_mac ? b : 0));
        }
        if (x->ilu && x->ilu_mask) {
            const int r = x->mac ? 1 : x->out_r;   /* a paired ILU writes r1 */
            if (r < 12) {
                const uint16_t b = (uint16_t)(1u << r);
                tainted = (x->ilu_mask & 14) == 14 ? (uint16_t)((tainted & ~b) | (t_ilu ? b : 0)) : (uint16_t)(tainted | (t_ilu ? b : 0));
            }
        }
    }
    if (pos_mask != 15 || !d0_v2) return WORLD_PROG_NONE;
    return object ? WORLD_PROG_OBJECT : WORLD_PROG_WORLD;
}

void world_lit_store_free(WorldLitStore *s) {
    free(s->n);
    memset(s, 0, sizeof *s);
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* One element as 3 floats: FLOAT3 read directly (the common case), any other type through geom_fetch. */
static inline void fetch3(uint32_t type, const uint8_t *p, float out[3]) {
    if (type == GEOM_FLOAT3) {
        memcpy(out, p, 12);
        return;
    }
    float t[4];
    geom_fetch(type, p, t);
    out[0] = t[0];
    out[1] = t[1];
    out[2] = t[2];
}

int world_light_record(WorldLitStore *s, WorldLitOp *op, const float (*wvp)[4], uint32_t pos_type, const uint8_t *pos, uint32_t pos_stride,
                       uint32_t nrm_type, const uint8_t *nrm, uint32_t nrm_stride, uint32_t nv) {
    op->first = 0;
    float sc[2];
    if (!nv || ssao_proj_scale(wvp[0], wvp[1], wvp[3], sc)) return -1;
    if (s->nn + nv > s->cap) {
        uint32_t c = s->cap ? s->cap : 65536;
        while (c < s->nn + nv) c *= 2;
        float *n = realloc(s->n, (size_t)c * 3 * sizeof *n);
        if (!n) return -1;
        s->n = n;
        s->cap = c;
    }
    const float ix = 1.0f / sc[0], iy = 1.0f / sc[1];
    /* The view-space rows: x = c69 / px, y = c70 / py, z = -c72. */
    float rx[4], ry[4], rz[4];
    for (int k = 0; k < 4; k++) {
        rx[k] = wvp[0][k] * ix;
        ry[k] = wvp[1][k] * iy;
        rz[k] = -wvp[3][k];
    }
    /* Normals through the rows (the fragment shader renormalises the interpolated normal, so they
     * are left at the length the transform gives: unit for the title's rigid world and object
     * transforms); the box from the input positions' box, its 8 corners into view space. */
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    float *out = s->n + (size_t)s->nn * 3;
    for (uint32_t i = 0; i < nv; i++) {
        float p[3], n[3];
        fetch3(pos_type, pos + (size_t)i * pos_stride, p);
        fetch3(nrm_type, nrm + (size_t)i * nrm_stride, n);
        for (int k = 0; k < 3; k++) {
            lo[k] = p[k] < lo[k] ? p[k] : lo[k];
            hi[k] = p[k] > hi[k] ? p[k] : hi[k];
        }
        out[3 * i] = dot3(rx, n);
        out[3 * i + 1] = dot3(ry, n);
        out[3 * i + 2] = dot3(rz, n);
    }
    for (int k = 0; k < 3; k++) {
        op->box[k] = 1e30f;
        op->box[3 + k] = -1e30f;
    }
    for (int c = 0; c < 8; c++) {
        const float p[3] = {c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]};
        const float q[3] = {dot3(rx, p) + rx[3], dot3(ry, p) + ry[3], dot3(rz, p) + rz[3]};
        for (int k = 0; k < 3; k++) {
            op->box[k] = q[k] < op->box[k] ? q[k] : op->box[k];
            op->box[3 + k] = q[k] > op->box[3 + k] ? q[k] : op->box[3 + k];
        }
    }
    op->first = s->nn + 1;
    s->nn += nv;
    return 0;
}

uint32_t world_light_mask(const float box[6], const RLampLights *p) {
    if (!(p->strength > 0)) return 0;
    uint32_t m = 0;
    const uint32_t n = p->n < RENDERER_LAMP_MAX ? p->n : RENDERER_LAMP_MAX;
    for (uint32_t i = 0; i < n && i < 32; i++) {
        const RLamp *l = &p->l[i];
        if (!(l->intensity > 0) || !(l->rgb[0] + l->rgb[1] + l->rgb[2] > 0) || !(l->radius > 0)) continue;
        float d2 = 0;
        for (int k = 0; k < 3; k++) {
            const float c = l->pos[k] < box[k] ? box[k] : l->pos[k] > box[3 + k] ? box[3 + k] : l->pos[k];
            d2 += (l->pos[k] - c) * (l->pos[k] - c);
        }
        if (d2 < l->radius * l->radius) m |= 1u << i;
    }
    return m;
}
