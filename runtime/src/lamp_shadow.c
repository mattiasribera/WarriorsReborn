/* --lamp-shadows options and the renderer input (lamp_shadow.h). */
#include "lamp_shadow.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int lamp_shadow_arg(LampShadowConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--lamp-shadows") || !strcmp(opt, "--no-lamp-shadows")) {
        c->on = opt[2] == 'l';
        return 1;
    }
    if (!strcmp(opt, "--lamp-shadows-strength")) {
        char *end;
        if (!val || !*val) return -1;
        float f = (float)strtod(val, &end);
        if (*end || !(f >= LAMP_SHADOW_STRENGTH_MIN && f <= LAMP_SHADOW_STRENGTH_MAX)) return -1;   /* NaN fails too */
        c->strength = f;
        return 2;
    }
    return 0;
}

int lamp_shadow_invert(const float m[4][4], float inv[4][4]) {
    double a[4][8];   /* [m | I], Gauss-Jordan with partial pivoting */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i][j] : (j - 4 == i);
    double scale = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) scale = fmax(scale, fabs(m[i][j]));
    if (!(scale > 0) || !isfinite(scale)) return -1;
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (!(fabs(a[p][c]) > 1e-12 * scale)) return -1;
        if (p != c)
            for (int j = 0; j < 8; j++) {
                const double t = a[c][j];
                a[c][j] = a[p][j];
                a[p][j] = t;
            }
        const double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c || a[r][c] == 0) continue;
            const double f = a[r][c];
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) inv[i][j] = (float)a[i][j + 4];
    return 0;
}

/* r = A q - B: the r row is A x the q row minus B x (0, 0, 0, 1). */
static int light_depth(const float prj[4][4], float *a, float *b) {
    int k = 0;
    for (int i = 1; i < 3; i++)
        if (fabsf(prj[3][i]) > fabsf(prj[3][k])) k = i;
    if (!(fabsf(prj[3][k]) > 0)) return -1;
    const double A = (double)prj[2][k] / prj[3][k], B = A * prj[3][3] - prj[2][3];
    if (!(A > 0) || !(B > 0)) return -1;
    for (int i = 0; i < 3; i++)   /* the other columns follow the same A */
        if (fabs(prj[2][i] - A * prj[3][i]) > 1e-4 * fabs(A) * (fabs(prj[3][k]) + 1e-6)) return -1;
    *a = (float)A;
    *b = (float)B;
    return 0;
}

typedef struct {
    float d2;
    uint32_t i;
} ByDistance;
static int by_distance(const void *x, const void *y) {
    const ByDistance *a = x, *b = y;
    return a->d2 < b->d2 ? -1 : a->d2 > b->d2 ? 1 : a->i < b->i ? -1 : a->i > b->i;
}

int lamp_shadow_setup(const float prj[4][4], const LampShadowLight *lights, uint32_t n, const float eye[3], float strength,
                      RLampShadows *out) {
    memset(out, 0, sizeof *out);
    if (light_depth(prj, &out->depth_a, &out->depth_b) || lamp_shadow_invert(prj, out->to_world)) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    out->strength = strength;
    out->height = LAMP_SHADOW_HEIGHT;
    out->length = LAMP_SHADOW_LENGTH;
    out->start = LAMP_SHADOW_START;
    out->dir_share = LAMP_SHADOW_DIR_SHARE;
    ByDistance *order = n ? malloc(n * sizeof *order) : NULL;
    if (n && !order) return 0;
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        const LampShadowLight *l = &lights[i];
        if (!(l->intensity > 0.01f) || !(l->radius > 0)) continue;
        const float q = prj[3][0] * l->pos[0] + prj[3][1] * l->pos[1] + prj[3][2] * l->pos[2] + prj[3][3];
        if (!(q > 0)) continue;
        const float dx = l->pos[0] - eye[0], dy = l->pos[1] - eye[1], dz = l->pos[2] - eye[2];
        order[m++] = (ByDistance){dx * dx + dy * dy + dz * dz, i};
    }
    qsort(order, m, sizeof *order, by_distance);
    if (m > RENDERER_LAMP_SHADOW_MAX) m = RENDERER_LAMP_SHADOW_MAX;
    for (uint32_t k = 0; k < m; k++) {
        const LampShadowLight *l = &lights[order[k].i];
        RLampShadowLight *o = &out->l[k];
        for (int r = 0; r < 4; r++) o->stq[r] = prj[r][0] * l->pos[0] + prj[r][1] * l->pos[1] + prj[r][2] * l->pos[2] + prj[r][3];
        memcpy(o->pos, l->pos, sizeof o->pos);
        o->radius = l->radius;
        o->weight = l->intensity < 1 ? l->intensity : 1;
    }
    free(order);
    out->n = m;
    return (int)m;
}
