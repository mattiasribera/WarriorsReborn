/* --materials: surface materials from the level's own collision.
 *
 * The title's render geometry carries no material; its collision mesh does (for footstep and
 * impact sounds). Every level's `<level>.lev` (in the disc's WAD archive) holds one collision item:
 * chunk class 4 the triangles (10 bytes each: u16 i0, i1, i2, u16 flags, u8 material, u8 extra),
 * class 7 the vertices (16 bytes each: float x, y, z, 1, world space, z up). The material byte is
 * the 1-based Lua MATERIAL enum (MATERIAL.NONE = 1 ... MATERIAL.SALAMI = 190; the engine's id ->
 * name switch at 0x266c00 uses the same values).
 *
 * At a level load the runtime reads that file from the player's own disc and rasterizes the
 * upward-facing collision triangles into a top-down map (MatMap): per cell up to two surfaces (the
 * top one and the highest one at least MAT_LAYER_GAP below it), each a material class and a
 * height. The wet ground looks the class up by the pixel's world position and height
 * (shaders/wet.frag): asphalt glossy, dirt matte mud, grass barely wet. Nothing derived from the
 * disc is written anywhere. */
#ifndef MATERIALS_H
#define MATERIALS_H
#include <stddef.h>
#include <stdint.h>
#include "renderer.h"

#define MATERIALS_STRENGTH_DEFAULT 1.0f
#define MATERIALS_STRENGTH_MIN 0.1f
#define MATERIALS_STRENGTH_MAX 2.0f
typedef struct {
    int on;           /* --materials / --no-materials */
    float strength;   /* --materials-strength 0.1-2: 0 the look without materials, 1 the table, above it stronger */
} MaterialsConfig;
#define MATERIALS_DEFAULTS {0, MATERIALS_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a materials option, -1 for a missing or out-of-range value (c unchanged). */
int materials_arg(MaterialsConfig *c, const char *opt, const char *val);

/* Material classes (shaders/wet.frag MAT_* holds their look, in this order). */
enum {
    MAT_UNKNOWN = 0,   /* none, undefined, props: the look without materials */
    MAT_ASPHALT,       /* asphalt, rooftop tar */
    MAT_CONCRETE,
    MAT_DIRT,          /* dirt, gravel, rubble, garbage */
    MAT_GRASS,         /* grass, vegetation */
    MAT_WOOD,
    MAT_METAL,
    MAT_BRICK,         /* brick and drywall tops: kerbs, walls */
    MAT_GLASS,
    MAT_CLASSES
};
/* The title's MATERIAL ids used here (Lua MATERIAL table). */
#define MAT_ID_NONE 1
#define MAT_ID_CAMERA_GHOST 95   /* invisible camera blockers: never in the map */

/* The class of a MATERIAL id (MAT_UNKNOWN for ids without a ground look). */
int mat_class(uint8_t id);
/* The class's short name ("asphalt", ...), "unknown" out of range. */
const char *mat_class_name(int cls);

/* The collision mesh of a level file: pointers into its buffer (not copied). */
typedef struct {
    const uint8_t *tri;    /* ntri x 10 bytes */
    const uint8_t *vert;   /* nvert x 16 bytes */
    uint32_t ntri, nvert;
} MatMesh;
/* Finds the collision mesh (the first item with class 4 and 7 chunks) in a level file: a pack
 * {count, total, 0, 0xDE686795} of items or one bare item; item {nchunks, size, 0, hash} then
 * chunks {class, size, 0, hash} + payload. 0, or -1 when there is none or the file is malformed. */
int mat_mesh_find(const uint8_t *buf, size_t n, MatMesh *m);
/* Collision triangle t's corners (world, z up) and material byte: 0 with an out-of-range index or a
 * non-finite or far corner; else 1 when it faces up (normal z above MAT_UP of its length), 2 otherwise
 * (a wall), 3 when it is degenerate (no normal). */
int mat_tri_read(const MatMesh *m, uint32_t t, float p[3][3], uint8_t *mat);

/* The top-down map. Texel (x, y) covers world [x0 + x cell, x0 + (x + 1) cell) and likewise y; two
 * 16-bit layers, the top surface then the second: class << 12 | height, height 0 no surface, h in
 * 1-4095 world z = z0 + (h - 1) zstep (the surface's height at the cell's centre). */
#define MAT_CELL 0.5f          /* world units (metres) per texel, raised when the level does not fit */
#define MAT_MAP_MAX 2048       /* texels per side at most */
#define MAT_LAYER_GAP 2.0f     /* the second surface lies at least this far below the top one */
#define MAT_UP 0.6f            /* triangles whose normal's z exceeds this face up (about 53 degrees) */
#define MAT_ZSTEP_MIN 0.01f
#define MAT_HEIGHT_MAX 4095
typedef struct {
    int w, h;
    float x0, y0, cell;
    float z0, zstep;
    uint16_t *texels;  /* w x h x 2, row y = 0 first (malloc'd; mat_map_free) */
    uint32_t surfaces; /* upward triangles rasterized */
} MatMap;
/* Builds the map of the mesh's upward-facing triangles (CAMERA_GHOST left out; out-of-range
 * indices skipped): bounds from those triangles, cells of MAT_CELL (larger when a side would
 * exceed MAT_MAP_MAX), every cell whose centre lies in a triangle (and the cell of each triangle's
 * centroid, so small triangles are not lost) takes the surface's height there. 0, or -1 without
 * an upward triangle or memory (map zeroed). */
int mat_map_build(const MatMesh *m, MatMap *map);
void mat_map_free(MatMap *map);
/* The class and world height of the surface at world (x, y) nearest in height to z within tol
 * (MAT_UNKNOWN and no height outside the map or without such a surface): what the shader does
 * per texel. Returns the class; *hz the surface's height when not NULL. */
int mat_map_at(const MatMap *map, float x, float y, float z, float tol, float *hz);

/* --lamp-shadows walls: the top-down map of the collision mesh's
 * walls, so a lamp-shadow ray from a receiver towards its light stops at a wall instead of reaching a
 * character on the far side (geom.frag lamp_shadow). Walls are the triangles not facing up (normal z
 * at most MAT_UP of its length: walls, ceilings, downward faces, steep slopes) with any material,
 * the closed doors' panels (material 0) included, except the camera ghosts and see-through ones
 * (glass, chain-link, railings: light passes, so do shadows). Per cell the lowest and highest point
 * of the walls over it (each triangle clipped to the cell exactly): two 16-bit layers, bottom then
 * top, 0 no wall, h in 1-65535 world z = z0 + (h - 1) zstep (bottom rounded down, top up). */
#define MAT_WALL_CELL 0.25f       /* world units per texel, raised when the level does not fit */
#define MAT_WALL_MAP_MAX 2048     /* texels per side at most (16 MiB) */
#define MAT_WALL_HEIGHT_MAX 65535
#define MAT_WALL_ZSTEP_MIN 0.001f
typedef struct {
    int w, h;
    float x0, y0, cell;
    float z0, zstep;
    uint16_t *texels;   /* w x h x 2 (bottom, top), row y = 0 first (malloc'd; mat_wall_map_free) */
    uint32_t walls;     /* wall triangles rasterized */
    /* Doors the title opens (mat_wall_map_set_open): the mesh's triangle count
     * and corner checksum (mat_tri_sum), and the walls kept to rebuild cells: their corners, their
     * cells, collision triangle index (increasing) and whether each is open (left out of the map). */
    uint32_t ntri, tri_sum;
    float (*wtri)[3][3];
    struct MatWallRect *wcells;   /* the cells each wall touches (its footprint's box) */
    uint32_t *wid;
    uint8_t *wopen;
} MatWallMap;
/* 1 when material id `mat` is see-through or never solid (no wall): camera ghost, glass, chain-link
 * fences, railings. */
int mat_wall_skip(uint8_t mat);
/* Builds the wall map of the mesh (bounds from its walls; out-of-range indices and non-finite
 * corners skipped). 0, or -1 without a wall or memory (map zeroed). */
int mat_wall_map_build(const MatMesh *m, MatWallMap *map);
void mat_wall_map_free(MatWallMap *map);
/* 1 when world point (x, y, z) lies within a wall cell's height span (bottom <= z <= top): what the
 * shader tests per march step; 0 outside the map or the span. */
int mat_wall_at(const MatWallMap *map, float x, float y, float z);
/* --audio-occlusion: the walls the world segment a -> b passes through, in a
 * 2D march over the map's cells (each cell the segment crosses, with the segment's height range
 * inside it, tested against the cell's span as mat_wall_at does); a run of blocked cells counts as
 * one wall. The first and last `skip` world units of the segment are left out (an emitter standing
 * at a wall, a listener against one). 0 without a map, outside it or for a vertical segment. */
int mat_wall_crossings(const MatWallMap *map, const float a[3], const float b[3], float skip);

/* Doors the title opens. The title keeps the level's collision triangles in
 * memory as the .lev's 10-byte records (the world at *(*(*(0x5af590) + 0x10) + 4): count u16 at +0x78,
 * records at +0x7c); the u16 at +6 holds flags, bit 0 (MAT_TRI_SOLID) set in every record of the file.
 * A door's behaviour clears it on its two panel triangles (SpawnDoor's 4th argument) once it has
 * swung open (message 0x22 with 5: 0x2b4040) and sets it again when it closes (0x22 with 3, 6 or 7:
 * 0x2b4070); characters then walk through the doorway. */
#define MAT_TRI_SOLID 0x0001
/* FNV-1a over the records' corner indices (bytes 0-5 of each) and their count: the same mesh in the
 * file and in the title's memory (flags and material, which the title changes, left out). */
uint32_t mat_tri_sum(const uint8_t *tri, uint32_t ntri);
/* The triangles among records `tri` (ntri) whose MAT_TRI_SOLID bit is clear, increasing, into out (at
 * most max written). Returns their count (which may exceed max). */
uint32_t mat_tri_open(const uint8_t *tri, uint32_t ntri, uint16_t *out, uint32_t max);
/* Cells [x0, x1) x [y0, y1) of a wall map; empty when x1 <= x0. */
typedef struct MatWallRect {
    int x0, y0, x1, y1;
} MatWallRect;
/* Marks the collision triangles tri[0..n) open (1: left out of the map) or closed (0: back in) and
 * rebuilds every cell they cover from the walls that remain, exactly as mat_wall_map_build would (the
 * grid and the height scale unchanged); *r the cells rebuilt (their union; empty without a change).
 * Triangles that are not walls of the map are ignored. Returns the walls whose state changed. */
int mat_wall_map_set_open(MatWallMap *map, const uint16_t *tri, uint32_t n, int open, MatWallRect *r);

/* Fills the materials fields of w (wet ground already built by rain_wet_build) when c->on and
 * has_map: w->materials = the strength, and the camera's axes and eye in the world (as
 * rain_puddle_build; view and projection as rain_wet_build's). Returns 1, else 0 with w unchanged
 * (off, no map or an unusable camera: exactly as without the option). */
int materials_wet_build(const MaterialsConfig *c, int has_map, const float view[16], const float proj[16], RRainWet *w);

/* The WAD file name hash: CRC-32 of "ee_files\" + the lowercased name with backslashes. */
uint32_t mat_wad_hash(const char *name);
/* Reads the file `name` (as "level51.lev") from the disc's WAD archive (disc/XBoxWad.idx and the
 * archives it names): malloc'd bytes and *n, or NULL. */
uint8_t *mat_wad_read(const char *disc, const char *name, size_t *n);
/* The map of level `level` (as "level51") from the disc: 0, or -1 (map zeroed). */
int mat_level_map(const char *disc, const char *level, MatMap *map);
/* Level `level`'s material map and / or wall map (either may be NULL) from one read of the disc:
 * each requested map built (0) or zeroed (-1); returns 0 when every requested map was built, else -1. */
int mat_level_maps(const char *disc, const char *level, MatMap *map, MatWallMap *walls);
#endif
