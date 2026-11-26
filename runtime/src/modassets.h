/* Mod assets built from the player's own disc (docs/MODDING.md "assets.ini"): a mod folder's
 * `assets.ini` declares meshes and textures made from the game's own characters, textures and
 * props (found by the names the game's scripts use) and from procedural fills; this engine builds
 * them in C at mod load and writes them into the player's cache, in the formats the mesh override
 * and texture layer loaders read (src/meshover.h, src/texlayer.h). Nothing game-derived is in a
 * mod, the repository or a release: the results exist only under <hdd>/mods-cache/.
 *
 * Plain C over src/disc.h, xtex and the PNG writer of stb_image_write; no runtime state, so the
 * launcher can run the same build (docs/MODDING.md).
 *
 * Cache: <hdd>/mods-cache/<id>/<key>/ with the output files, build.json and an empty `done`. The
 * key (16 hex digits) is FNV-1a 64 over the engine version, the assets.ini bytes, the mod version
 * and the disc fingerprint (FNV-1a 64 of XBoxWad.idx), so any change rebuilds. A build writes into
 * <key>.partial and renames it when complete; a folder without `done` is never used. After a
 * successful build the mod's other keys are deleted. Budgets per mod: MODASSETS_TIME_MS of build
 * time (checked between steps), MODASSETS_BYTES_MAX of output, MODASSETS_FILES_MAX files; textures
 * at most MODASSETS_TEX_MAX texels a side, meshes at most 65535 vertices.
 *
 * Errors: one line `<file>:<line>: <reason>` (modassets_prepare's err), logged by the caller as
 * `[modassets] <id>: ...`; the mod then loads without its assets. */
#ifndef MODASSETS_H
#define MODASSETS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODASSETS_ENGINE "1"      /* engine version: part of every cache key */
#define MODASSETS_TIME_MS 30000
#define MODASSETS_BYTES_MAX (64u << 20)
#define MODASSETS_FILES_MAX 256
#define MODASSETS_TEX_MAX 2048
#define MODASSETS_LINE_MAX 512

/* Budgets in force (tests lower them). */
extern unsigned modassets_time_ms, modassets_bytes_max, modassets_files_max;

/* ---- the recipe file ---- */
typedef struct {
    int line;
    char key[32];
    char value[MODASSETS_LINE_MAX];
} MaLine;
typedef struct {
    char kind[16], name[64];
    int line;
    int first, n;   /* its lines: lines[first .. first + n) */
} MaSection;
typedef struct {
    char file[64];   /* "assets.ini": the name in error messages */
    char dir[1024];  /* the mod's folder (own/ files are read from it; "" = none: an own file is an error) */
    char warn[400];  /* set by ma_build: the first warning (e.g. own/x.glb: N vertices had more than 3 bones), or "" */
    int format;
    MaSection *sec;
    int nsec;
    MaLine *lines;
    int nlines;
} MaRecipe;

/* Parses assets.ini text: `key = value` lines, `;` comments (whole line or after the value),
 * `[kind name]` sections; the top level holds only `format = 1`. Section kinds: source, texture,
 * mesh, hide, set, layer (a layer's name may hold {code} and {stage}), sound, collision. 0, or -1 with "<file>:<line>: <reason>" in err. */
int ma_recipe_parse(const char *text, size_t len, const char *file, MaRecipe *r, char *err, size_t cap);
void ma_recipe_free(MaRecipe *r);

/* The cache key of a recipe (see above). */
uint64_t ma_cache_key(const char *recipe, size_t len, const char *mod_version, uint64_t disc_fingerprint);
/* The key with the mod's own files mixed in: every regular file of <mod_dir>/own and its
 * subfolders (one level), by name and bytes in name order; the key unchanged without an own folder.
 * -1 when own/ holds more than MA_OWN_FILES_MAX files or MA_OWN_BYTES_MAX bytes (err). */
int ma_own_key(const char *mod_dir, uint64_t *key, char *err, size_t cap);

/* ---- building ---- */
typedef struct {
    char cache[1200];   /* the ready cache folder (modassets_prepare returned 1) */
    uint64_t key;
    int built;          /* 1: built now, 0: cache hit */
    int outputs;        /* output sections in the cache ([mesh], [hide], [layer], [sound], [collision]) */
    int files;          /* files written */
    double ms;          /* build time */
    char warn[400];     /* a build's first warning, or "" (also on a cache hit: "") */
} MaResult;

/* Builds (or finds) the assets of the mod in folder mod_dir (its assets.ini) for disc folder disc
 * into hdd's cache. rebuild: ignore an existing cache. Returns 1 (res->cache ready), 0 (no
 * assets.ini: nothing to do), -1 (err: "<file>:<line>: <reason>" or a plain reason; no cache). */
int modassets_prepare(const char *mod_dir, const char *id, const char *version, const char *disc, const char *hdd, int rebuild,
                      MaResult *res, char *err, size_t cap);

/* Whether the mod in folder mod_dir needs no build: 1 when its cache for this disc is complete, 0
 * when modassets_prepare would build (or fail), -1 when it has no assets.ini. Cheap: it reads
 * assets.ini and XBoxWad.idx only (the launcher asks before starting the game). */
int modassets_current(const char *mod_dir, const char *id, const char *version, const char *disc, const char *hdd);

/* The disc's chunk index file builds use (<hdd>/mods-cache/disc-<fingerprint>.idx, set by
 * modassets_prepare; empty: no file): the first build scans the disc once, later ones read it. */
void ma_set_index_path(const char *path);

/* The build itself (src/modassets_build.c): runs recipe r against the disc folder disc, writing
 * the output files into out_dir (existing); own/ files are read from r->dir and the first warning
 * goes to r->warn. Returns the number of output sections, or -1 (err).
 * *files and *bytes: what was written. */
int ma_build(MaRecipe *r, const char *disc, const char *out_dir, int *files, uint64_t *bytes, char *err, size_t cap);

/* PNG (8-bit RGBA, filter 0 on every row, zlib from stb_image_write) of w x h RGBA texels. 0 or -1. */
int ma_png_write(const char *path, int w, int h, const uint8_t *rgba);

/* ---- own assets (src/modassets_own.c; docs/MODDING.md "Your own assets") ----
 * A mod's own creations in its `own/` folder: glTF 2.0 binary meshes (.glb, read with cgltf),
 * PNG / DDS (BC1, BC3) textures and WAV / OGG sounds (stb_vorbis). glTF is Y-up, the game Z-up:
 * (x, y, z) -> (x, -z, y), a rotation, so windings hold and Blender's own coordinates (exported
 * with +Y Up) are the game's; units are metres. */
#define MA_OWN_FILES_MAX 64
#define MA_OWN_BYTES_MAX (32u << 20)        /* own/ in all */
#define MA_OWN_EXTENT_MAX 10.0              /* m: a larger mesh was exported in centimetres */
#define MA_OWN_SKIN_JOINTS_MAX 64           /* joints of a .glb skin (the game's rig has 34) */
#define MA_SOUND_BYTES_MAX (16u << 20)      /* decoded sounds of one mod */
#define MA_SOUND_SECONDS_MAX 120
#define MA_COLLISION_TRIS_MAX 2048

/* The game's 34 rig joints; palette slot k = joint k + 2,
 * so joints 0 and 1 (bip_sw_ground, bip_sw) carry no vertices. */
#define MA_JOINTS 34
extern const char *const ma_joints[MA_JOINTS];
int ma_joint_index(const char *name);        /* 0..33, or -1 */
const char *ma_joint_closest(const char *name);   /* the rig joint name nearest by edit distance */

/* "own/<name>" with name a-z A-Z 0-9 - _ . and at most one subfolder, no "..": 0, or -1 with the
 * reason in err ("path outside the mod's own folder"). */
int ma_own_path_check(const char *rel, char *err, size_t cap);

typedef struct {
    int nv, ni;
    float (*pos)[3], (*nrm)[3], (*uv)[2];   /* game axes */
    int8_t (*slot)[3];                      /* palette slots (joint - 2) of up to 3 influences, -1 none */
    float (*w)[3];                          /* their weights (sum 1), skinned meshes only */
    uint16_t *idx;                          /* triangle list */
    int skinned;                            /* 1: every vertex has slots and weights */
    float (*col)[3][3];                     /* collision triangles (nodes / meshes named collision*), game axes */
    int ncol;
    float lo[3], hi[3];                     /* the visual mesh's bounds */
    int trimmed;                            /* vertices whose weakest influences were dropped (more than 3) */
    int moved;                              /* 1: a skinned mesh's node has its own transform (ignored) */
    int image;                              /* the first visual primitive's base colour image (-1 none) */
} MaOwnMesh;
/* Reads a .glb (n bytes): every mesh node of the default scene (else of every node), its triangle
 * primitives merged; a node or mesh whose name starts with "collision" gives collision triangles
 * instead. Static meshes take their node's world transform; a skinned mesh is in its skin's bind
 * space (its node transform is ignored), each vertex's joints mapped by their node names to the
 * game's rig, the 3 strongest kept and renormalised. 0, or -1 with the reason in err (not a .glb,
 * external buffers, no triangles, no normals, no UVs, unknown joint (with the closest name), a joint
 * without palette slot, weightless vertices, too many bones, too many vertices, too large). */
int ma_glb_load(const uint8_t *data, size_t n, MaOwnMesh *m, char *err, size_t cap);
void ma_own_mesh_free(MaOwnMesh *m);
/* The base colour image of a .glb's first visual primitive, as encoded bytes (PNG), malloc'd. */
uint8_t *ma_glb_image(const uint8_t *data, size_t n, size_t *len, char *err, size_t cap);
/* A PNG or DDS (BC1 / BC3 with its full mip chain; texpack_dds_parse) image: RGBA texels
 * (malloc'd) of at most MODASSETS_TEX_MAX a side. 0, or -1 with the reason in err. */
int ma_image_load(const uint8_t *data, size_t n, int *w, int *h, uint8_t **rgba, char *err, size_t cap);
/* A RIFF WAVE (PCM 8/16/24/32-bit, float 32; mono or stereo) or Ogg Vorbis sound: interleaved
 * 16-bit samples (malloc'd). 0, or -1 with the reason in err. */
int ma_sound_decode(const uint8_t *data, size_t n, int16_t **pcm, int *channels, int *rate, int *frames, char *err, size_t cap);
/* A 16-bit PCM WAV file (44-byte header): 0 or -1. */
int ma_wav_write(const char *path, const int16_t *pcm, int channels, int rate, int frames);

#ifdef __cplusplus
}
#endif
#endif
