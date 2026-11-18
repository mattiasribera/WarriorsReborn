/* Game-independent test of --materials: options, the collision mesh in a level file, the top-down
 * material map and the WAD lookup (src/materials.c), on synthetic data only. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/materials.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_options(void) {
    MaterialsConfig c = MATERIALS_DEFAULTS;
    CHECK(!c.on && c.strength == MATERIALS_STRENGTH_DEFAULT, "defaults: off, strength %.2f", c.strength);
    CHECK(materials_arg(&c, "--rain-hd", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(materials_arg(&c, "--materials", NULL) == 1 && c.on, "--materials turns it on");
    CHECK(materials_arg(&c, "--no-materials", NULL) == 1 && !c.on, "--no-materials turns it off");
    CHECK(materials_arg(&c, "--materials-strength", "0.5") == 2 && c.strength == 0.5f && !c.on, "strength sets the strength only");
    CHECK(materials_arg(&c, "--materials-strength", "0.1") == 2 && materials_arg(&c, "--materials-strength", "2") == 2 && c.strength == 2.0f,
          "strength range 0.1-2");
    const char *bad[] = {"0", "-1", "2.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        MaterialsConfig d = MATERIALS_DEFAULTS;
        CHECK(materials_arg(&d, "--materials-strength", bad[i]) == -1 && d.strength == MATERIALS_STRENGTH_DEFAULT, "strength '%s' is rejected",
              bad[i] ? bad[i] : "(missing)");
    }
}

static void test_classes(void) {
    static const struct { int id, cls; } want[] = {
        {6, MAT_ASPHALT}, {105, MAT_ASPHALT}, {5, MAT_CONCRETE}, {36, MAT_DIRT}, {106, MAT_DIRT}, {24, MAT_DIRT}, {35, MAT_GRASS}, {114, MAT_GRASS},
        {16, MAT_WOOD}, {179, MAT_WOOD}, {3, MAT_METAL}, {12, MAT_METAL}, {13, MAT_METAL}, {18, MAT_METAL}, {116, MAT_BRICK}, {15, MAT_BRICK},
        {2, MAT_GLASS}, {0, MAT_UNKNOWN}, {1, MAT_UNKNOWN}, {95, MAT_UNKNOWN}, {190, MAT_UNKNOWN}, {255, MAT_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof want / sizeof *want; i++)
        CHECK(mat_class((uint8_t)want[i].id) == want[i].cls, "material %d -> class %d (%s), got %d", want[i].id, want[i].cls, mat_class_name(want[i].cls),
              mat_class((uint8_t)want[i].id));
    CHECK(!strcmp(mat_class_name(MAT_ASPHALT), "asphalt") && !strcmp(mat_class_name(MAT_GRASS), "grass") && !strcmp(mat_class_name(99), "unknown"),
          "class names");
}

/* ---- A synthetic collision mesh ---- */
typedef struct {
    float v[64][4];
    uint8_t t[64][10];
    int nv, nt;
} Mesh;
static int vert(Mesh *m, float x, float y, float z) {
    m->v[m->nv][0] = x, m->v[m->nv][1] = y, m->v[m->nv][2] = z, m->v[m->nv][3] = 1;
    return m->nv++;
}
static void tri(Mesh *m, int a, int b, int c, int mat) {
    uint8_t *t = m->t[m->nt++];
    memset(t, 0, 10);
    t[0] = (uint8_t)a, t[2] = (uint8_t)b, t[4] = (uint8_t)c, t[8] = (uint8_t)mat;
}
/* An axis-aligned quad facing up (counter-clockwise from above) at height z. */
static void quad(Mesh *m, float x0, float y0, float x1, float y1, float z, int mat) {
    int a = vert(m, x0, y0, z), b = vert(m, x1, y0, z), c = vert(m, x1, y1, z), d = vert(m, x0, y1, z);
    tri(m, a, b, c, mat);
    tri(m, a, c, d, mat);
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
/* A level file: a pack with a first item without collision, then the collision item (chunks 3, 4, 7). */
static size_t level_file(const Mesh *m, uint8_t *out) {
    const uint32_t ts = (uint32_t)m->nt * 10, vs = (uint32_t)m->nv * 16;
    uint8_t *p = out + 16;
    put32(p, 1), put32(p + 4, 8), put32(p + 8, 0), put32(p + 12, 0x1111);   /* item 1: one 8-byte chunk of class 0x2a */
    put32(p + 16, 0x2a), put32(p + 20, 8), put32(p + 24, 0), put32(p + 28, 0x2222);
    memset(p + 32, 0xab, 8);
    p += 40;
    put32(p, 3), put32(p + 4, 4 + ts + vs), put32(p + 8, 0), put32(p + 12, 0x3333);   /* item 2: the collision */
    p += 16;
    put32(p, 3), put32(p + 4, 4), put32(p + 8, 0), put32(p + 12, 1);
    put32(p + 16, 0x5a5a5a5a);
    p += 20;
    put32(p, 4), put32(p + 4, ts), put32(p + 8, 0), put32(p + 12, 2);
    memcpy(p + 16, m->t, ts);
    p += 16 + ts;
    put32(p, 7), put32(p + 4, vs), put32(p + 8, 0), put32(p + 12, 3);
    memcpy(p + 16, m->v, vs);
    p += 16 + vs;
    const size_t n = (size_t)(p - out);
    put32(out, 2), put32(out + 4, (uint32_t)(n - 16)), put32(out + 8, 0), put32(out + 12, 0xDE686795u);
    return n;
}

/* The test level: asphalt street 0..10 x 0..10 at z 0; grass 10..20 x 0..10 at z 0.3; a metal roof
 * 2..6 x 2..6 at z 5 over the street; a brick wall (vertical, left out); a camera ghost slab high
 * above everything (left out); a small dirt triangle inside one cell; a triangle with a bad index. */
static void test_mesh(Mesh *m) {
    memset(m, 0, sizeof *m);
    quad(m, 0, 0, 10, 10, 0, 6);
    quad(m, 10, 0, 20, 10, 0.3f, 35);
    quad(m, 2, 2, 6, 6, 5, 3);
    int a = vert(m, 0, 10, 0), b = vert(m, 10, 10, 0), c = vert(m, 10, 10, 4);
    tri(m, a, b, c, 116);
    quad(m, -50, -50, 50, 50, 30, 95);
    a = vert(m, 15.1f, 8.1f, 1), b = vert(m, 15.3f, 8.1f, 1), c = vert(m, 15.1f, 8.3f, 1);
    tri(m, a, b, c, 36);
    tri(m, 0, 1, 200, 6);
}

static void test_parse_and_map(void) {
    static Mesh m;
    static uint8_t file[8192];
    test_mesh(&m);
    const size_t n = level_file(&m, file);
    MatMesh mm;
    CHECK(mat_mesh_find(file, n, &mm) == 0 && mm.ntri == (uint32_t)m.nt && mm.nvert == (uint32_t)m.nv, "mesh found: %u tris, %u verts", mm.ntri, mm.nvert);
    CHECK(mat_mesh_find(file, n - 5, &mm) == -1, "a truncated file is rejected");
    CHECK(mat_mesh_find(file + 16 + 40, n - 56, &mm) == 0 && mm.ntri == (uint32_t)m.nt, "a bare collision item is found");
    uint8_t junk[64] = {1, 2, 3};
    CHECK(mat_mesh_find(junk, sizeof junk, &mm) == -1 && mat_mesh_find(NULL, 0, &mm) == -1, "junk is rejected");

    mat_mesh_find(file, n, &mm);
    MatMap map;
    CHECK(mat_map_build(&mm, &map) == 0 && map.texels, "map built");
    CHECK(map.cell == MAT_CELL && map.x0 <= 0 && map.x0 > -1 && map.y0 <= 0 && map.y0 > -1 && map.w >= 40 && map.w <= 42 && map.h >= 20 && map.h <= 22,
          "bounds from the upward triangles without the ghost: x0 %.2f y0 %.2f %dx%d cell %.2f", map.x0, map.y0, map.w, map.h, map.cell);
    CHECK(map.surfaces == 7, "7 upward triangles (2 street, 2 grass, 2 roof, 1 dirt), got %u", map.surfaces);
    float hz = -1;
    CHECK(mat_map_at(&map, 8, 8, 0.1f, 1, &hz) == MAT_ASPHALT && fabsf(hz) < 0.1f, "street: asphalt at z 0 (%.2f)", hz);
    CHECK(mat_map_at(&map, 15, 5, 0.2f, 1, &hz) == MAT_GRASS && fabsf(hz - 0.3f) < 0.1f, "grass at z 0.3 (%.2f)", hz);
    CHECK(mat_map_at(&map, 4, 4, 0.0f, 1, &hz) == MAT_ASPHALT && fabsf(hz) < 0.1f, "under the roof: the street (second layer)");
    CHECK(mat_map_at(&map, 4, 4, 5.2f, 1, &hz) == MAT_METAL && fabsf(hz - 5) < 0.1f, "on the roof: metal");
    CHECK(mat_map_at(&map, 4, 4, 2.5f, 1, NULL) == MAT_UNKNOWN, "between the layers: unknown");
    CHECK(mat_map_at(&map, 8, 8, 3.0f, 1, NULL) == MAT_UNKNOWN, "far above the street: unknown");
    CHECK(mat_map_at(&map, 15.17f, 8.17f, 1, 0.5f, NULL) == MAT_DIRT, "the small dirt triangle has its cell");
    CHECK(mat_map_at(&map, -5, 5, 0, 1, NULL) == MAT_UNKNOWN && mat_map_at(&map, 25, 5, 0, 1, NULL) == MAT_UNKNOWN &&
              mat_map_at(&map, 5, 40, 0, 1, NULL) == MAT_UNKNOWN,
          "outside the map: unknown");
    int ghost = 0;
    for (int i = 0; i < map.w * map.h * 2; i++) ghost |= (map.texels[i] & 0xfff) && map.z0 + ((map.texels[i] & 0xfff) - 1) * map.zstep > 20;
    CHECK(!ghost, "the camera ghost is not in the map");
    mat_map_free(&map);
    CHECK(!map.texels && !map.w, "freed");

    /* No upward triangle: no map. */
    static Mesh w;
    memset(&w, 0, sizeof w);
    int a = vert(&w, 0, 0, 0), b = vert(&w, 1, 0, 0), c = vert(&w, 1, 0, 3);
    tri(&w, a, b, c, 116);
    const size_t wn = level_file(&w, file);
    mat_mesh_find(file, wn, &mm);
    CHECK(mat_map_build(&mm, &map) == -1 && !map.texels, "walls only: no map");

    /* A huge level: the cell grows so a side stays within MAT_MAP_MAX. */
    memset(&w, 0, sizeof w);
    quad(&w, 0, 0, 3000, 10, 0, 6);
    const size_t hn = level_file(&w, file);
    mat_mesh_find(file, hn, &mm);
    CHECK(mat_map_build(&mm, &map) == 0 && map.w <= MAT_MAP_MAX && map.cell > MAT_CELL && mat_map_at(&map, 2999, 5, 0, 1, NULL) == MAT_ASPHALT,
          "a 3 km level: %d texels wide, cell %.2f", map.w, map.cell);
    mat_map_free(&map);
}

/* A vertical quad from (x0, y0) to (x1, y1) on the ground plane, z from z0 to z1. */
static void wall(Mesh *m, float x0, float y0, float x1, float y1, float z0, float z1, int mat) {
    int a = vert(m, x0, y0, z0), b = vert(m, x1, y1, z0), c = vert(m, x1, y1, z1), d = vert(m, x0, y0, z1);
    tri(m, a, b, c, mat);
    tri(m, a, c, d, mat);
}

/* --lamp-shadows walls: the wall map of a synthetic level. The floor
 * faces up (not a wall); a brick wall x = 5, y 0..4, z 0..3; beside it a closed door's panel with
 * material 0 (the title's door panels: SpawnDoor's static triangles), y 4..6, z 0..2.3; a camera
 * ghost x = 8 and a chain-link fence x = 2 (left out); a ceiling facing down at z 3 over 6..8 x 6..8;
 * a gable x = 9, y 0..4, its apex 4 up at y 2 (per cell its own height, not the triangle's). */
static void test_wall_map(void) {
    static Mesh m;
    static uint8_t file[8192];
    memset(&m, 0, sizeof m);
    quad(&m, 0, 0, 10, 10, 0, 6);
    wall(&m, 5, 0, 5, 4, 0, 3, 116);
    wall(&m, 5, 4, 5, 6, 0, 2.3f, 0);
    wall(&m, 8, 0, 8, 10, 0, 5, MAT_ID_CAMERA_GHOST);
    wall(&m, 2, 0, 2, 10, 0, 2, 107);
    int a = vert(&m, 6, 6, 3), b = vert(&m, 8, 6, 3), c = vert(&m, 8, 8, 3), d = vert(&m, 6, 8, 3);
    tri(&m, a, c, b, 1), tri(&m, a, d, c, 1);   /* clockwise from above: facing down */
    a = vert(&m, 9, 0, 0), b = vert(&m, 9, 4, 0), c = vert(&m, 9, 2, 4);
    tri(&m, a, b, c, 116);
    const size_t n = level_file(&m, file);
    MatMesh mm;
    CHECK(mat_mesh_find(file, n, &mm) == 0, "wall mesh found");
    MatWallMap w;
    CHECK(mat_wall_map_build(&mm, &w) == 0 && w.texels, "wall map built");
    CHECK(w.cell == MAT_WALL_CELL && w.walls == 7, "cell %.2f, 7 wall triangles (brick 2, door 2, ceiling 2, gable 1), got %u", w.cell, w.walls);
    CHECK(w.x0 < 5 && w.x0 > 4.5f && w.y0 < 0 && w.y0 > -0.5f && w.x0 + w.w * w.cell >= 9 && w.y0 + w.h * w.cell >= 8,
          "bounds from the walls only, one cell of margin: x0 %.2f y0 %.2f %dx%d", w.x0, w.y0, w.w, w.h);
    CHECK(mat_wall_at(&w, 5, 2, 1.5f) && mat_wall_at(&w, 5.1f, 2, 0.1f) && mat_wall_at(&w, 4.95f, 2, 2.9f), "the brick wall blocks up to 3 (%d %d %d)", mat_wall_at(&w, 5, 2, 1.5f), mat_wall_at(&w, 5.1f, 2, 0.1f), mat_wall_at(&w, 4.95f, 2, 2.9f));
    CHECK(!mat_wall_at(&w, 5, 2, 3.2f), "above the brick wall: open");
    CHECK(mat_wall_at(&w, 5, 5, 1) && !mat_wall_at(&w, 5, 5, 2.5f), "the material-0 door panel blocks up to 2.3, open above");
    CHECK(!mat_wall_at(&w, 8, 5, 1), "the camera ghost is no wall");
    CHECK(!mat_wall_at(&w, 2, 5, 1), "the chain-link fence is no wall (see-through)");
    CHECK(!mat_wall_at(&w, 6.5f, 2, 1) && !mat_wall_at(&w, 3, 5, 0), "open ground: no wall (the floor faces up)");
    CHECK(mat_wall_at(&w, 7, 7, 3) && !mat_wall_at(&w, 7, 7, 1.5f), "the ceiling: a wall at its height only");
    CHECK(mat_wall_at(&w, 9, 2, 3.9f) && mat_wall_at(&w, 9, 0.1f, 0.3f) && !mat_wall_at(&w, 9, 0.1f, 1), "the gable: each cell its own height");
    CHECK(!mat_wall_at(&w, -5, 2, 1) && !mat_wall_at(&w, 5, 40, 1) && !mat_wall_at(NULL, 5, 2, 1), "outside the map: no wall");
    /* --audio-occlusion: walls crossed by a segment, in a 2D march over
     * the map's cells with the segment's height in each cell. */
    {
        const float skip = 0.5f;
        int k1 = mat_wall_crossings(&w, (const float[3]){3, 2, 1.5f}, (const float[3]){7, 2, 1.5f}, skip);
        int k2 = mat_wall_crossings(&w, (const float[3]){3, 2, 1}, (const float[3]){9.8f, 2, 1}, skip);
        int k3 = mat_wall_crossings(&w, (const float[3]){7, 2, 1.5f}, (const float[3]){3, 2, 1.5f}, skip);
        int k4 = mat_wall_crossings(&w, (const float[3]){3, 1, 1}, (const float[3]){7, 3, 1}, skip);
        CHECK(k1 == 1 && k3 == 1 && k4 == 1, "the brick wall: one wall crossed, either way and diagonally (%d %d %d)", k1, k3, k4);
        CHECK(k2 == 2, "the brick wall and the gable: two walls (%d)", k2);
        CHECK(!mat_wall_crossings(&w, (const float[3]){3, 2, 3.5f}, (const float[3]){7, 2, 3.5f}, skip), "over the brick wall: none");
        CHECK(!mat_wall_crossings(&w, (const float[3]){3, 5, 2.6f}, (const float[3]){7, 5, 2.6f}, skip) &&
                  mat_wall_crossings(&w, (const float[3]){3, 5, 1}, (const float[3]){7, 5, 1}, skip) == 1,
              "the door panel: above it none, through it one");
        CHECK(!mat_wall_crossings(&w, (const float[3]){6.5f, 1, 1}, (const float[3]){7.5f, 3, 1}, skip), "open ground: none");
        CHECK(!mat_wall_crossings(&w, (const float[3]){1, 5, 1}, (const float[3]){3, 5, 1}, skip), "the chain-link fence: none");
        CHECK(!mat_wall_crossings(&w, (const float[3]){4.8f, 2, 1}, (const float[3]){7, 2, 1}, skip) &&
                  !mat_wall_crossings(&w, (const float[3]){3, 2, 1}, (const float[3]){5.2f, 2, 1}, skip),
              "a wall within the skip distance of either end: none (an emitter at a wall)");
        CHECK(!mat_wall_crossings(&w, (const float[3]){-9, 2, 1}, (const float[3]){-3, 2, 1}, skip) &&
                  !mat_wall_crossings(NULL, (const float[3]){3, 2, 1}, (const float[3]){7, 2, 1}, skip) &&
                  !mat_wall_crossings(&w, (const float[3]){5, 2, 1}, (const float[3]){5, 2, 2}, skip),
              "outside the map, no map, a vertical segment: none");
        CHECK(mat_wall_crossings(&w, (const float[3]){-20, 2, 1}, (const float[3]){7, 2, 1}, skip) == 1,
              "a segment starting outside the map still finds the wall inside");
    }
    mat_wall_map_free(&w);
    CHECK(!w.texels && !w.w, "freed");
    CHECK(mat_wall_skip(MAT_ID_CAMERA_GHOST) && mat_wall_skip(2) && mat_wall_skip(107) && !mat_wall_skip(0) && !mat_wall_skip(116) && !mat_wall_skip(13),
          "skipped: ghost, glass, chain-link; kept: material 0, brick, train cars");

    /* Floors only: no wall map. */
    memset(&m, 0, sizeof m);
    quad(&m, 0, 0, 10, 10, 0, 6);
    mat_mesh_find(file, level_file(&m, file), &mm);
    CHECK(mat_wall_map_build(&mm, &w) == -1 && !w.texels, "no wall: no map");
    /* A 3 km wall: the cell grows so a side stays within MAT_WALL_MAP_MAX. */
    memset(&m, 0, sizeof m);
    wall(&m, 0, 0, 3000, 0, 0, 3, 116);
    mat_mesh_find(file, level_file(&m, file), &mm);
    CHECK(mat_wall_map_build(&mm, &w) == 0 && w.w <= MAT_WALL_MAP_MAX && w.cell > MAT_WALL_CELL && mat_wall_at(&w, 2999, 0, 1),
          "a 3 km wall: %d texels wide, cell %.2f", w.w, w.cell);
    mat_wall_map_free(&w);
}

/* Doors the title opens: the test_wall_map level (triangles: floor 0-1, brick
 * 2-3, door panel 4-5, ghost 6-7, fence 8-9, ceiling 10-11, gable 12). Opening the panel's triangles
 * rebuilds only the cells they cover, from the walls that remain (the brick wall shares the cells at
 * y 4), to the map of the level without the panel; closing restores the original map. */
static void test_wall_doors(void) {
    static Mesh m;
    static uint8_t file[8192], file2[8192];
    memset(&m, 0, sizeof m);
    quad(&m, 0, 0, 10, 10, 0, 6);
    wall(&m, 5, 0, 5, 4, 0, 3, 116);
    wall(&m, 5, 4, 5, 6, 0, 2.3f, 0);
    wall(&m, 8, 0, 8, 10, 0, 5, MAT_ID_CAMERA_GHOST);
    wall(&m, 2, 0, 2, 10, 0, 2, 107);
    int a = vert(&m, 6, 6, 3), b = vert(&m, 8, 6, 3), c = vert(&m, 8, 8, 3), d = vert(&m, 6, 8, 3);
    tri(&m, a, c, b, 1), tri(&m, a, d, c, 1);
    a = vert(&m, 9, 0, 0), b = vert(&m, 9, 4, 0), c = vert(&m, 9, 2, 4);
    tri(&m, a, b, c, 116);
    MatMesh mm, mm2;
    mat_mesh_find(file, level_file(&m, file), &mm);
    m.t[4][8] = m.t[5][8] = MAT_ID_CAMERA_GHOST;   /* the same level without the panel */
    mat_mesh_find(file2, level_file(&m, file2), &mm2);
    MatWallMap w, none;
    CHECK(mat_wall_map_build(&mm, &w) == 0 && mat_wall_map_build(&mm2, &none) == 0, "both maps built");
    CHECK(w.ntri == 13 && w.tri_sum == mat_tri_sum(mm.tri, mm.ntri), "the mesh's triangle count (%u) and corner checksum kept", w.ntri);
    CHECK(w.w == none.w && w.h == none.h && w.x0 == none.x0 && w.y0 == none.y0 && w.z0 == none.z0 && w.zstep == none.zstep,
          "the panel lies inside the others' bounds (same grid)");
    const size_t bytes = (size_t)w.w * w.h * 2 * sizeof *w.texels;
    uint16_t *orig = malloc(bytes);
    memcpy(orig, w.texels, bytes);
    CHECK(memcmp(orig, none.texels, bytes) != 0, "the panel is in the map");

    const uint16_t door[2] = {4, 5};
    MatWallRect r;
    CHECK(mat_wall_map_set_open(&w, door, 2, 1, &r) == 2, "opening the panel: 2 wall triangles changed");
    CHECK(!memcmp(w.texels, none.texels, bytes), "open: the map of the level without the panel");
    CHECK(!mat_wall_at(&w, 5, 5, 1) && mat_wall_at(&w, 5, 2, 1.5f) && mat_wall_at(&w, 5, 3.95f, 1), "open: the doorway passes, the brick wall still blocks");
    /* The rebuilt cells: the panel's footprint (x 5 on a cell border: both columns; y 4..6 with the
     * border rows), nothing else. */
    const int cx = (int)floorf((5 - w.x0) / w.cell), cy0 = (int)floorf((4 - w.y0) / w.cell), cy1 = (int)floorf((6 - w.y0) / w.cell);
    CHECK(r.x0 == cx - 1 && r.x1 == cx + 1 && r.y0 == cy0 - 1 && r.y1 == cy1 + 1, "rect x %d..%d y %d..%d (want x %d..%d y %d..%d)", r.x0, r.x1, r.y0,
          r.y1, cx - 1, cx + 1, cy0 - 1, cy1 + 1);
    int outside = 0, inside = 0;
    for (int y = 0; y < w.h; y++)
        for (int x = 0; x < w.w; x++) {
            const size_t i = 2 * ((size_t)y * w.w + x);
            const int diff = orig[i] != w.texels[i] || orig[i + 1] != w.texels[i + 1];
            if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) inside += diff;
            else outside += diff;
        }
    CHECK(outside == 0 && inside > 0, "only cells inside the rect change (%d inside, %d outside)", inside, outside);
    CHECK(mat_wall_map_set_open(&w, door, 2, 1, &r) == 0 && r.x1 <= r.x0, "opening again: nothing changes, empty rect");
    const uint16_t floor_and_far[3] = {0, 1, 999};
    CHECK(mat_wall_map_set_open(&w, floor_and_far, 3, 1, &r) == 0 && r.x1 <= r.x0 && !memcmp(w.texels, none.texels, bytes),
          "the floor (not a wall) and an index past the mesh: ignored");
    CHECK(mat_wall_map_set_open(&w, door, 1, 0, &r) == 1 && mat_wall_map_set_open(&w, door + 1, 1, 0, &r) == 1, "closing the panel one triangle at a time");
    CHECK(!memcmp(w.texels, orig, bytes), "closed: the original map again");
    CHECK(mat_wall_map_set_open(NULL, door, 2, 1, &r) == 0 && r.x1 <= r.x0, "no map: nothing");
    free(orig);
    mat_wall_map_free(&w);
    mat_wall_map_free(&none);
    CHECK(!w.wtri && !w.ntri, "freed (the kept triangles too)");

    /* The game's collision in memory: the file's 10-byte records, the u16 at +6 their flags, bit 0
     * (MAT_TRI_SOLID) set in every record of the file; a door clears it on its panel while open. */
    uint8_t rec[6][10];
    for (int i = 0; i < 6; i++) {
        memset(rec[i], 0, 10);
        rec[i][0] = (uint8_t)i, rec[i][2] = (uint8_t)(i + 1), rec[i][4] = (uint8_t)(i + 2);
        rec[i][6] = 0x01, rec[i][7] = 0xf0, rec[i][8] = 6;
    }
    rec[2][6] = 0x02, rec[2][7] = 0xf4;   /* a door panel (0x402 from its spawn), open */
    rec[4][6] = 0x02, rec[4][7] = 0xf4;
    rec[5][6] = 0x03, rec[5][7] = 0xf4;   /* a closed door panel */
    uint16_t open[4] = {9, 9, 9, 9};
    CHECK(mat_tri_open(&rec[0][0], 6, open, 4) == 2 && open[0] == 2 && open[1] == 4 && open[2] == 9, "open: triangles 2 and 4 (%u %u)", open[0], open[1]);
    open[0] = 9;
    CHECK(mat_tri_open(&rec[0][0], 6, open, 1) == 2 && open[0] == 2 && open[1] == 4, "more than max: the count, only max written");
    const uint32_t sum = mat_tri_sum(&rec[0][0], 6);
    rec[3][6] = 0, rec[3][8] = 99;
    CHECK(mat_tri_sum(&rec[0][0], 6) == sum, "the checksum ignores flags and material");
    rec[3][2] = 77;
    CHECK(mat_tri_sum(&rec[0][0], 6) != sum && mat_tri_sum(&rec[0][0], 5) != mat_tri_sum(&rec[0][0], 6), "a corner index or the count changes it");
}

static void test_wad(void) {
    CHECK(mat_wad_hash("level51.lev") == mat_wad_hash("LEVEL51.LEV") && mat_wad_hash("a/b") == mat_wad_hash("a\\b"), "hash: lowercase, backslashes");
    /* crc32("ee_files\\a") by the standard (zlib) CRC-32. */
    CHECK(mat_wad_hash("a") == 0xa31dad6cu, "hash value %08x", mat_wad_hash("a"));
    char dir[] = "/tmp/materials_test.XXXXXX";
    if (!mkdtemp(dir)) {
        CHECK(0, "mkdtemp");
        return;
    }
    static Mesh m;
    static uint8_t file[8192];
    test_mesh(&m);
    const uint32_t n = (uint32_t)level_file(&m, file);
    char path[512];
    /* XBoxWad.idx: 1 wad "Main.wad0", 2 files; level51.lev at 2 KiB in it. */
    uint8_t idx[20 + 36 + 32];
    memset(idx, 0, sizeof idx);
    put32(idx, 1), put32(idx + 4, 2), put32(idx + 8, 0x100000), put32(idx + 12, 20), put32(idx + 16, 56);
    memcpy(idx + 20, "Main.wad0", 9);
    put32(idx + 56, 0), put32(idx + 60, 16), put32(idx + 64, 0), put32(idx + 68, mat_wad_hash("other.txt"));
    put32(idx + 72, 2), put32(idx + 76, n), put32(idx + 80, 0), put32(idx + 84, mat_wad_hash("level51.lev"));
    snprintf(path, sizeof path, "%s/XBoxWad.idx", dir);
    FILE *f = fopen(path, "wb");
    fwrite(idx, 1, sizeof idx, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/Main.wad0", dir);
    f = fopen(path, "wb");
    static uint8_t pad[2048];
    fwrite(pad, 1, sizeof pad, f);
    fwrite(file, 1, n, f);
    fclose(f);
    size_t got = 0;
    uint8_t *b = mat_wad_read(dir, "level51.lev", &got);
    CHECK(b && got == n && !memcmp(b, file, n), "level51.lev read from the WAD (%zu bytes)", got);
    free(b);
    CHECK(!mat_wad_read(dir, "level52.lev", &got), "a missing file: NULL");
    MatMap map;
    CHECK(mat_level_map(dir, "level51", &map) == 0 && mat_map_at(&map, 15, 5, 0.3f, 1, NULL) == MAT_GRASS, "level51's map from the disc");
    mat_map_free(&map);
    MatWallMap walls;
    CHECK(mat_level_maps(dir, "level51", &map, &walls) == 0 && mat_map_at(&map, 15, 5, 0.3f, 1, NULL) == MAT_GRASS && mat_wall_at(&walls, 5, 10, 1),
          "level51's material and wall maps from one read");
    mat_map_free(&map);
    mat_wall_map_free(&walls);
    CHECK(mat_level_maps(dir, "level51", NULL, &walls) == 0 && walls.texels, "the wall map alone");
    mat_wall_map_free(&walls);
    CHECK(mat_level_maps(dir, "level52", NULL, &walls) == -1 && !walls.texels, "no level52: no wall map");
    CHECK(mat_level_map(dir, "level52", &map) == -1 && !map.texels, "no level52: no map");
    CHECK(mat_level_map("/nonexistent-disc", "level51", &map) == -1, "no disc: no map");
    unlink(path);
    snprintf(path, sizeof path, "%s/XBoxWad.idx", dir);
    unlink(path);
    rmdir(dir);
}

/* A camera at (1, 2, 3) looking along world +y (D3D row-vector view: x right = world x, y up =
 * world z, looking down -z = world +y), right-handed projection (proj[11] -1). */
static void test_wet_build(void) {
    const float view[16] = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, -1, -3, 2, 1};
    float proj[16] = {0};
    proj[0] = 1, proj[5] = 1.7f, proj[10] = -1, proj[11] = -1, proj[14] = -0.1f;
    MaterialsConfig c = MATERIALS_DEFAULTS;
    RRainWet w, z;
    memset(&w, 0, sizeof w);
    z = w;
    CHECK(!materials_wet_build(&c, 1, view, proj, &w) && !memcmp(&w, &z, sizeof w), "off: nothing set");
    c.on = 1;
    CHECK(!materials_wet_build(&c, 0, view, proj, &w) && !memcmp(&w, &z, sizeof w), "no map: nothing set");
    float ortho[16];
    memcpy(ortho, proj, sizeof ortho);
    ortho[11] = 0, ortho[15] = 1;
    CHECK(!materials_wet_build(&c, 1, view, ortho, &w) && !memcmp(&w, &z, sizeof w), "no perspective camera: nothing set");
    c.strength = 0.7f;
    CHECK(materials_wet_build(&c, 1, view, proj, &w) == 1 && w.materials == 0.7f, "on with a map: the strength (%.2f)", w.materials);
    CHECK(fabsf(w.eye[0] - 1) < 1e-5f && fabsf(w.eye[1] - 2) < 1e-5f && fabsf(w.eye[2] - 3) < 1e-5f, "eye (%.2f %.2f %.2f)", w.eye[0], w.eye[1], w.eye[2]);
    CHECK(w.right[0] == 1 && w.up_v[2] == 1 && w.back[1] == -1, "axes: right +x, up +z, towards the viewer -y");
    CHECK(!w.reflect && !w.streaks && !w.puddles, "the other fields untouched");
}

int main(void) {
    test_options();
    test_wet_build();
    test_classes();
    test_parse_and_map();
    test_wall_map();
    test_wall_doors();
    test_wad();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
