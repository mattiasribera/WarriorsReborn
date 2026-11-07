/* Game-independent test of the --tri-check detector (src/tri_check.c): a clean grid of clip
 * positions is not flagged, one exploded vertex or one NaN is, with the triangles that use it, and
 * a draw too small for robust bounds only gets the non-finite test. */
#include "../src/tri_check.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

enum { GW = 20, GH = 30, NV = GW * GH };
typedef struct { float x, y, z, w, pad[3]; } V;   /* a stride of 7 floats, as RVertex has more fields */
static V v[NV];
static uint32_t idx[(GW - 1) * (GH - 1) * 6];

static uint32_t grid(void) {   /* a 0.5 x 1.8 "character" 5 units in front of the camera */
    uint32_t n = 0;
    for (int j = 0; j < GH; j++)
        for (int i = 0; i < GW; i++) {
            V *p = &v[j * GW + i];
            memset(p, 0, sizeof *p);
            p->x = -0.25f + 0.5f * i / (GW - 1);
            p->y = -0.9f + 1.8f * j / (GH - 1);
            p->w = 5 + 0.1f * sinf((float)i);
            p->z = p->w - 0.1f;
        }
    for (int j = 0; j + 1 < GH; j++)
        for (int i = 0; i + 1 < GW; i++) {
            const uint32_t a = j * GW + i;
            const uint32_t q[6] = {a, a + 1, a + GW, a + 1, a + GW + 1, a + GW};
            memcpy(idx + n, q, sizeof q);
            n += 6;
        }
    return n;
}

int main(void) {
    TriCheckResult r;
    const size_t st = sizeof(V) / sizeof(float);
    uint32_t ni = grid();
    CHECK(!tri_check_draw(&v[0].x, st, NV, idx, ni, &r) && !r.nonfinite && !r.outliers && !r.tris, "a clean draw is not flagged");

    const uint32_t mid = 15 * GW + 10;
    v[mid].y = 400;   /* an exploded vertex: a long spike up out of the mesh */
    CHECK(tri_check_draw(&v[0].x, st, NV, idx, ni, &r) && r.outliers == 1 && !r.nonfinite && r.worst == mid && r.tris == 6,
          "one exploded vertex: 1 outlier at %u (want %u), %u triangles (want 6), ratio %.1f", r.worst, mid, r.tris, r.ratio);

    grid();
    v[mid].y = 2.5f;   /* an arm raised to twice the height: still inside the spread */
    CHECK(!tri_check_draw(&v[0].x, st, NV, idx, ni, &r), "a vertex just outside the box (ratio %.2f) is not flagged", r.ratio);

    grid();
    v[7].w = NAN;
    v[8].x = INFINITY;
    CHECK(tri_check_draw(&v[0].x, st, NV, idx, ni, &r) && r.nonfinite == 2 && r.worst == 7 && isinf(r.ratio) && r.tris > 0,
          "NaN and infinite positions: %u non-finite (want 2), worst %u, %u triangles", r.nonfinite, r.worst, r.tris);

    grid();
    for (int k = 0; k < 4; k++) v[(const int[]){0, 1, GW, GW + 1}[k]].y += 900;   /* the first quad far away (a distant particle of a batch) */
    CHECK(!tri_check_draw(&v[0].x, st, NV, idx, 6, &r) && r.outliers == 4, "a quad whose vertices are all far away is no spike (%u outliers)",
          r.outliers);

    grid();
    v[2].x = 1e6f;   /* 4 vertices: no robust bounds, only the non-finite test */
    CHECK(!tri_check_draw(&v[0].x, st, 4, idx, 6, &r), "a tiny draw gets no outlier test");
    v[1].y = NAN;
    CHECK(tri_check_draw(&v[0].x, st, 4, idx, 6, &r) && r.nonfinite == 1, "a tiny draw with a NaN is flagged");

    grid();
    for (int i = 0; i < NV; i++) v[i].w = 5;   /* flat in w (a billboard facing the camera) */
    CHECK(!tri_check_draw(&v[0].x, st, NV, idx, ni, &r), "a draw flat in w is not flagged");
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
