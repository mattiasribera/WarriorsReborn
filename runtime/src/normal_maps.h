/* --normal-maps: per-pixel relief and specular from companion maps in a texture pack. Next to a
 * texture's colour replacement (or in a pack of its own: the companions are keyed by the guest
 * texture's content hash, texpack.h, so they also work over the original textures):
 *
 *   <hash>_n.png | <hash>_n.dds   tangent-space normal map, OpenGL convention (+Y: green points up
 *                                 the image, Blender's and Substance's "OpenGL" default; a DirectX
 *                                 map needs its green channel inverted): rgb = n * 0.5 + 0.5. Flat
 *                                 is (128, 128, 255). Alpha: the height for --parallax (255 = the
 *                                 surface, lower = deeper; 255 everywhere = no depth), else ignored.
 *   <hash>_s.png | <hash>_s.dds   specular map: R = intensity (0 = matte, 255 = full highlight),
 *                                 G = gloss / smoothness (0 = broad dull sheen, 255 = tight sharp
 *                                 highlight); B and alpha ignored.
 *
 * A .png is any PNG (expanded to RGBA8, linear: never sRGB-decoded); a .dds is BC1, BC3 or BC7
 * UNORM with a full mip chain, as texpack_load_image reads colour replacements (BC7 suits normal
 * maps best); a .ktx2 (<hash>_n.ktx2, _s.ktx2: Basis Universal UASTC or XUBC7, transcoded to BC7 on
 * load) counts as the map's .dds, and a .dds of the same map in the same pack wins over it. A map is sampled with its texture's own (transformed) coordinates, so its size need
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
#define NORMAL_MAPS_DRY_DEFAULT 1.0f
#define NORMAL_MAPS_DRY_MIN 0.1f
#define NORMAL_MAPS_DRY_MAX 3.0f
#define NORMAL_MAPS_PARALLAX_DEFAULT 0.015f
#define NORMAL_MAPS_PARALLAX_MIN 0.005f
#define NORMAL_MAPS_PARALLAX_MAX 0.1f
/* --dry-highlights: material highlights from the specular maps' B (shine) and A (gloss) channels,
 * written by tools/material_shine.py (car paint, glass, chrome and sheet metal shine bright and
 * tight, brick or cloth faintly). A specular texel with B > 0 shades with intensity 2 B and the
 * Blinn-Phong exponent 2^(3 + 7 A) instead of R / G, the highlights reach RENDERER_DRY_REACH times
 * the light's radius (the world draws' light masks reach as far) and glossy texels add a soft,
 * Fresnel-weighted glint of the lights. B = 0 (maps without material, characters) keeps R / G; the
 * dry strength scales every highlight of the draw. Off: B and A are ignored, nothing changes. */
/* --parallax: parallax occlusion mapping from the height in the normal maps' alpha (written by
 * tools/parallax_height.py for brick, cobbles, stone and wood): on world-lit draws (not characters)
 * whose normal map has a height (normal_maps_image_height), the draw's texture, normal and specular
 * maps are read where the view ray meets the height field below the surface, at most `depth` (a
 * fraction of the texture's size) deep, so mortar, gaps and cobbles sink in and shift as the camera
 * moves. It fades out where the shift is under a pixel (no cost far away). Off, or a map without
 * height: nothing changes. */
typedef struct {
    int on;                   /* --normal-maps / --no-normal-maps (default off) */
    float normal_strength;    /* --normal-strength 0-4: tilt of the map's normals (0 = flat, 1 = as authored) */
    float specular_strength;  /* --specular-strength 0-4: scale of the highlights (0 = none) */
    float baked;              /* --normal-baked 0-1: relief on the game's own (baked / vertex) light (0 = untouched) */
    int dry;                  /* --dry-highlights / --no-dry-highlights (default off; needs --normal-maps) */
    float dry_strength;       /* --dry-highlights-strength 0.1-3 */
    int parallax;             /* --parallax / --no-parallax (default off; needs --normal-maps) */
    float parallax_depth;     /* --parallax-depth 0.005-0.1: the deepest point, a fraction of the texture's size */
} NormalMapsConfig;
#define NORMAL_MAPS_DEFAULTS {0, NORMAL_MAPS_STRENGTH_DEFAULT, NORMAL_MAPS_SPECULAR_DEFAULT, NORMAL_MAPS_BAKED_DEFAULT, 0, NORMAL_MAPS_DRY_DEFAULT, \
                              0, NORMAL_MAPS_PARALLAX_DEFAULT}
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

/* Whether a loaded normal map holds a height (--parallax): some texel's alpha below
 * NORMAL_MAPS_HEIGHT_FLAT in its first level of at most 64 texels a side (or its last level).
 * format: texpack.h TEXPACK_* (BGRA8 levels or BC blocks, as texpack_load_image returns them); a BC1
 * map has no height. */
#define NORMAL_MAPS_HEIGHT_FLAT 250
int normal_maps_image_height(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data);
/* The index remembers it per hash once the map was loaded: -1 unknown (not loaded, or not indexed). */
void normal_maps_set_height(uint64_t hash, int on);
int normal_maps_height(uint64_t hash);

/* --reflections sssr's glass: whether a loaded specular map is glossy, some texel of its first level
 * of at most 64 a side (or its last) mirroring the scene: smoothstep(GLOSSY_SHINE_FROM-TO, B shine) x
 * smoothstep(GLOSSY_GLOSS_FROM-TO, A gloss) above NORMAL_MAPS_GLOSSY_MIN, as shaders/wet.frag glass_k
 * (glass, chrome, car paint; sheet metal, signs, brick, asphalt and cloth not: the minimum is above the
 * shader's GLASS_MIN, so the faint mirror of a metal texel is left out with its draw's cost). format as
 * for normal_maps_image_height; BGRA8, BC3 or BC7 (a BC1 map has no gloss). Only glossy maps' draws go
 * into the glass's surface pass (renderer_set_relief_glossy). */
#define NORMAL_MAPS_GLOSSY_SHINE_FROM 0.25f
#define NORMAL_MAPS_GLOSSY_SHINE_TO 0.7f
#define NORMAL_MAPS_GLOSSY_GLOSS_FROM 0.45f
#define NORMAL_MAPS_GLOSSY_GLOSS_TO 0.8f
#define NORMAL_MAPS_GLOSSY_MIN 0.15f
int normal_maps_image_glossy(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data);
/* The index remembers it per hash once the specular map was loaded: -1 unknown. */
void normal_maps_set_glossy(uint64_t hash, int on);
int normal_maps_glossy(uint64_t hash);

#endif
