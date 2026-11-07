/* --neon-glow: lit and neon signs glow at night. A texture pack may hold an
 * emission mask per texture, named after the texture's content hash (texpack.h):
 *
 *   <hash>.emit.png   grey PNG, any size (the red channel is read)
 *   <hash>.emit.dds   BC4 UNORM (DXGI 80, or the legacy FourCC ATI1 / BC4U); level 0 is read
 *
 * next to the pack's colour textures or in a pack of its own (a mask-only pack works over the
 * original textures; texpack ignores these names). The first pack that has a hash's mask wins;
 * within one pack the .dds wins over the .png. A mask is sampled with the texture's coordinates, so
 * its size need not match the colour texture's. Users make them from their own copy (docs/TEXTURE_PACKS.md).
 *
 * A draw whose stage texture has a mask (src/hle_d3d.c) adds that stage's colour times the mask
 * after the fog mix and writes the mask into the bloom light mask (renderer_set_emission). This
 * file is the game- and renderer-independent part: options, names, the mask index, loading masks
 * into an L8 mip chain and the per-sign flicker. One host thread (the index); the loaders touch no
 * shared state. Tested by tests/neon_glow_test.c. */
#ifndef NEON_GLOW_H
#define NEON_GLOW_H
#include <stddef.h>
#include <stdint.h>

#define NEON_GLOW_STRENGTH_DEFAULT 1.25f
#define NEON_GLOW_BLOOM_DEFAULT 1.0f
#define NEON_GLOW_FLICKER_DEFAULT 0.04f
typedef struct {
    int on;          /* --neon-glow / --no-neon-glow */
    float strength;  /* --neon-glow-strength 0.1-3: how much of the sign's own colour is added */
    float bloom;     /* --neon-glow-bloom 0-1: the mask's weight in the bloom light mask */
    float flicker;   /* --neon-glow-flicker 0-0.3: per-sign brightness wobble (0 = steady) */
} NeonGlowConfig;
#define NEON_GLOW_DEFAULTS {0, NEON_GLOW_STRENGTH_DEFAULT, NEON_GLOW_BLOOM_DEFAULT, NEON_GLOW_FLICKER_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a neon-glow option, -1 for a missing or out-of-range value (c unchanged). */
int neon_glow_arg(NeonGlowConfig *c, const char *opt, const char *val);

/* "<16 lower-case hex digits>.emit.png" (dds 0) or ".emit.dds" (dds 1) into buf; -1 when n is too
 * small. */
int neon_glow_mask_name(uint64_t hash, int dds, char *buf, size_t n);
/* The hash and kind of a mask file name (hex and extension in any case); -1 for anything else. */
int neon_glow_parse_name(const char *name, uint64_t *hash, int *dds);

/* Mask index. neon_glow_index_add indexes the mask files directly in dir, layered under the packs
 * already added; returns the hashes it added (not already masked by an earlier pack), or -1 (nothing
 * changes) when dir cannot be read. */
int neon_glow_index_add(const char *dir);
int neon_glow_index_count(void);
int neon_glow_has(uint64_t hash);
/* The mask's file path (malloc'd), NULL when the hash has none. */
char *neon_glow_mask_path(uint64_t hash);
/* Drops a hash (its file failed to load). */
void neon_glow_index_forget(uint64_t hash);
void neon_glow_index_clear(void);

/* A mask file (either kind) as an L8 mip chain (levels one after the other, each max(1, w >> l) x
 * max(1, h >> l), the layout renderer_texture_create takes for RENDERER_TEX_L8). Malloc'd, or NULL
 * with a static reason in *why. */
uint8_t *neon_glow_load(const char *path, uint32_t *w, uint32_t *h, uint32_t *levels, const char **why);
/* A .emit.dds in memory: level 0's size and offset. NULL on success, else a static reason (only
 * BC4 UNORM 2D textures; level 0 must be complete). */
const char *neon_glow_dds_parse(const uint8_t *buf, size_t n, uint32_t *w, uint32_t *h, size_t *offset);
/* Decodes w x h texels of BC4 blocks (row-major 4x4 blocks of 8 bytes) into out (w * h bytes). */
void neon_glow_bc4_decode(const uint8_t *blocks, uint32_t w, uint32_t h, uint8_t *out);
/* L8 mip chain of a w x h image (2x2 box, rounded; odd edges clamp, as texpack_mips). Malloc'd. */
uint8_t *neon_glow_l8_mips(const uint8_t *l8, uint32_t w, uint32_t h, uint32_t *levels);

/* Brightness factor of a sign (by its texture hash) at a frame, within [1 - amp, 1]: a slow
 * per-sign hum, a faint buzz and a rare short dip, deterministic (frame count, not wall time), at
 * most amp per frame. amp 0: exactly 1. */
float neon_glow_flicker(uint64_t hash, uint32_t frame, float amp);
#endif
