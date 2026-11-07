/* Texture dumps and replacement packs (Dolphin/PCSX2-style), kept free of HLE and renderer state
 * (used by src/hle_d3d.c, tested by tools/texpack_test.c).
 *
 * Content hash: FNV-1a 64 over a 16-byte header {D3DFORMAT code, width, height, 0} (uint32 LE)
 * followed by the level 0 texels exactly as the guest stores them (swizzled bytes, DXT blocks;
 * for LIN_* textures each row's w * bpp / 8 texel bytes without the 64-byte pitch padding). Only
 * level 0 is hashed: a replacement replaces the whole image and its mips are generated on load,
 * so the guest's mip count and lower levels do not change the hash. Guest addresses never enter
 * it, so the same texture gets the same hash across runs and wherever it is loaded. File names
 * are the hash as 16 lower-case hex digits plus ".png" (or ".dds" in a pack).
 *
 * Images are B8G8R8A8 texels (uint32_t 0xAARRGGBB, xtex's decode format); PNG files are RGBA8
 * (stb_image / stb_image_write, runtime/third_party/stb). Not thread-safe: one host thread, except
 * texpack_load_file, texpack_load_image and the PNG/DDS/mip helpers, which touch no shared state. */
#ifndef TEXPACK_H
#define TEXPACK_H
#include "xtex.h"
#include <stddef.h>
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

/* Formats of a loaded replacement: B8G8R8A8 texels (a PNG, with texpack_mips' chain) or BC
 * blocks as stored in a .dds file (BC1 = DXT1, BC3 = DXT5, BC7; UNORM, row-major 4x4 blocks). */
enum { TEXPACK_BGRA8, TEXPACK_BC1, TEXPACK_BC3, TEXPACK_BC7 };
/* Bytes of one level: w * h * 4 for B8G8R8A8, ceil(w / 4) * ceil(h / 4) blocks of 8 (BC1) or 16
 * bytes otherwise (a level smaller than a block still takes a whole one). */
size_t texpack_level_bytes(uint32_t format, uint32_t w, uint32_t h);

/* A .dds file in memory: the format, size and levels of its payload (offset, size bytes). */
typedef struct {
    uint32_t format, w, h, levels;
    size_t offset, size;
} TexpackDds;
/* Parses a DDS file (n bytes): "DDS " + the 124-byte header, either with the DX10 extension
 * (DXGI BC1_UNORM 71, BC3_UNORM 77 or BC7_UNORM 98; a 2D texture, array size 1, not a cube) or a
 * legacy DXT1/DXT5 FourCC. The mip chain must be complete (texpack_mip_count levels, as PNG
 * replacements get, so both sample alike) and the file exactly header + levels, without trailing
 * bytes. Returns NULL on success, else a static reason (sRGB, other formats, cubes, arrays,
 * volumes, partial chains, truncated or oversized files are all rejected). */
const char *texpack_dds_parse(const uint8_t *buf, size_t n, TexpackDds *out);

/* A replacement ready to upload: format (TEXPACK_*), size, levels stored one after the other
 * (the layout renderer_texture_create takes), malloc'd data. */
typedef struct {
    uint32_t format, w, h, levels;
    void *data;
} TexpackImage;
/* Loads the file texpack_path returned: a .dds as is (texpack_dds_parse), anything else as a PNG
 * with its mip chain. Returns 0, or -1 with a static reason in *why. Touches no shared state. */
int texpack_load_image(const char *path, TexpackImage *img, const char **why);

/* Level 0 of a loaded replacement as B8G8R8A8 texels (w * h, malloc'd; NULL when out of memory or
 * of another format): BC1/BC3/BC7 blocks decoded on the CPU, B8G8R8A8 copied. For code that paints
 * on a replacement (the keyboard/mouse icons); the renderer samples BC blocks itself. */
uint32_t *texpack_image_bgra(const TexpackImage *img);
/* One BC7 block (16 bytes) into 16 B8G8R8A8 texels, row-major; reserved mode 8 gives transparent
 * black, like the GPU. */
void texpack_bc7_block(const uint8_t *b, uint32_t out[16]);

/* Replacement pack: indexes <hash>.png and <hash>.dds files directly in dir (other files are
 * ignored); images are read only when loaded. A pack may hold both files of a hash: the .dds is
 * offered when texpack_set_dds(1) was called (the renderer samples BC blocks), the .png otherwise
 * or after texpack_drop_dds. A hash with only a .dds is absent while .dds files are off. Returns
 * the number of hashes indexed, or -1 when dir cannot be read. texpack_open starts over with one pack; texpack_add layers another pack under the ones already
 * open (a hash found in an earlier pack keeps that pack's files; only when that pack has just a
 * .dds does a later pack's .png become its fallback) and returns the number of hashes
 * it added, or -1 (nothing changes) when dir cannot be read. */
int texpack_open(const char *dir);
int texpack_add(const char *dir);
int texpack_pack_count(void);   /* packs open */
int texpack_active(void);
int texpack_has(uint64_t hash);
void texpack_set_dds(int on);   /* offer .dds files (default off) */
int texpack_dds_count(void);    /* hashes whose offered file is a .dds */
/* Loads a replacement's PNG with its mip chain (texpack_mips). NULL when absent, unreadable or
 * only a .dds. */
uint32_t *texpack_load(uint64_t hash, uint32_t *w, uint32_t *h, uint32_t *levels);
/* The same in two steps, so the slow part can run on another thread: the replacement's file path
 * (malloc'd; NULL when absent; the .dds when offered), then the image with its mip chain from a
 * PNG file (texpack_load_image reads either kind). */
char *texpack_path(uint64_t hash);
uint32_t *texpack_load_file(const char *path, uint32_t *w, uint32_t *h, uint32_t *levels);
/* Removes a replacement from the index (e.g. after its file failed to load). */
void texpack_disable(uint64_t hash);
/* Forgets a replacement's .dds (it failed to load): returns 1 when its .png is offered from now
 * on, else 0 (the replacement is then absent, like texpack_disable). */
int texpack_drop_dds(uint64_t hash);
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
