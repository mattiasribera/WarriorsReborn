/* Collision patches for mods (docs/MODDING.md "Collision patches").
 *
 * A mod's mod.ini names a patch file (`collision = cage.col`); while the mod is enabled the runtime adds
 * its triangles to the title's collision world of the level it names, and can clear the solid bit of
 * existing triangles (a door gap cut in a fence). Player, character and camera queries then find the new
 * geometry like the level's own.
 *
 * The title's collision world (the level's .lev class-3 chunk, loaded as a memory image) is reached at
 * *(*(*(0x5af590) + 0x10) + 4); every query walks its uniform grid:
 *   +0x04 4x4 world -> grid matrix (row vectors, translation at +0x34)
 *   +0x44 grid minimum, +0x54 grid maximum (x y z w): queries clamp grid coordinates to them
 *   +0x64 u16 nx, +0x66 u16 ny, +0x68 u32 nz
 *   +0x6c u32 offsets[nz][ny][nx] into the u16 lists at +0x74 (0 = empty cell; a list is its count then
 *         triangle indices, increasing), +0x70 u32 list length
 *   +0x78 u16 triangle count, +0x7c 10-byte records {u16 i0, i1, i2, u16 flags, u8 material, u8 extra}
 *   +0x80 float4 vertices, +0x84 u32 vertex count
 *   +0x88 visited bits (one per triangle; the queries clear round16((n + 7) / 8) bytes)
 * Flags: bit 0 solid, bit 1 two-sided (a triangle seen from behind is otherwise ignored).
 *
 * Applying a patch builds new arrays (old content + the patch) in one allocation and points the header
 * at them; nothing of the title's own memory is freed or moved. */
#ifndef COLPATCH_H
#define COLPATCH_H
#include <stddef.h>
#include <stdint.h>

#define COLPATCH_TRI_MAX 4096     /* triangles one patch may add */
#define COLPATCH_REMOVE_MAX 64
#define COLPATCH_FLAG_SOLID 0x0001u
#define COLPATCH_FLAG_TWO_SIDED 0x0002u
#define COLPATCH_FLAGS_DEFAULT 0x0021u   /* solid + bit 5, as nearly every record of the levels */

typedef struct {
    float p[3][3];
    uint16_t flags;
    uint8_t material;
} ColTri;
typedef struct {
    float lo[3], hi[3];
} ColBox;
typedef struct {
    char level[32];          /* "level95": the level whose collision this patches */
    ColTri *tri;             /* malloc'd (colpatch_free) */
    uint32_t ntri, cap;
    ColBox remove[COLPATCH_REMOVE_MAX];
    uint32_t nremove;
} ColPatch;

/* Parses a patch file:
 *   ; or # starts a comment (a whole line or after a value); blank lines ignored; numbers are world units (metres, z up)
 *   level = level95              required, before any shape
 *   material = 5                 the Lua MATERIAL id of the shapes that follow (default 1, NONE)
 *   flags = 0x21                 the record flags of the shapes that follow (default 0x21; solid is
 *                                always set; walls, boxes and ceilings add two-sided)
 *   floor X0 Y0 X1 Y1 Z          an upward rectangle (two triangles)
 *   ceiling X0 Y0 X1 Y1 Z        a downward rectangle, two-sided
 *   wall X0 Y0 X1 Y1 Z0 Z1       a vertical rectangle from (X0, Y0) to (X1, Y1), two-sided
 *   box X0 Y0 Z0 X1 Y1 Z1        four two-sided walls and an upward top
 *   tri X Y Z X Y Z X Y Z        one triangle, front side counter-clockwise
 *   remove X0 Y0 Z0 X1 Y1 Z1     the level's triangles whose three corners lie in the box stop being
 *                                solid
 * 0, or -1 with the first problem ("line N: ...") in err (p freed). */
int colpatch_parse(const char *text, size_t len, ColPatch *p, char *err, size_t cap);
void colpatch_free(ColPatch *p);

/* Field offsets of the title's collision world. */
enum {
    COLW_MATRIX = 0x04, COLW_GMIN = 0x44, COLW_GMAX = 0x54, COLW_NX = 0x64, COLW_NY = 0x66, COLW_NZ = 0x68,
    COLW_OFFSETS = 0x6c, COLW_LISTLEN = 0x70, COLW_LISTS = 0x74, COLW_NTRI = 0x78, COLW_TRIS = 0x7c,
    COLW_VERTS = 0x80, COLW_NVERT = 0x84, COLW_VISITED = 0x88, COLW_SIZE = 0x8c
};

/* World point -> grid cell as the title's queries pick it (matrix, clamp, truncate). */
void colpatch_cell(const uint8_t *world, const float p[3], uint32_t c[3]);

/* Applies the patches to the collision world at `world` (a host pointer; the addresses inside it are
 * host pointers too, as in the title's memory): clears the solid bit of the removed triangles, then
 * builds the old arrays plus the patches' triangles (each added to every cell its grid-space box
 * touches) in one malloc'd block, *block (free it once the world no longer uses it), and points the
 * header at them. Returns the triangles added (0 when only removals applied, *block NULL), -1 when the
 * result would not fit the u16 counts or memory ran out (world unchanged). *removed: the triangles
 * made non-solid. */
int colpatch_apply(uint8_t *world, const ColPatch *const *patches, int npatches, void **block, uint32_t *removed);

/* --- run time (hle_mods.c registers, hle_d3d.c calls colpatch_frame at each swap) --- */
/* Registers a mod's patch text (copied and parsed; a parse error logs `[collision] <id>: ...` and
 * returns -1). */
int colpatch_register(const char *mod_id, const char *text, size_t len);
/* Forgets every registered patch (tests). */
void colpatch_reset(void);
int colpatch_count(void);
/* The disc folder the levels' .lev files are read from (to recognise the level a world belongs to). */
void colpatch_set_disc(const char *disc);
/* At the swap, on the guest thread: when the title's collision world changed and it is the unpatched
 * world of a level some patch names (its triangle count and corner checksum equal the level file's),
 * applies that level's patches and logs `[collision] levelNN: ...`. `readable(addr, len)` checks guest
 * memory. Returns 1 when it applied patches in this call. */
int colpatch_frame(int (*readable)(uint32_t addr, uint32_t len));
/* The triangle count of the level's own world while a patch is applied to it, else 0 (--wall-doors
 * compares only those records with the level file). */
uint32_t colpatch_base_ntri(void);
#endif
