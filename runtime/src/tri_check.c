/* --tri-check: non-finite and exploded post-transform vertices (tri_check.h). */
#include "tri_check.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES 255

static int cmp_float(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}
static int finite4(const float *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) && isfinite(v[3]); }

int tri_check_draw(const float *xyzw, size_t stride, uint32_t nv, const uint32_t *idx, uint32_t ni, TriCheckResult *r) {
    static const int axis[3] = {0, 1, 3};   /* x, y, w: z is a function of w for a perspective projection */
    memset(r, 0, sizeof *r);
    if (!nv) return 0;
    /* The robust box from an even sample of the finite vertices. */
    float s[3][SAMPLES];
    uint32_t ns = 0;
    const uint32_t step = nv > SAMPLES ? (nv + SAMPLES - 1) / SAMPLES : 1;
    for (uint32_t i = 0; i < nv && ns < SAMPLES; i += step) {
        const float *v = xyzw + (size_t)i * stride;
        if (!finite4(v)) continue;
        for (int a = 0; a < 3; a++) s[a][ns] = v[axis[a]];
        ns++;
    }
    float span = 0;
    const int robust = nv >= TRI_CHECK_MIN_VERTS && ns >= TRI_CHECK_MIN_VERTS / 2;
    if (robust) {
        for (int a = 0; a < 3; a++) {
            qsort(s[a], ns, sizeof(float), cmp_float);
            r->lo[a] = s[a][ns / 10];
            r->hi[a] = s[a][ns - 1 - ns / 10];
            if (r->hi[a] - r->lo[a] > span) span = r->hi[a] - r->lo[a];
        }
        /* A flat or tiny draw: at least a thousandth of its distance, so rounding is never flagged. */
        const float wmid = fabsf(r->lo[2] + r->hi[2]) * 0.5f;
        if (span < wmid * 1e-3f) span = wmid * 1e-3f;
        if (span < 1e-6f) span = 1e-6f;
    }
    /* Per vertex: 0 fine, 1 flagged. */
    uint8_t stackbuf[4096], *bad = nv <= sizeof stackbuf ? stackbuf : malloc(nv);
    if (!bad) return 0;
    for (uint32_t i = 0; i < nv; i++) {
        const float *v = xyzw + (size_t)i * stride;
        bad[i] = 0;
        if (!finite4(v)) {
            if (!r->nonfinite++) {
                r->worst = i;
                r->ratio = INFINITY;
            }
            bad[i] = 1;
            continue;
        }
        if (!robust) continue;
        float d = 0;
        for (int a = 0; a < 3; a++) {
            const float c = v[axis[a]];
            const float o = c < r->lo[a] ? r->lo[a] - c : c > r->hi[a] ? c - r->hi[a] : 0;
            if (o > d) d = o;
        }
        const float ratio = d / span;
        if (ratio > TRI_CHECK_SPREAD) {
            bad[i] = 1;
            r->outliers++;
            if (!r->nonfinite && ratio > r->ratio) {
                r->ratio = ratio;
                r->worst = i;
            }
        }
    }
    /* A spike: a triangle from inside the box out to an exploded or non-finite vertex. A triangle with
     * every vertex far away (a distant particle quad of a batch) is not one. */
    for (uint32_t t = 0; t + 2 < ni && (r->nonfinite || r->outliers); t += 3) {
        const uint32_t q[3] = {idx[t], idx[t + 1], idx[t + 2]};
        int nbad = 0, nonfinite = 0;
        for (int k = 0; k < 3; k++) {
            if (q[k] >= nv) continue;
            nbad += bad[q[k]];
            nonfinite |= !finite4(xyzw + (size_t)q[k] * stride);
        }
        r->tris += nonfinite || (nbad && nbad < 3);
    }
    if (bad != stackbuf) free(bad);
    return r->tris != 0;
}
