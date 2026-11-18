/* Mesh overrides of the Lua mods (docs/MODDING.md, "Mesh overrides"): a mod whose mod.ini says
 * `meshes = FOLDER` brings replacement meshes for any indexed triangle-list draw of the title
 * (characters and objects alike), each in the draw's own vertex layout, keyed by the --mesh-pack
 * content hash of the original draw (meshpack_hash: index count, strides, indices, the vertex bytes
 * the indices reach). Unlike --mesh-pack (characters only, one fixed layout) the layout is free:
 * the override must only have the strides of the draw it replaces, which the title's own vertex
 * program then reads.
 *
 * Files in FOLDER (16 lower-case hex digits each):
 *   <mesh>.wmesh          replaces every draw of that mesh
 *   <mesh>-<tex>.wmesh    only the draws whose stage-0 texture has the texture-pack content hash
 *                         <tex> (texpack_hash), e.g. one skin of a character model
 *   <same name>.png       optional: the stage-0 texture the replacement is drawn with (else the
 *                         draw's own texture)
 *   <mesh>[-<tex>].hide   those draws are not drawn at all (the file's content is ignored)
 * A subfolder SUB of FOLDER (a-z 0-9 - _) holds the same kinds of files as a group, named
 * "<id>/SUB" (id: the mod's), which is off until meshover_group_set turns it on (the Lua mods'
 * ModMeshSet): an override of an enabled group wins over FOLDER's own for the same draw.
 * A group folder with a file "<marker>.worn" (the content hash of an object draw, e.g. a hat) is an
 * instance group: per character, not per mesh. Its overrides apply to a character draw only while
 * that character's head carries no draw of the marker, and to the marker's own draws only while
 * they are not on a head (the caller judges both: meshover_instance_mask, meshover_marker_mask,
 * meshover_find_inst). One marker per group.
 * Points: a mod's Lua gives a group world positions, each under a key (meshover_point_set; the Lua
 * mods' ModMeshPoint), e.g. where each character is that lost the marker. For an instance group a
 * character whose head lies near such a point does not wear the marker (the state survives the
 * character leaving the view). A group folder with a file "placed"
 * is a placed group: its overrides apply only to object draws whose origin lies near one of its
 * points (one object among many of the same mesh). The caller judges both (meshover_point,
 * meshover_placed_mask).
 * The .wmesh format is meshpack.h's (WMP1) with the draw's strides; stride 1 is 0 for a draw with
 * one vertex stream. A replacement is always a triangle list, whatever primitive the original draw
 * used (its hash: meshover_draw_hash). Nothing here is game-specific: the files are made by a mod's local tool.
 * Off (no mod with meshes enabled) costs nothing: meshover_count() is 0. */
#ifndef MESHOVER_H
#define MESHOVER_H
#include <stdint.h>
#include "meshpack.h"

typedef struct {
    MeshPackMesh mesh;    /* mesh.hash = the original draw's content hash */
    uint64_t tex;         /* the stage-0 texture hash it is limited to (has_tex) */
    int has_tex;
    char *png;            /* the replacement's stage-0 texture file, or NULL */
    int mod;              /* the caller's tag (the mod's index) */
    int group;            /* 0: always on; else its group (meshover_group_set) is index group - 1 */
    int hide;             /* a .hide file: the draw is not drawn (no mesh) */
    int used;             /* meshover_first_use was called */
} MeshOverride;

/* Content hash of a draw: meshpack_hash of its indices and vertices; a draw that is not a triangle
 * list (primitive 5) mixes its primitive type in, so the same bytes drawn as a strip differ. */
uint64_t meshover_draw_hash(uint32_t prim, const uint16_t *idx, uint32_t ni, uint32_t lo, uint32_t hi, const uint8_t *s0,
                            uint32_t stride0, const uint8_t *s1, uint32_t stride1);
/* Loads every valid override file of dir and, when id is not NULL, of each subfolder as the group
 * "<id>/<subfolder>" (off); invalid files are reported and skipped. The number loaded (groups
 * included), -1 if dir cannot be read. Overrides of an earlier call win over later ones. */
int meshover_add_dir(const char *dir, int mod, const char *id);
/* Turns a group on or off; the number of overrides in it, -1 when there is no such group. */
int meshover_group_set(const char *name, int on);
/* Whether a group is on (-1: no such group). */
int meshover_group_on(const char *name);
int meshover_count(void);
/* Number of overrides loaded for one mod tag. */
int meshover_count_mod(int mod);
/* Whether any override replaces this mesh (with or without a texture condition), and whether one
 * of them needs the draw's texture hash. */
int meshover_has_mesh(uint64_t mesh, int *needs_tex);
/* The override for a draw of this mesh whose stage-0 texture hash is tex (tex_known 0: unknown,
 * only unconditional overrides match), among the always-on ones and those of enabled groups: a
 * texture-limited one first, else the unconditional one; at each step an enabled group's first. */
const MeshOverride *meshover_find(uint64_t mesh, uint64_t tex, int tex_known);
/* meshover_find with the groups of the bit mask off (bit g = group index g) treated as off for this
 * draw: an instance group whose marker this character wears, or whose marker draw is on a head. */
const MeshOverride *meshover_find_inst(uint64_t mesh, uint64_t tex, int tex_known, uint32_t off);
/* The enabled instance groups (bit mask) that override this mesh, and those whose marker it is. */
uint32_t meshover_instance_mask(uint64_t mesh);
uint32_t meshover_marker_mask(uint64_t mesh);
/* Points of a group: sets (p not NULL) or removes (p NULL) the point under key (at most
 * MESHOVER_KEY - 1 bytes are kept). The group's number of points after it, -1 for no such group,
 * -2 when the group already holds MESHOVER_POINTS points. */
#define MESHOVER_POINTS 64
#define MESHOVER_KEY 24
int meshover_point_set(const char *group, const char *key, const float *p);
/* Removes every point of every group (a new Lua state: the keys are the old state's handles). */
void meshover_points_clear(void);
/* The number of points of group index g, and point i of it (NULL out of range). */
int meshover_point_count(int g);
const float *meshover_point(int g, int i);
/* Whether any group holds a point. */
int meshover_points_any(void);
/* The enabled placed groups (bit mask) that override this mesh. */
uint32_t meshover_placed_mask(uint64_t mesh);
/* 1 the first time it is called for o (the "[meshover] mod mesh ... drawn" log line), else 0. */
int meshover_first_use(const MeshOverride *o);
/* Index of an override (meshover_get) for the frame list. */
int meshover_index(const MeshOverride *o);
const MeshOverride *meshover_get(int i);
/* Draw identity memo like meshpack's, larger (object draws are many). */
int meshover_memo_get(uint64_t key, uint64_t *hash);
void meshover_memo_set(uint64_t key, uint64_t hash);
/* Parses an override file name (.wmesh, or .hide: *hide = 1); 0 when it is one (mesh, tex,
 * has_tex, hide set). */
int meshover_parse_name(const char *name, uint64_t *mesh, uint64_t *tex, int *has_tex, int *hide);
void meshover_close(void);
#endif
