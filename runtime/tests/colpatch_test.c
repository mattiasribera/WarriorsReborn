/* colpatch_test: collision patch files and their insertion into a collision world laid out as the
 * title's (src/colpatch.h), found by a ground query that walks the grid as the title's queries do. */
#include "../src/colpatch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, oks;
#define CHECK(c, ...) do { if (c) oks++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rd32(const uint8_t *w, int off) { uint32_t v; memcpy(&v, w + off, 4); return v; }
static uint16_t rd16(const uint8_t *w, int off) { uint16_t v; memcpy(&v, w + off, 2); return v; }
static void wr32(uint8_t *w, int off, uint32_t v) { memcpy(w + off, &v, 4); }
static void wrf(uint8_t *w, int off, float v) { memcpy(w + off, &v, 4); }
#define P(a) ((uint8_t *)(uintptr_t)(a))

/* A 4 x 4 x 1 grid of 10 m cells over x, y 0..40, z -100..100: one floor quad (0..10, 0..10, z 0) in
 * cell (0, 0), as the level files hold it (list index 0 unused). */
static uint8_t world[COLW_SIZE];
static float verts[4][4] = {{0, 0, 0, 1}, {10, 0, 0, 1}, {10, 10, 0, 1}, {0, 10, 0, 1}};
static uint8_t tris[2 * 10];
static uint32_t offs[16];
static uint16_t lists[4] = {0, 2, 0, 1};
static uint8_t visited[16];
static void make_world(void) {
    memset(world, 0, sizeof world);
    wrf(world, COLW_MATRIX + 0, 0.1f), wrf(world, COLW_MATRIX + 20, 0.1f), wrf(world, COLW_MATRIX + 40, 0.005f);
    wrf(world, COLW_MATRIX + 56, 0.5f), wrf(world, COLW_MATRIX + 60, 1.0f);   /* z translation, w */
    for (int a = 0; a < 3; a++) wrf(world, COLW_GMIN + 4 * a, 0.5f);
    wrf(world, COLW_GMAX, 3.5f), wrf(world, COLW_GMAX + 4, 3.5f), wrf(world, COLW_GMAX + 8, 0.5f);
    uint16_t nx = 4, ny = 4;
    memcpy(world + COLW_NX, &nx, 2), memcpy(world + COLW_NY, &ny, 2);
    wr32(world, COLW_NZ, 1);
    const uint16_t t[2][5] = {{0, 1, 2, 0x21, 5}, {0, 2, 3, 0x21, 5}};
    for (int i = 0; i < 2; i++) {
        memcpy(tris + 10 * i, t[i], 8);
        tris[10 * i + 8] = 5, tris[10 * i + 9] = 0;
    }
    memset(offs, 0, sizeof offs);
    offs[0] = 1;
    lists[0] = 0, lists[1] = 2, lists[2] = 0, lists[3] = 1;
    wr32(world, COLW_OFFSETS, (uint32_t)(uintptr_t)offs);
    wr32(world, COLW_LISTLEN, 4);
    wr32(world, COLW_LISTS, (uint32_t)(uintptr_t)lists);
    uint16_t n = 2;
    memcpy(world + COLW_NTRI, &n, 2);
    wr32(world, COLW_TRIS, (uint32_t)(uintptr_t)tris);
    wr32(world, COLW_VERTS, (uint32_t)(uintptr_t)verts);
    wr32(world, COLW_NVERT, 4);
    wr32(world, COLW_VISITED, (uint32_t)(uintptr_t)visited);
}

/* The title's way: the query point's cell, its list, solid triangles only (bit 0), visited bits.
 * Returns the triangle under (x, y) whose height is the highest at or below z (its height in *hz),
 * -1 when none. */
static int ground(float x, float y, float z, float *hz) {
    const float q[3] = {x, y, z};
    uint32_t c[3];
    colpatch_cell(world, q, c);
    const uint32_t nx = rd16(world, COLW_NX), ny = rd16(world, COLW_NY), ntri = rd16(world, COLW_NTRI);
    memset(P(rd32(world, COLW_VISITED)), 0, ((ntri + 7) / 8 + 15) / 16 * 16);
    const uint32_t o = ((const uint32_t *)P(rd32(world, COLW_OFFSETS)))[(c[2] * ny + c[1]) * nx + c[0]];
    if (!o) return -1;
    const uint16_t *l = (const uint16_t *)P(rd32(world, COLW_LISTS)) + o;
    const uint8_t *rec = P(rd32(world, COLW_TRIS));
    const float *v = (const float *)P(rd32(world, COLW_VERTS));
    uint8_t *bits = P(rd32(world, COLW_VISITED));
    int best = -1;
    for (uint32_t k = 1; k <= l[0]; k++) {
        const uint16_t t = l[k];
        if (t >= ntri || bits[t >> 3] & (1 << (t & 7))) continue;
        bits[t >> 3] |= (uint8_t)(1 << (t & 7));
        const uint8_t *r = rec + 10 * t;
        if (!(rd16(r, 6) & 1)) continue;
        const float *a = v + 4 * rd16(r, 0), *b = v + 4 * rd16(r, 2), *cc = v + 4 * rd16(r, 4);
        const float d = (b[0] - a[0]) * (cc[1] - a[1]) - (cc[0] - a[0]) * (b[1] - a[1]);
        if (fabsf(d) < 1e-6f) continue;   /* vertical */
        const float u = ((x - a[0]) * (cc[1] - a[1]) - (cc[0] - a[0]) * (y - a[1])) / d;
        const float w = ((b[0] - a[0]) * (y - a[1]) - (x - a[0]) * (b[1] - a[1])) / d;
        if (u < -1e-4f || w < -1e-4f || u + w > 1 + 1e-4f) continue;
        const float h = a[2] + u * (b[2] - a[2]) + w * (cc[2] - a[2]);
        if (h <= z + 1e-3f && (best < 0 || h > *hz)) best = t, *hz = h;
    }
    return best;
}
/* Whether a two-sided solid triangle among the cell's list crosses the segment (x0, y0) -> (x1, y1) at
 * height z (a wall test). */
static int wall_between(float x0, float y0, float x1, float y1, float z) {
    const float m[3] = {(x0 + x1) / 2, (y0 + y1) / 2, z};
    uint32_t c[3];
    colpatch_cell(world, m, c);
    const uint32_t nx = rd16(world, COLW_NX), ny = rd16(world, COLW_NY);
    const uint32_t o = ((const uint32_t *)P(rd32(world, COLW_OFFSETS)))[(c[2] * ny + c[1]) * nx + c[0]];
    if (!o) return 0;
    const uint16_t *l = (const uint16_t *)P(rd32(world, COLW_LISTS)) + o;
    const uint8_t *rec = P(rd32(world, COLW_TRIS));
    const float *v = (const float *)P(rd32(world, COLW_VERTS));
    for (uint32_t k = 1; k <= l[0]; k++) {
        const uint8_t *r = rec + 10 * l[k];
        if ((rd16(r, 6) & 3) != 3) continue;
        const float *a = v + 4 * rd16(r, 0), *b = v + 4 * rd16(r, 2), *cc = v + 4 * rd16(r, 4);
        const float zl = fminf(a[2], fminf(b[2], cc[2])), zh = fmaxf(a[2], fmaxf(b[2], cc[2]));
        if (z < zl || z > zh) continue;
        /* the wall's footprint: the segment between its two most distant corners */
        const float *e0 = a, *e1 = b;
        if (hypotf(cc[0] - a[0], cc[1] - a[1]) > hypotf(e1[0] - e0[0], e1[1] - e0[1])) e1 = cc;
        if (hypotf(cc[0] - b[0], cc[1] - b[1]) > hypotf(e1[0] - e0[0], e1[1] - e0[1])) e0 = b, e1 = cc;
        const float s1 = (e1[0] - e0[0]) * (y0 - e0[1]) - (e1[1] - e0[1]) * (x0 - e0[0]);
        const float s2 = (e1[0] - e0[0]) * (y1 - e0[1]) - (e1[1] - e0[1]) * (x1 - e0[0]);
        const float s3 = (x1 - x0) * (e0[1] - y0) - (y1 - y0) * (e0[0] - x0);
        const float s4 = (x1 - x0) * (e1[1] - y0) - (y1 - y0) * (e1[0] - x0);
        if (s1 * s2 < 0 && s3 * s4 < 0) return 1;
    }
    return 0;
}

static void test_parse(void) {
    ColPatch p;
    char err[160];
    const char *ok = "; cage\nlevel = level95   ; the hideout\nmaterial = 5 # concrete\nfloor -174 81 -165 99 -197.57\n"
                     "flags = 0x21\nwall -165 81 -165 86 -197.6 -194.4\nbox 0 0 0 1 1 1\nceiling 0 0 2 2 3\n"
                     "tri 0 0 0 1 0 0 0 1 0\nremove -165.3 81 -198 -164.8 97 -194\n";
    CHECK(!colpatch_parse(ok, strlen(ok), &p, err, sizeof err), "valid file: %s", err);
    CHECK(!strcmp(p.level, "level95"), "level %s", p.level);
    CHECK(p.ntri == 2 + 2 + 10 + 2 + 1, "triangles %u", p.ntri);
    CHECK(p.nremove == 1 && p.remove[0].lo[0] == -165.3f && p.remove[0].hi[2] == -194.0f, "removal box");
    CHECK(p.tri[0].material == 5 && p.tri[0].flags == 0x21, "floor: material %u flags %#x", p.tri[0].material, p.tri[0].flags);
    /* the floor faces up: (b - a) x (c - a) has a positive z */
    const float *a = p.tri[0].p[0], *b = p.tri[0].p[1], *c = p.tri[0].p[2];
    CHECK((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) > 0, "floor winding faces up");
    CHECK(p.tri[2].flags == 0x23, "wall: two-sided (%#x)", p.tri[2].flags);
    CHECK(p.tri[14].flags == 0x23, "ceiling: two-sided (%#x)", p.tri[14].flags);
    a = p.tri[14].p[0], b = p.tri[14].p[1], c = p.tri[14].p[2];
    CHECK((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) < 0, "ceiling winding faces down");
    colpatch_free(&p);
    static const struct { const char *text, *msg; } bad[] = {
        {"floor 0 0 1 1 0\n", "line 1: level = NAME must come first"},
        {"level = Level 9\n", "line 1: level: \"Level 9\" must be a level name like level95"},
        {"level = level95\nfloor 0 0 1 1\n", "line 2: floor needs 5 numbers"},
        {"level = level95\nfloor 0 0 1 1 2 3\n", "line 2: floor needs 5 numbers"},
        {"level = level95\nwall 0 0 0 0 1 2\n", "line 2: wall needs two different ends and Z0 < Z1"},
        {"level = level95\nbox 0 0 0 1 1 0\n", "line 2: box needs X0 < X1, Y0 < Y1, Z0 < Z1"},
        {"level = level95\nmaterial = 300\n", "line 2: material: \"300\" must be a MATERIAL id 1-255"},
        {"level = level95\nstairs 1 2 3\n", "line 2: unknown keyword \"stairs\""},
        {"level = level95\n", "no shapes"},
        {"; nothing\n", "level = NAME is missing"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        err[0] = 0;
        int r = colpatch_parse(bad[i].text, strlen(bad[i].text), &p, err, sizeof err);
        CHECK(r == -1 && !strcmp(err, bad[i].msg), "bad %zu: %d \"%s\" (want \"%s\")", i, r, err, bad[i].msg);
        CHECK(!p.tri && !p.ntri, "bad %zu: nothing kept", i);
    }
}

static void test_apply(void) {
    make_world();
    ColPatch p;
    char err[160];
    /* a floor in cell (2, 2) at z 5, a wall across cells (1, 0)-(1, 1), the level's floor (both its
     * triangles inside the box) made non-solid */
    const char *t = "level = test\nmaterial = 7\nfloor 21 21 29 29 5\nwall 15 2 15 18 0 3\nremove -1 -1 -1 10.5 10.5 1\n";
    CHECK(!colpatch_parse(t, strlen(t), &p, err, sizeof err), "parse: %s", err);
    float hz = 0;
    CHECK(ground(25, 25, 10, &hz) == -1, "before: nothing under (25, 25)");
    CHECK(ground(8, 2, 1, &hz) == 0 && hz == 0, "before: the level's floor under (8, 2)");
    const ColPatch *ps[1] = {&p};
    void *block = NULL;
    uint32_t removed = 0;
    int r = colpatch_apply(world, ps, 1, &block, &removed);
    CHECK(r == 4 && block, "apply: %d triangles added", r);
    CHECK(removed == 2, "apply: %u made non-solid", removed);
    CHECK(rd16(world, COLW_NTRI) == 6 && rd32(world, COLW_NVERT) == 4 + 12, "counts %u %u", rd16(world, COLW_NTRI), rd32(world, COLW_NVERT));
    int g = ground(25, 25, 10, &hz);
    CHECK(g == 2 || g == 3, "after: the new floor under (25, 25) (%d)", g);
    CHECK(fabsf(hz - 5) < 1e-4f, "after: at z 5 (%f)", hz);
    CHECK(ground(25, 25, 4, &hz) == -1, "after: nothing below the new floor's height");
    CHECK(P(rd32(world, COLW_TRIS))[10 * 2 + 8] == 7, "material kept");
    CHECK(ground(8, 2, 1, &hz) == -1 && ground(2, 8, 1, &hz) == -1, "after: the removed triangles are not found");
    CHECK(wall_between(12, 10, 18, 10, 1.5f), "after: the wall blocks (12, 10) -> (18, 10)");
    CHECK(!wall_between(12, 10, 14, 10, 1.5f), "after: not before reaching it");
    CHECK(!wall_between(12, 10, 18, 10, 4), "after: not above it");
    /* the level's own arrays are untouched (only the removed bit) and the lists stay increasing */
    CHECK(verts[2][0] == 10 && lists[1] == 2 && offs[0] == 1 && (tris[6] & 1) == 0 && tris[6 + 10] == 0x20, "old arrays kept (only the solid bit cleared)");
    const uint32_t *o = (const uint32_t *)P(rd32(world, COLW_OFFSETS));
    const uint16_t *l = (const uint16_t *)P(rd32(world, COLW_LISTS));
    int sorted = 1, entries = 0;
    for (int c = 0; c < 16; c++)
        if (o[c]) for (int k = 2; k <= l[o[c]]; k++) sorted &= l[o[c] + k] > l[o[c] + k - 1];
    for (int c = 0; c < 16; c++) entries += o[c] ? l[o[c]] : 0;
    CHECK(sorted, "lists increasing");
    CHECK(entries == 2 + 2 + 2 * 2, "list entries %d", entries);   /* floor: 1 cell; wall: 2 cells */
    CHECK(rd32(world, COLW_LISTLEN) == 1 + 3 + 3 + 3 + 3, "list length %u", rd32(world, COLW_LISTLEN));
    CHECK(((uintptr_t)P(rd32(world, COLW_VISITED)) & 15) == 0, "visited bits aligned");
    free(block);
    colpatch_free(&p);
}

static void test_limits(void) {
    make_world();
    ColPatch p;
    char err[160];
    const char *t = "level = test\nremove 9 9 -1 11 11 1\n";   /* no triangle has all corners inside */
    CHECK(!colpatch_parse(t, strlen(t), &p, err, sizeof err), "parse: %s", err);
    const ColPatch *ps[1] = {&p};
    void *block = (void *)1;
    uint32_t removed = 9;
    CHECK(colpatch_apply(world, ps, 1, &block, &removed) == 0 && !block && removed == 0, "removal only: nothing allocated");
    CHECK(rd32(world, COLW_TRIS) == (uint32_t)(uintptr_t)tris, "world unchanged");
    colpatch_free(&p);
    /* a geometry far outside the grid lands in the edge cells the queries clamp to */
    make_world();
    t = "level = test\nfloor 100 100 110 110 0\n";
    CHECK(!colpatch_parse(t, strlen(t), &p, err, sizeof err), "parse: %s", err);
    CHECK(colpatch_apply(world, ps, 1, &block, &removed) == 2, "outside the grid: added");
    float hz;
    int g = ground(105, 105, 1, &hz);
    CHECK(g == 2 || g == 3, "outside the grid: found through the clamped cell (%d)", g);
    free(block);
    colpatch_free(&p);
    /* too many triangles for the u16 count */
    make_world();
    uint16_t big = 0xfffe;
    memcpy(world + COLW_NTRI, &big, 2);
    t = "level = test\nfloor 0 0 1 1 0\n";
    CHECK(!colpatch_parse(t, strlen(t), &p, err, sizeof err), "parse: %s", err);
    CHECK(colpatch_apply(world, ps, 1, &block, &removed) == -1 && rd32(world, COLW_TRIS) == (uint32_t)(uintptr_t)tris, "over 65535: refused, unchanged");
    colpatch_free(&p);
}

static void test_register(void) {
    colpatch_reset();
    CHECK(colpatch_register("m", "level = level95\nfloor 0 0 1 1 0\n", 32) == 0 && colpatch_count() == 1, "registered");
    CHECK(colpatch_register("m2", "floor 0 0 1 1 0\n", 16) == -1 && colpatch_count() == 1, "bad file not registered");
    CHECK(colpatch_base_ntri() == 0, "nothing patched yet");
    colpatch_reset();
    CHECK(colpatch_count() == 0, "reset");
}

int main(void) {
    test_parse();
    test_apply();
    test_limits();
    test_register();
    printf("colpatch_test: %d ok, %d failed\n%s\n", oks, fails, fails ? "FAILED" : "ALL OK");
    return fails != 0;
}
