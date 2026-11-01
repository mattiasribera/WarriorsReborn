/* Game-independent Xbox texture helpers: D3DFORMAT properties, D3DPixelContainer Format/Size
 * parsing, mip level sizes and offsets, Morton (swizzle) addressing, conversion of the 8/16/32-bit
 * formats and CPU DXT1/3/5 decoding to B8G8R8A8, and the key of the host texture cache. Kept free
 * of HLE and renderer state (used by src/hle_d3d.c and tested by tools/xtex_test.c). */
#ifndef XTEX_H
#define XTEX_H
#include <stdint.h>

/* D3DFORMAT codes (bits 8-15 of the Format dword). */
int xtex_fmt_linear(uint32_t code);   /* LIN_* formats: pitch from Size, unnormalized coordinates */
int xtex_fmt_dxt(uint32_t code);      /* 0x0C DXT1, 0x0E DXT2/3, 0x0F DXT4/5 */
int xtex_fmt_bpp(uint32_t code);      /* bits per texel (4 for DXT1, 8 for DXT3/5) */
/* Bytes per row (DXT: per row of 4x4 blocks) of a level w texels wide; linear rows are padded to
 * 64 bytes. */
uint32_t xtex_level_pitch(uint32_t code, uint32_t w);
uint32_t xtex_level_size(uint32_t code, uint32_t w, uint32_t h);

/* Format dword fields. */
#define XTEX_CUBEMAP 0x4u
#define XTEX_DIM(f) (((f) >> 4) & 0xFu)
#define XTEX_CODE(f) (((f) >> 8) & 0xFFu)
#define XTEX_LEVELS(f) (((f) >> 16) & 0xFu)
#define XTEX_ULOG(f) (((f) >> 20) & 0xFu)
#define XTEX_VLOG(f) (((f) >> 24) & 0xFu)
#define XTEX_PLOG(f) (((f) >> 28) & 0xFu)

/* Level 0 of a 2D texture. */
typedef struct {
    uint32_t code, w, h, levels, pitch;   /* pitch of level 0 in bytes (block rows for DXT) */
    int linear, dxt;
} XTexInfo;
/* Parses Format and Size (Size != 0: linear, width-1 | height-1 << 12 | pitch/64-1 << 24).
 * Returns 0, or -1 for what the helpers cannot sample: cube maps, volumes, palettized (P8) and
 * depth formats, unknown codes. */
int xtex_info(uint32_t format, uint32_t size, XTexInfo *t);
uint32_t xtex_level_w(const XTexInfo *t, uint32_t level);
uint32_t xtex_level_h(const XTexInfo *t, uint32_t level);
uint32_t xtex_level_bytes(const XTexInfo *t, uint32_t level);
uint32_t xtex_level_offset(const XTexInfo *t, uint32_t level);   /* from the texture's Data */
uint32_t xtex_total_bytes(const XTexInfo *t);                    /* all levels */

/* Xbox swizzled textures store texels in Morton order: the bits of x and y are interleaved
 * starting with x, and once the smaller power-of-two dimension runs out of bits the remaining
 * bits of the other follow in order. w and h must be powers of two. */
uint32_t xbox_swizzle_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void xbox_unswizzle(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h, uint32_t bytes_per_texel,
                    uint32_t dst_pitch);

/* One texel of an uncompressed format to B8G8R8A8 (0xAARRGGBB). Alpha-less formats give alpha
 * 255, luminance formats replicate L into RGB, A8 gives white with alpha [I]. Returns -1 for
 * formats it does not convert (DXT, YUV, bump, depth, P8). */
int xtex_texel(uint32_t code, const uint8_t *p, uint32_t *out);
/* Decodes one 4x4 DXT block (8 bytes DXT1, 16 bytes DXT3/5) into 16 B8G8R8A8 texels, row-major. */
void xtex_dxt_block(uint32_t code, const uint8_t *b, uint32_t out[16]);
/* Decodes mip level `level` of the texture whose level 0 starts at data into
 * xtex_level_w x xtex_level_h B8G8R8A8 texels (tightly packed). Swizzled levels are unswizzled,
 * DXT blocks are read in row-major block order. Returns 0, or -1 for an unsupported format. */
int xtex_decode_level(const XTexInfo *t, uint32_t level, const uint8_t *data, uint32_t *out);

/* Cache key: the data address, Format and Size dwords and a sparse checksum of the texture bytes
 * (first and last 16 bytes, 64 evenly spaced dwords), so a texture rebuilt at the same address
 * with other content gets a new key without hashing every byte each frame. */
uint64_t xtex_key(uint32_t data, uint32_t format, uint32_t size, const uint8_t *bytes, uint32_t len);
#endif
