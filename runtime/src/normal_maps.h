/* --normal-maps: per-pixel relief and specular from companion maps in a texture pack. Next to a
 * texture's colour replacement (or in a pack of its own: the companions are keyed by the guest
 * texture's content hash, texpack.h, so they also work over the original textures):
 *
 *   <hash>_n.png | <hash>_n.dds   tangent-space normal map, OpenGL convention (+Y: green points up
 *                                 the image, Blender's and Substance's "OpenGL" default; a DirectX
 *                                 map needs its green channel inverted): rgb = n * 0.5 + 0.5,
 *                                 alpha ignored. Flat is (128, 128, 255).
 *   <hash>_s.png | <hash>_s.dds   specular map: R = intensity (0 = matte, 255 = full highlight),
 *                                 G = gloss / smoothness (0 = broad dull sheen, 255 = tight sharp
 *                                 highlight); B and alpha ignored.
 *
 * A .png is any PNG (expanded to RGBA8, linear: never sRGB-decoded); a .dds is BC1, BC3 or BC7
 * UNORM with a full mip chain, as texpack_load_image reads colour replacements (BC7 suits normal
 * maps best). A map is sampled with its texture's own (transformed) coordinates, so its size need
 * not match the colour texture's. A texture without companions is drawn exactly as before.
 *
 * The first pack that has a hash's map wins; within one pack its .dds wins over its .png while .dds
 * files are offered (normal_maps_set_dds: the device samples BC blocks), and a later pack's .png
 * backs a hash whose earlier pack holds only a .dds. texpack ignores these names (they are not
 * 16 hex digits + .png/.dds) and so does --neon-glow. tools/normal_from_albedo.py makes stopgap maps
 * from a PNG pack until real ones are baked.
 *
 * This file is the game- and renderer-independent part: options, names, the index. One host thread
 * (the index); src/hle_d3d.c binds the maps per draw (renderer_set_relief) and the renderer shades
 * them in its lit geometry variant (shaders/geom.frag RELIEF). Tested by tests/normal_maps_test.c. */
#ifndef NORMAL_MAPS_H
#define NORMAL_MAPS_H
#include <stddef.h>
#include <stdint.h>

#define NORMAL_MAPS_STRENGTH_DEFAULT 1.0f
#define NORMAL_MAPS_SPECULAR_DEFAULT 1.0f
#define NORMAL_MAPS_BAKED_DEFAULT 1.0f
#define NORMAL_MAPS_STRENGTH_MAX 4.0f
typedef struct {
    int on;                   /* --normal-maps / --no-normal-maps (default off) */
    float normal_strength;    /* --normal-strength 0-4: tilt of the map's normals (0 = flat, 1 = as authored) */
    float specular_strength;  /* --specular-strength 0-4: scale of the highlights (0 = none) */
    float baked;              /* --normal-baked 0-1: relief on the game's own (baked / vertex) light (0 = untouched) */
} NormalMapsConfig;
#define NORMAL_MAPS_DEFAULTS {0, NORMAL_MAPS_STRENGTH_DEFAULT, NORMAL_MAPS_SPECULAR_DEFAULT, NORMAL_MAPS_BAKED_DEFAULT}
extern NormalMapsConfig hle_d3d_normal_maps;   /* defined in src/hle_d3d.c, set by src/main.c */

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a normal-maps option, -1 for a missing or out-of-range value (c unchanged). */
int normal_maps_arg(NormalMapsConfig *c, const char *opt, const char *val);

enum { NORMAL_MAPS_RELIEF, NORMAL_MAPS_SPECULAR, NORMAL_MAPS_KINDS };   /* _n, _s */

/* "<16 lower-case hex digits>_n.png" (kind relief, dds 0), "_s.dds" (specular, dds 1), ... into buf;
 * -1 when n is too small or kind unknown. */
int normal_maps_name(uint64_t hash, int kind, int dds, char *buf, size_t n);
/* The companion of a colour texture's file name or path ("<dir>/<hash>.png" or ".dds", any case):
 * the same path with "_n" or "_s" before the extension (its case kept). -1 when base is not
 * <16 hex>.png / .dds (a companion or an emission mask has no companion) or n is too small. */
int normal_maps_companion(const char *base, int kind, char *buf, size_t n);
/* The hash, kind and file type of a companion file name (hex and suffix in any case); -1 otherwise. */
int normal_maps_parse_name(const char *name, uint64_t *hash, int *kind, int *dds);

/* Companion index. normal_maps_index_add indexes the companion files directly in dir, layered under
 * the packs already added; returns the (hash, kind) maps it added (not already in an earlier pack),
 * or -1 (nothing changes) when dir cannot be read. */
int normal_maps_index_add(const char *dir);
void normal_maps_set_dds(int on);   /* offer .dds files (default off) */
int normal_maps_has(uint64_t hash, int kind);
int normal_maps_count(int kind);    /* hashes with a map of this kind offered */
/* The offered file's path (malloc'd), NULL when the hash has no map of that kind. */
char *normal_maps_path(uint64_t hash, int kind);
/* Drops the offered file of (hash, kind) (it failed to load): its other file, if any, is offered next. */
void normal_maps_forget(uint64_t hash, int kind);
void normal_maps_index_clear(void);

#endif
