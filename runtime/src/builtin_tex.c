/* Built-in replacement textures (src/builtin_tex.h). */
#include "builtin_tex.h"
#include "builtin_joint.h"
#include "texpack.h"
#include <stddef.h>

int builtin_textures_off;

static const BuiltinTexture builtin[] = {
    /* The cigarette prop (16x16 DXT1) as a hand-rolled joint: tools/joint_texture/make_joint.py. */
    {BUILTIN_JOINT_HASH, 0x0C, 16, 16, BUILTIN_JOINT_WIDTH, BUILTIN_JOINT_HEIGHT, builtin_joint_texels},
};
#define BUILTIN_COUNT (sizeof builtin / sizeof builtin[0])
static uint32_t *mips[BUILTIN_COUNT], mip_levels[BUILTIN_COUNT];

int builtin_texture_candidate(const XTexInfo *t) {
    if (builtin_textures_off) return 0;
    for (size_t i = 0; i < BUILTIN_COUNT; i++)
        if (t->code == builtin[i].guest_code && t->w == builtin[i].guest_w && t->h == builtin[i].guest_h) return 1;
    return 0;
}

const BuiltinTexture *builtin_texture_find(uint64_t hash) {
    if (builtin_textures_off) return NULL;
    for (size_t i = 0; i < BUILTIN_COUNT; i++)
        if (builtin[i].hash == hash) return &builtin[i];
    return NULL;
}

const uint32_t *builtin_texture_mips(const BuiltinTexture *b, uint32_t *levels) {
    size_t i = (size_t)(b - builtin);
    if (!mips[i]) mips[i] = texpack_mips(b->texels, b->w, b->h, &mip_levels[i]);
    *levels = mips[i] ? mip_levels[i] : 0;
    return mips[i];
}

int builtin_texture_source(uint64_t hash, int pack_active) {
    if (builtin_texture_find(hash)) return TEXSRC_BUILTIN;
    return pack_active && texpack_has(hash) ? TEXSRC_PACK : TEXSRC_GUEST;
}
