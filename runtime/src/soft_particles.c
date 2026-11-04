/* --soft-particles options, the particle draw signature and the soft quad rule (soft_particles.h). */
#include "soft_particles.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int soft_arg(SoftConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--soft-particles") || !strcmp(opt, "--no-soft-particles")) {
        c->on = opt[2] == 's';
        return 1;
    }
    if (strcmp(opt, "--soft-particles-distance")) return 0;
    char *end;
    if (!val || !*val) return -1;
    const float f = (float)strtod(val, &end);   /* compared as float: "0.05" is exactly the 0.05f bound */
    if (*end || !(f >= SOFT_DISTANCE_MIN && f <= SOFT_DISTANCE_MAX)) return -1;   /* NaN fails the range check */
    c->distance = f;
    return 2;
}

int soft_particle_draw(const SoftDraw *d) {
    const int add = !d->blend_eq || d->blend_eq == 0x8006 || d->blend_eq == 0xF006;   /* unset: ADD (the NV2A reset value) */
    const int src = d->blend_src == 1 || d->blend_src == 0x302;                     /* ONE, SRC_ALPHA */
    const int dst = d->blend_dst == 1 || d->blend_dst == 0x303;                     /* ONE, ONE_MINUS_SRC_ALPHA */
    return !d->offscreen && d->fvf == SOFT_FVF && d->prim == SOFT_PRIM_TRIANGLELIST && d->stride == SOFT_STRIDE && d->indexed &&
           d->depth_test && !d->depth_write && d->blend_enable && src && dst && add;
}

/* View position of a vertex from its clip coordinates. */
static void view_pos(const float *p, const SoftView *v, float out[3]) {
    out[0] = p[0] / v->px;
    out[1] = p[1] / v->py;
    out[2] = p[3] * v->zsign;
}
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static int soft_quad(const float *xyzw, size_t stride, const uint32_t *idx, const SoftView *v) {
    float p[6][3], c[3] = {0, 0, 0};
    for (int i = 0; i < 6; i++) {
        view_pos(xyzw + (size_t)idx[i] * stride, v, p[i]);
        for (int k = 0; k < 3; k++) c[k] += p[i][k] / 6;
    }
    const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
    const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const float nn = sqrtf(dot3(n, n)), cc = sqrtf(dot3(c, c));
    if (!(nn > 1e-12f) || !(cc > 1e-6f)) return 0;
    if (!(fabsf(n[2]) >= SOFT_FACING_MIN * nn) && !(fabsf(dot3(n, c)) >= SOFT_FACING_MIN * nn * cc)) return 0;
    if (v->has_up) {
        const float un = sqrtf(dot3(v->up, v->up));
        if (un > 0 && !(fabsf(dot3(n, v->up)) < SOFT_LEVEL_MAX * nn * un)) return 0;
    }
    return 1;
}

uint32_t soft_classify(const float *xyzw, size_t stride, const uint32_t *idx, uint32_t ni, const SoftView *v, uint8_t *soft) {
    if (!ni || ni % 6 || !(v->px > 0) || !(v->py > 0) || !v->zsign) return 0;
    for (uint32_t q = 0; q < ni / 6; q++) soft[q] = (uint8_t)soft_quad(xyzw, stride, idx + q * 6, v);
    return ni / 6;
}
