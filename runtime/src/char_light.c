/* --char-lighting: option and smooth character normals (char_light.h). */
#include "char_light.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int char_light_arg(CharLightConfig *c, const char *opt) {
    if (!strcmp(opt, "--char-lighting") || !strcmp(opt, "--no-char-lighting")) {
        c->on = opt[2] == 'c';
        return 1;
    }
    return 0;
}

/* Vertex i's clip position as a sort key (bit patterns: exactly equal positions sort together). */
typedef struct {
    uint32_t k[4];
    uint32_t i;
} WeldKey;
static int weld_cmp(const void *a, const void *b) {
    const WeldKey *x = a, *y = b;
    for (int c = 0; c < 4; c++)
        if (x->k[c] != y->k[c]) return x->k[c] < y->k[c] ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

/* Scratch memory, grown on demand (one caller: the renderer thread). */
static float *pos_buf;
static WeldKey *key_buf;
static uint32_t cap;

int char_light_normals(const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, float px, float py, float *n) {
    for (uint32_t i = 0; i < nv; i++) {
        n[3 * i] = n[3 * i + 1] = 0;
        n[3 * i + 2] = 0;
    }
    if (!nv || ni < 3 || !(fabsf(px) > 1e-6f) || !(fabsf(py) > 1e-6f)) {
        for (uint32_t i = 0; i < nv; i++) n[3 * i + 2] = 1;
        return -1;
    }
    if (nv > cap) {
        float *p = realloc(pos_buf, (size_t)nv * 3 * sizeof *p);
        if (p) pos_buf = p;
        WeldKey *k = realloc(key_buf, (size_t)nv * sizeof *k);
        if (k) key_buf = k;
        if (!p || !k) {
            for (uint32_t i = 0; i < nv; i++) n[3 * i + 2] = 1;
            return -1;
        }
        cap = nv;
    }
    float *P = pos_buf;
    for (uint32_t i = 0; i < nv; i++) {
        P[3 * i] = v[i].x / px;
        P[3 * i + 1] = v[i].y / py;
        P[3 * i + 2] = -v[i].w;
    }
    /* Area-weighted face normals into their vertices; the mesh's volume seen from the camera. */
    double vol = 0;
    uint32_t used = 0;
    for (uint32_t t = 0; t + 3 <= ni; t += 3) {
        const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= nv || b >= nv || c >= nv || !(v[a].w > 0) || !(v[b].w > 0) || !(v[c].w > 0)) continue;
        const float *A = P + 3 * a, *B = P + 3 * b, *C = P + 3 * c;
        const float e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
        const float f[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        if (!(f[0] * f[0] + f[1] * f[1] + f[2] * f[2] > 1e-24f)) continue;
        vol += (double)A[0] * f[0] + (double)A[1] * f[1] + (double)A[2] * f[2];
        for (int k = 0; k < 3; k++) {
            n[3 * a + k] += f[k];
            n[3 * b + k] += f[k];
            n[3 * c + k] += f[k];
        }
        used++;
    }
    /* Weld: vertices at the same clip position share their sums. */
    WeldKey *K = key_buf;
    for (uint32_t i = 0; i < nv; i++) {
        memcpy(K[i].k, &v[i].x, sizeof K[i].k);
        K[i].i = i;
    }
    qsort(K, nv, sizeof *K, weld_cmp);
    for (uint32_t g = 0; g < nv;) {
        uint32_t e = g + 1;
        while (e < nv && !memcmp(K[e].k, K[g].k, sizeof K[g].k)) e++;
        if (e - g > 1) {
            float s[3] = {0, 0, 0};
            for (uint32_t j = g; j < e; j++)
                for (int k = 0; k < 3; k++) s[k] += n[3 * K[j].i + k];
            for (uint32_t j = g; j < e; j++) memcpy(n + 3 * K[j].i, s, sizeof s);
        }
        g = e;
    }
    const float sign = vol < 0 ? -1.0f : 1.0f;
    for (uint32_t i = 0; i < nv; i++) {
        float *m = n + 3 * i;
        const float l2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
        if (l2 > 1e-30f) {
            const float s = sign / sqrtf(l2);
            m[0] *= s;
            m[1] *= s;
            m[2] *= s;
        } else {
            m[0] = m[1] = 0;
            m[2] = 1;
        }
    }
    return used ? 0 : -1;
}
