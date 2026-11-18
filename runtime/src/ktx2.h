/* KTX2 (Basis Universal) texture-pack files transcoded to BC blocks, with the vendored Basis
 * Universal transcoder (third_party/basisu, Apache-2.0; its zstd decoder BSD-3-Clause). The pack
 * logic (names, priority, checks, fallback to the .png) is texpack.c's: main registers
 * ktx2_transcode with texpack_set_ktx2.
 *
 * Accepted: a 2D texture (one face, no array layers or depth) with a full mip chain, in one of
 *   ETC1S without alpha   -> BC1 (no 3-colour blocks: the renderer samples BC1 alpha)
 *   ETC1S with alpha      -> BC3
 *   UASTC LDR 4x4, XUBC7  -> BC7 (XUBC7 is BC7 data supercompressed: it comes back bit-exact)
 * HDR, ASTC/XUASTC (other block sizes) and video files are refused with a reason. The KTX2 transfer
 * function is ignored: like every pack texture the result is sampled as UNORM (no sRGB decode).
 * Thread-safe after ktx2_init (each call has its own transcoder state). Tested by tests/ktx2_test.c. */
#ifndef KTX2_H
#define KTX2_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "texpack.h"   /* a C header: inside the block for ktx2.cpp */

/* The transcoder's global tables; once, before any ktx2_transcode (cheap, idempotent). */
void ktx2_init(void);
/* buf: the whole file. NULL and *img filled (malloc'd levels one after the other, as
 * texpack_load_image returns them), or a static reason with *img untouched. */
const char *ktx2_transcode(const uint8_t *buf, size_t n, TexpackImage *img);

#ifdef __cplusplus
}
#endif
#endif
