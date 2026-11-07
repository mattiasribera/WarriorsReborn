/* --tri-check (or XB_TRI_CHECK=1): an opt-in detector for broken post-transform geometry, the
 * "black triangles" / "stretched triangle out of a head" reports. Every replayed draw's clip
 * positions are checked for non-finite values and for exploded vertices (far outside the draw's
 * own robust bounds); hits are logged with the draw's path, program and mesh hash. Off by
 * default: nothing is checked or printed. Game-independent (tests/tri_check_test.c). */
#ifndef TRI_CHECK_H
#define TRI_CHECK_H
#include <stddef.h>
#include <stdint.h>

/* An exploded vertex lies more than TRI_CHECK_SPREAD times the draw's robust extent outside its
 * robust box (the 10th to 90th percentile of x, y and w over a sample of its vertices). */
#define TRI_CHECK_SPREAD 32.0f
#define TRI_CHECK_MIN_VERTS 12   /* fewer vertices: only the non-finite test */

typedef struct {
    uint32_t nonfinite;    /* vertices with a NaN or infinite x, y, z or w */
    uint32_t outliers;     /* finite vertices outside the robust box by more than the spread */
    uint32_t tris;         /* spike triangles: a non-finite vertex, or from inside the box out to an exploded one */
    uint32_t worst;        /* the worst vertex (first non-finite, else the farthest outlier) */
    float ratio;           /* its distance outside the box in units of the box extent (inf: non-finite) */
    float lo[3], hi[3];    /* the robust box (x, y, w) */
} TriCheckResult;

/* Checks nv vertices (x, y, z, w at xyzw + i * stride floats) and the ni indices into them;
 * returns 1 when it has a spike triangle (r filled either way). */
int tri_check_draw(const float *xyzw, size_t stride, uint32_t nv, const uint32_t *idx, uint32_t ni, TriCheckResult *r);

#endif
