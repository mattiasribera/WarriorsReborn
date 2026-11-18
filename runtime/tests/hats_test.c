/* Game-independent test of --hat-fix and --hat-subdiv (src/hats.c): options, hat detection, the
 * smoothed normals of a faceted hat and its PN-triangle subdivision. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/hats.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    HatConfig c = HAT_DEFAULTS;
    CHECK(hat_fix_on(&c), "the fix is on by default, with or without --char-lighting");
    CHECK(hat_subdiv_on(&c), "the subdivision follows the fix by default");
    CHECK(hat_arg(&c, "--hair") == 0, "other options are not consumed");
    CHECK(hat_arg(&c, "--no-hat-fix") == 1 && !hat_fix_on(&c) && !hat_subdiv_on(&c), "--no-hat-fix turns both off");
    CHECK(hat_arg(&c, "--hat-subdiv") == 1 && hat_subdiv_on(&c) && !hat_fix_on(&c), "--hat-subdiv alone");
    CHECK(hat_arg(&c, "--hat-fix") == 1 && hat_fix_on(&c), "--hat-fix");
    CHECK(hat_arg(&c, "--no-hat-subdiv") == 1 && !hat_subdiv_on(&c) && hat_fix_on(&c), "--no-hat-subdiv keeps the fix");
}

static void test_detection(void) {
    const float a[3] = {-0.17f, -0.17f, 0}, b[3] = {0.17f, 0.17f, 0.2f}, big[3] = {0.6f, 0.4f, 0.3f};
    CHECK(hat_is_small(a, b), "a top hat's box is small");
    CHECK(!hat_is_small(a, big), "a crate is not");
    const float heads[2][3] = {{10, 5, 1.8f}, {0, 0, 1.75f}};
    const float on[3] = {0.05f, 0.02f, 1.9f}, ground[3] = {0.3f, 0.1f, 0.05f};
    CHECK(hat_near_head(on, heads, 2, HAT_HEAD_REACH), "a hat on the second head");
    CHECK(!hat_near_head(ground, heads, 2, HAT_HEAD_REACH), "a hat on the ground is not worn");
    CHECK(!hat_near_head(on, heads, 0, HAT_HEAD_REACH), "no head, no hat");
}

/* A faceted top-hat crown: SIDES flat side quads (each with its own 4 vertices and the face's
 * normal), a flat top cap (centre + one vertex per corner, normal up) and a flat bottom cap (normal
 * down): a closed mesh. Vertex layout: position, normal, uv, a colour. */
#define SIDES 8
#define STRIDE 36
typedef struct {
    float p[3], n[3], uv[2];
    unsigned char rgba[4];
} Vert;
static HatMesh crown(void) {
    const uint32_t nv = SIDES * 4 + 2 * (SIDES + 1), ni = SIDES * 6 + 2 * SIDES * 3;
    HatMesh m;
    memset(&m, 0, sizeof m);
    m.nv = nv, m.stride = STRIDE, m.v = calloc(nv, STRIDE), m.idx = calloc(ni, 2), m.ni = ni;
    m.pos_off = 0, m.nrm_off = 12;
    m.nattr = 2;
    m.attr[0] = (HatAttr){24, HAT_ATTR_FLOAT, 2};
    m.attr[1] = (HatAttr){32, HAT_ATTR_BYTES, 4};
    Vert *v = (Vert *)m.v;
    uint32_t k = 0, t = 0;
    for (int s = 0; s < SIDES; s++) {
        const float a0 = 6.2831853f * s / SIDES, a1 = 6.2831853f * ((s + 1) % SIDES) / SIDES, am = 6.2831853f * (s + 0.5f) / SIDES;
        const float c[4][2] = {{cosf(a0), sinf(a0)}, {cosf(a1), sinf(a1)}, {cosf(a1), sinf(a1)}, {cosf(a0), sinf(a0)}};
        const float z[4] = {0, 0, 1, 1};
        for (int j = 0; j < 4; j++) {
            Vert *o = &v[k + j];
            o->p[0] = c[j][0], o->p[1] = c[j][1], o->p[2] = z[j];
            o->n[0] = cosf(am), o->n[1] = sinf(am), o->n[2] = 0;
            o->uv[0] = (float)(s + (j == 1 || j == 2)) / SIDES, o->uv[1] = z[j];
            o->rgba[0] = (unsigned char)(z[j] * 200), o->rgba[1] = 50, o->rgba[2] = 10, o->rgba[3] = 255;
        }
        const uint16_t q[6] = {0, 1, 2, 0, 2, 3};
        for (int j = 0; j < 6; j++) m.idx[t++] = (uint16_t)(k + q[j]);
        k += 4;
    }
    for (int cap = 0; cap < 2; cap++) {   /* top (z 1, up), bottom (z 0, down) */
        const float z = cap ? 0.0f : 1.0f, nz = cap ? -1.0f : 1.0f;
        const uint32_t c0 = k;
        Vert *o = &v[k++];
        o->p[2] = z, o->n[2] = nz, o->uv[0] = 0.5f, o->uv[1] = 0.5f, o->rgba[3] = 255;
        for (int s = 0; s < SIDES; s++) {
            const float a = 6.2831853f * s / SIDES;
            o = &v[k++];
            o->p[0] = cosf(a), o->p[1] = sinf(a), o->p[2] = z, o->n[2] = nz;
            o->uv[0] = 0.5f + 0.5f * cosf(a), o->uv[1] = 0.5f + 0.5f * sinf(a), o->rgba[3] = 255;
        }
        for (int s = 0; s < SIDES; s++) {
            const uint16_t a = (uint16_t)(c0 + 1 + s), b = (uint16_t)(c0 + 1 + (s + 1) % SIDES);
            m.idx[t++] = (uint16_t)c0;
            m.idx[t++] = cap ? b : a;
            m.idx[t++] = cap ? a : b;
        }
    }
    return m;
}
static const float *vp(const HatMesh *m, uint32_t i) { return (const float *)(m->v + (size_t)i * m->stride + m->pos_off); }
static const float *vn(const HatMesh *m, uint32_t i) { return (const float *)(m->v + (size_t)i * m->stride + m->nrm_off); }

/* The largest difference of a Lambert term (light from the side, partly above) between two vertices
 * at the same position of the crown's sides: the brightness step across an edge between faces. */
static float side_step(const HatMesh *m) {
    const float l[3] = {0.8f, 0.36f, 0.48f};
    float worst = 0;
    for (uint32_t i = 0; i < SIDES * 4; i++)
        for (uint32_t j = 0; j < SIDES * 4; j++)
            if (!memcmp(vp(m, i), vp(m, j), 12)) {
                const float *a = vn(m, i), *b = vn(m, j);
                const float d = fabsf(fmaxf(0, a[0] * l[0] + a[1] * l[1] + a[2] * l[2]) - fmaxf(0, b[0] * l[0] + b[1] * l[1] + b[2] * l[2]));
                worst = fmaxf(worst, d);
            }
    return worst;
}

static void test_smooth(void) {
    HatMesh m = crown();
    const float before = side_step(&m);
    CHECK(before > 0.2f, "the faceted crown steps between faces (%.3f)", before);
    CHECK(hat_smooth_normals(&m, HAT_SMOOTH_DEG) == 0, "smoothed");
    const float after = side_step(&m);
    CHECK(after < 1e-4f, "no brightness step between the crown's faces after smoothing (%.5f)", after);
    for (uint32_t i = 0; i < SIDES * 4; i++) {   /* the sides: radial, horizontal, unit */
        const float *p = vp(&m, i), *n = vn(&m, i);
        const float r = sqrtf(p[0] * p[0] + p[1] * p[1]);
        CHECK(fabsf(n[0] * p[0] / r + n[1] * p[1] / r - 1) < 1e-4f && fabsf(n[2]) < 1e-5f, "side vertex %u radial (%.3f %.3f %.3f)", i, n[0], n[1], n[2]);
    }
    for (uint32_t i = SIDES * 4; i < m.nv; i++) {   /* the caps stay flat: their 90-degree rim stays sharp */
        const float *n = vn(&m, i);
        CHECK(fabsf(fabsf(n[2]) - 1) < 1e-5f, "cap vertex %u keeps its normal (%.3f %.3f %.3f)", i, n[0], n[1], n[2]);
    }
    hat_mesh_free(&m);
    HatMesh e;
    memset(&e, 0, sizeof e);
    CHECK(hat_smooth_normals(&e, HAT_SMOOTH_DEG) < 0, "an empty mesh is refused");
}

/* Edges by position: every edge of a closed mesh is used by exactly two triangles. */
static int closed(const HatMesh *m) {
    const uint32_t nt = m->ni / 3;
    int ok = 1;
    for (uint32_t t = 0; t < nt && ok; t++)
        for (int e = 0; e < 3 && ok; e++) {
            const float *a = vp(m, m->idx[3 * t + e]), *b = vp(m, m->idx[3 * t + (e + 1) % 3]);
            int uses = 0;
            for (uint32_t u = 0; u < nt; u++)
                for (int f = 0; f < 3; f++) {
                    const float *c = vp(m, m->idx[3 * u + f]), *d = vp(m, m->idx[3 * u + (f + 1) % 3]);
                    uses += (!memcmp(a, c, 12) && !memcmp(b, d, 12)) || (!memcmp(a, d, 12) && !memcmp(b, c, 12));
                }
            ok = uses == 2;
        }
    return ok;
}
static void test_subdivide(void) {
    HatMesh m = crown();
    CHECK(closed(&m), "the test crown is closed");
    hat_smooth_normals(&m, HAT_SMOOTH_DEG);
    HatMesh s;
    const int sub = hat_subdivide(&m, HAT_SUBDIV_LEVEL, &s);
    CHECK(sub == 0, "subdivided");
    if (sub) {
        hat_mesh_free(&m);
        return;
    }
    const uint32_t nt = m.ni / 3, L = HAT_SUBDIV_LEVEL;
    CHECK(s.ni == m.ni * L * L, "%u triangles from %u", s.ni / 3, nt);
    CHECK(s.nv == nt * (L + 1) * (L + 2) / 2 && s.stride == m.stride && s.nattr == m.nattr, "%u vertices, the same layout", s.nv);
    CHECK(closed(&s), "the subdivided crown is closed (no crack along any edge)");
    /* rounder: the sides' points lie closer to the unit circle than the facets' (worst 0.924 at a
     * facet's middle) */
    float worst_in = 1, worst_out = 1;
    for (uint32_t t = 0; t < SIDES * 2; t++) {   /* the original side triangles' edge middles */
        for (int e = 0; e < 3; e++) {
            const float *a = vp(&m, m.idx[3 * t + e]), *b = vp(&m, m.idx[3 * t + (e + 1) % 3]);
            if (a[2] != b[2]) continue;
            const float x = (a[0] + b[0]) / 2, y = (a[1] + b[1]) / 2;
            worst_in = fminf(worst_in, sqrtf(x * x + y * y));
        }
    }
    for (uint32_t i = 0; i < SIDES * 2 * (L + 1) * (L + 2) / 2; i++) {   /* the side patches' points */
        const float *p = vp(&s, i);
        worst_out = fminf(worst_out, sqrtf(p[0] * p[0] + p[1] * p[1]));
        CHECK(p[2] >= -1e-5f && p[2] <= 1 + 1e-5f, "side point %u stays between the caps (z %.4f)", i, p[2]);
    }
    CHECK(worst_in < 0.93f && worst_out > 0.97f, "rounder: worst radius %.4f -> %.4f", worst_in, worst_out);
    /* the caps stay flat (their normals are perpendicular to them) */
    for (uint32_t i = SIDES * 2 * (L + 1) * (L + 2) / 2; i < s.nv; i++) {
        const float *p = vp(&s, i);
        CHECK(fabsf(p[2]) < 1e-5f || fabsf(p[2] - 1) < 1e-5f, "cap point %u stays flat (z %.5f)", i, p[2]);
    }
    /* corners keep their attributes; a point interpolates its triangle's corners' */
    const Vert *sv = (const Vert *)s.v, *mv = (const Vert *)m.v;
    int corners = 0, attrs_ok = 1, unit = 1;
    for (uint32_t t = 0; t < nt; t++)
        for (int c = 0; c < 3; c++) {
            const Vert *o = &mv[m.idx[3 * t + c]];
            for (uint32_t i = t * (L + 1) * (L + 2) / 2; i < (t + 1) * (L + 1) * (L + 2) / 2; i++)
                if (!memcmp(sv[i].p, o->p, 12)) {
                    corners++;
                    attrs_ok &= !memcmp(sv[i].uv, o->uv, 8) && !memcmp(sv[i].rgba, o->rgba, 4);
                }
        }
    for (uint32_t i = 0; i < s.nv; i++) unit &= fabsf(sqrtf(sv[i].n[0] * sv[i].n[0] + sv[i].n[1] * sv[i].n[1] + sv[i].n[2] * sv[i].n[2]) - 1) < 1e-4f;
    CHECK(corners == (int)nt * 3 && attrs_ok, "corners keep their uv and colour (%d of %u)", corners, nt * 3);
    CHECK(unit, "unit normals");
    /* the middle of the first side quad's lower edge (its first triangle's corners 0 and 1): uv and
     * colour halfway */
    {
        const Vert *a = &mv[m.idx[0]], *b = &mv[m.idx[1]];
        int found = 0;
        for (uint32_t i = 0; i < (L + 1) * (L + 2) / 2; i++) {
            const float u = sv[i].uv[0], ex = a->uv[0] + (b->uv[0] - a->uv[0]) / L;
            if (sv[i].p[2] == 0 && fabsf(u - ex) < 1e-5f) found = 1;
        }
        CHECK(found, "an edge point at a third has the uv a third along");
    }
    hat_mesh_free(&s);
    CHECK(hat_subdivide(&m, 0, &s) < 0 && !s.v && !s.idx, "level 0 is refused");
    hat_mesh_free(&m);
}

int main(void) {
    test_options();
    test_detection();
    test_smooth();
    test_subdivide();
    if (fails) {
        printf("hats_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("hats_test: all passed\n");
    return 0;
}
