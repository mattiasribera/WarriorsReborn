/* Game-independent test of the --char-lighting option and the smooth character normals
 * (src/char_light.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/char_light.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A view-space point (right-handed, in front: z < 0) as a clip-space vertex with camera scale px, py. */
static RVertex vert(float x, float y, float z, float px, float py) {
    RVertex r;
    memset(&r, 0, sizeof r);
    r.x = x * px;
    r.y = y * py;
    r.w = -z;
    r.z = 0.5f * r.w;
    return r;
}

static void test_options(void) {
    CharLightConfig c = CHAR_LIGHT_DEFAULTS;
    CHECK(!c.on, "off by default");
    CHECK(char_light_arg(&c, "--lamp-lights") == 0 && !c.on, "other options are not consumed");
    CHECK(char_light_arg(&c, "--char-lighting") == 1 && c.on, "--char-lighting turns it on");
    CHECK(char_light_arg(&c, "--no-char-lighting") == 1 && !c.on, "--no-char-lighting turns it off");
}

/* An octahedron centred at view (0, 0, -5), radius 1: its 6 corners shared by 8 faces. */
static const float OCTA[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
static const uint32_t OCTA_IDX[24] = {0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 4, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 5};

static void check_outward(const float *n, const float (*p)[3], uint32_t nv, const char *what) {
    for (uint32_t i = 0; i < nv; i++) {
        const float len = sqrtf(n[3 * i] * n[3 * i] + n[3 * i + 1] * n[3 * i + 1] + n[3 * i + 2] * n[3 * i + 2]);
        const float d = n[3 * i] * p[i][0] + n[3 * i + 1] * p[i][1] + n[3 * i + 2] * p[i][2];
        CHECK(fabsf(len - 1) < 1e-4f, "%s: vertex %u normal unit (%.4f)", what, i, len);
        CHECK(d > 0.99f, "%s: vertex %u normal is the corner direction (dot %.4f)", what, i, d);
    }
}

static void test_octahedron(void) {
    const float px = 1.7f, py = 2.3f;
    RVertex v[6];
    for (int i = 0; i < 6; i++) v[i] = vert(OCTA[i][0], OCTA[i][1], OCTA[i][2] - 5, px, py);
    float n[18];
    CHECK(char_light_normals(v, 6, OCTA_IDX, 24, px, py, n) == 0, "octahedron: usable");
    check_outward(n, OCTA, 6, "octahedron");
    /* The opposite winding: still outward. */
    uint32_t rev[24];
    for (int t = 0; t < 8; t++) {
        rev[3 * t] = OCTA_IDX[3 * t];
        rev[3 * t + 1] = OCTA_IDX[3 * t + 2];
        rev[3 * t + 2] = OCTA_IDX[3 * t + 1];
    }
    CHECK(char_light_normals(v, 6, rev, 24, px, py, n) == 0, "reversed: usable");
    check_outward(n, OCTA, 6, "reversed winding");
}

/* The same octahedron with every face's corners duplicated (24 vertices: a seam everywhere). */
static void test_welded(void) {
    const float px = 1.0f, py = 1.0f;
    RVertex v[24];
    uint32_t idx[24];
    float p[24][3];
    for (int i = 0; i < 24; i++) {
        memcpy(p[i], OCTA[OCTA_IDX[i]], sizeof p[i]);
        v[i] = vert(p[i][0], p[i][1], p[i][2] - 5, px, py);
        idx[i] = (uint32_t)i;
        v[i].tex[0][0] = (float)i;   /* different UVs: still the same position */
    }
    float n[72];
    CHECK(char_light_normals(v, 24, idx, 24, px, py, n) == 0, "split: usable");
    check_outward(n, (const float (*)[3])p, 24, "split vertices welded");
}

static void test_degenerate(void) {
    RVertex v[4];
    for (int i = 0; i < 4; i++) v[i] = vert(0, 0, -3, 1, 1);   /* all at one point */
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 9};                 /* the second has an index out of range */
    float n[12];
    memset(n, 0x7f, sizeof n);
    CHECK(char_light_normals(v, 4, idx, 6, 1, 1, n) == -1, "nothing usable: -1");
    for (int i = 0; i < 4; i++) CHECK(n[3 * i] == 0 && n[3 * i + 1] == 0 && n[3 * i + 2] == 1, "vertex %d faces the camera", i);
    /* A lone triangle plus an unused vertex: the triangle faces the camera (an open mesh's volume
     * relative to the camera), the unused vertex gets the fallback. */
    RVertex t[4] = {vert(0, 0, -3, 1, 1), vert(1, 0, -3, 1, 1), vert(0, 1, -3, 1, 1), vert(5, 5, -3, 1, 1)};
    const uint32_t ti[3] = {0, 1, 2};
    CHECK(char_light_normals(t, 4, ti, 3, 1, 1, n) == 0, "triangle: usable");
    for (int i = 0; i < 3; i++) CHECK(fabsf(fabsf(n[3 * i + 2]) - 1) < 1e-5f, "triangle vertex %d normal along z (%.3f)", i, n[3 * i + 2]);
    CHECK(n[9] == 0 && n[10] == 0 && n[11] == 1, "unused vertex faces the camera");
    /* Behind the camera: skipped. */
    RVertex b[3] = {vert(0, 0, -3, 1, 1), vert(1, 0, -3, 1, 1), vert(0, 1, -3, 1, 1)};
    b[2].w = -1;
    CHECK(char_light_normals(b, 3, ti, 3, 1, 1, n) == -1, "a vertex behind the camera: skipped");
    CHECK(char_light_normals(b, 0, NULL, 0, 1, 1, n) == -1, "empty: -1");
}

/* A ring of 16 quads (a cylinder around the view axis... sideways: axis y) seen from the camera:
 * the normals point away from the axis, smoothly (neighbouring vertices differ by one step). */
static void test_cylinder(void) {
    enum { S = 16 };
    RVertex v[2 * S];
    float p[2 * S][3];
    uint32_t idx[6 * S];
    for (int i = 0; i < S; i++) {
        const float a = 6.2831853f * i / S;
        for (int k = 0; k < 2; k++) {
            p[2 * i + k][0] = cosf(a);
            p[2 * i + k][1] = k ? 1.0f : -1.0f;
            p[2 * i + k][2] = sinf(a);
            v[2 * i + k] = vert(p[2 * i + k][0], p[2 * i + k][1], p[2 * i + k][2] - 6, 1.2f, 1.2f);
        }
        const uint32_t a0 = 2 * i, a1 = 2 * i + 1, b0 = 2 * ((i + 1) % S), b1 = b0 + 1;
        const uint32_t q[6] = {a0, b0, a1, a1, b0, b1};
        memcpy(idx + 6 * i, q, sizeof q);
    }
    float n[6 * S];
    CHECK(char_light_normals(v, 2 * S, idx, 6 * S, 1.2f, 1.2f, n) == 0, "cylinder: usable");
    for (int i = 0; i < 2 * S; i++) {
        const float r = n[3 * i] * p[i][0] + n[3 * i + 2] * p[i][2];
        CHECK(r > 0.99f && fabsf(n[3 * i + 1]) < 1e-4f, "cylinder vertex %d: radial (%.4f, y %.4f)", i, r, n[3 * i + 1]);
    }
}

int main(void) {
    test_options();
    test_octahedron();
    test_welded();
    test_degenerate();
    test_cylinder();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
