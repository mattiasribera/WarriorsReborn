/* Built-in replacement textures: original art compiled into the runtime and applied by default,
 * ahead of any texture pack (src/hle_d3d.c resolve_texture; tested by tests/builtin_tex_test.c).
 * Today one entry: the hand-rolled joint (tools/joint_texture/make_joint.py) for the cigarette prop.
 *
 * Entries are keyed by the texpack content hash (src/texpack.h), which covers the guest format
 * and size, so builtin_texture_candidate (format + size of an entry) is an exact pre-filter: only
 * those textures need hashing for this lookup. Host thread only. */
#ifndef BUILTIN_TEX_H
#define BUILTIN_TEX_H
#include "xtex.h"
#include <stdint.h>

typedef struct {
    uint64_t hash;                        /* texpack_hash of the guest texture it replaces */
    uint32_t guest_code, guest_w, guest_h;   /* that texture's D3DFORMAT code and size */
    uint32_t w, h;                        /* replacement size */
    const uint32_t *texels;               /* w x h, 0xAARRGGBB (B8G8R8A8), row-major */
} BuiltinTexture;

#define BUILTIN_JOINT_HASH 0x8f3dc7227bda2c45ull

/* Internal opt-out for tests (no user flag): nonzero disables every built-in texture. */
extern int builtin_textures_off;

/* A guest texture with the format and size of some entry (worth hashing for the lookup). */
int builtin_texture_candidate(const XTexInfo *t);
/* Entry for a content hash, or NULL. */
const BuiltinTexture *builtin_texture_find(uint64_t hash);
/* Full mip chain of an entry (texpack_mips layout), built once and kept; NULL on allocation failure. */
const uint32_t *builtin_texture_mips(const BuiltinTexture *b, uint32_t *levels);

/* Where a texture with this content hash comes from: a built-in entry wins over the packs. */
enum { TEXSRC_GUEST, TEXSRC_BUILTIN, TEXSRC_PACK };
int builtin_texture_source(uint64_t hash, int pack_active);
#endif
