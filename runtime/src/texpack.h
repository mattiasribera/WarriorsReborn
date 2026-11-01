/* Texture dumps and replacement packs (Dolphin/PCSX2-style), kept free of HLE and renderer state
 * (used by src/hle_d3d.c, tested by tools/texpack_test.c).
 *
 * Content hash: FNV-1a 64 over a 16-byte header {D3DFORMAT code, width, height, 0} (uint32 LE)
 * followed by the level 0 texels exactly as the guest stores them (swizzled bytes, DXT blocks;
 * for LIN_* textures each row's w * bpp / 8 texel bytes without the 64-byte pitch padding). Only
 * level 0 is hashed: a replacement replaces the whole image and its mips are generated on load,
 * so the guest's mip count and lower levels do not change the hash. Guest addresses never enter
 * it, so the same texture gets the same hash across runs and wherever it is loaded. File names
 * are the hash as 16 lower-case hex digits plus ".png".
 *
 * Images are B8G8R8A8 texels (uint32_t 0xAARRGGBB, xtex's decode format); PNG files are RGBA8
 * (stb_image / stb_image_write, runtime/third_party/stb). Not thread-safe: one host thread, except
 * texpack_load_file and the PNG/mip helpers, which touch no shared state. */
#ifndef TEXPACK_H
#define TEXPACK_H
#include "xtex.h"
#include <stdint.h>

uint64_t texpack_hash(const XTexInfo *t, const uint8_t *data);

/* PNG of w x h texels; returns 0 on success. */
int texpack_png_write(const char *path, uint32_t w, uint32_t h, const uint32_t *bgra);
/* Any PNG (other bit depths and channel counts are expanded to RGBA8); malloc'd texels or NULL. */
uint32_t *texpack_png_read(const char *path, uint32_t *w, uint32_t *h);

/* Levels of a full chain down to 1x1 (each level max(1, w >> l) x max(1, h >> l)). */
uint32_t texpack_mip_count(uint32_t w, uint32_t h);
/* Full mip chain (levels stored one after the other, the layout renderer_texture_create takes):
 * each level is a 2x2 box filter of the previous one (rounded; odd edges clamp). Malloc'd. */
uint32_t *texpack_mips(const uint32_t *bgra, uint32_t w, uint32_t h, uint32_t *levels);

/* Replacement pack: indexes <hash>.png files directly in dir (other files are ignored); images
 * are read only when loaded. Returns the number indexed, or -1 when dir cannot be read.
 * texpack_open starts over with one pack; texpack_add layers another pack under the ones already
 * open (a hash found in an earlier pack keeps that pack's file) and returns the number of hashes
 * it added, or -1 (nothing changes) when dir cannot be read. */
int texpack_open(const char *dir);
int texpack_add(const char *dir);
int texpack_pack_count(void);   /* packs open */
int texpack_active(void);
int texpack_has(uint64_t hash);
/* Loads a replacement with its mip chain (texpack_mips). NULL when absent or unreadable. */
uint32_t *texpack_load(uint64_t hash, uint32_t *w, uint32_t *h, uint32_t *levels);
/* The same in two steps, so the slow part can run on another thread: the replacement's file path
 * (malloc'd; NULL when absent), then the image with its mip chain from that file. */
char *texpack_path(uint64_t hash);
uint32_t *texpack_load_file(const char *path, uint32_t *w, uint32_t *h, uint32_t *levels);
/* Removes a replacement from the index (e.g. after its file failed to load). */
void texpack_disable(uint64_t hash);
void texpack_close(void);

/* Memo of cache key (xtex_key, never 0) -> content hash, so each guest texture is hashed once. */
int texpack_key_hash(uint64_t key, uint64_t *hash);
void texpack_key_set(uint64_t key, uint64_t hash);

/* Dump directory (created if missing): one <hash>.png (level 0) per unique texture and
 * manifest.csv with "hash,format,w,h,levels,linear,has_alpha,draws,first_frame" (format = D3DFORMAT
 * code in hex, has_alpha = some texel alpha below 255, draws = stage bindings by draws, first_frame
 * = presented frame number when first seen). An existing manifest is read first, so runs into the
 * same directory merge: known textures are not rewritten, draw counts add up. Returns 0 or -1. */
int texpack_dump_open(const char *dir);
int texpack_dump_active(void);
/* Writes the PNG of a texture not seen before (returns 1; 0 when known, -1 on a write error). */
int texpack_dump_texture(uint64_t hash, const XTexInfo *t, const uint32_t *bgra, uint32_t frame);
void texpack_dump_draw(uint64_t hash);   /* one draw sampled the texture */
/* Rewrites manifest.csv; returns the number of textures listed (-1 on error). */
int texpack_dump_flush(void);
/* Flushes and closes the dump. */
int texpack_dump_close(void);
#endif
