/* Game-independent test of the built-in replacement textures (src/builtin_tex.c): the joint is
 * found for its hash and nothing for other hashes, the format/size pre-filter, its texels and mip
 * chain, and its priority over a texture pack holding the same hash (synthetic PNGs written under
 * a fresh directory made in the parent directory given, $TMPDIR or /tmp; removed when all pass).
 * Usage: builtin_tex_test [parent dir] */
#define _XOPEN_SOURCE 700
#include "../runtime/src/builtin_tex.h"
#include "../runtime/src/builtin_joint.h"
#include "../runtime/src/texpack.h"
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int rm_entry(const char *p, const struct stat *s, int f, struct FTW *w) {
    (void)s; (void)f; (void)w;
    return remove(p);
}

static int write_png(const char *dir, uint64_t hash, uint32_t color) {
    char path[512];
    uint32_t px[4 * 4];
    for (int i = 0; i < 16; i++) px[i] = color;
    snprintf(path, sizeof path, "%s/%016llx.png", dir, (unsigned long long)hash);
    return texpack_png_write(path, 4, 4, px);
}

int main(int argc, char **argv) {
    const char *parent = argc > 1 ? argv[1] : getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    const uint64_t other = 0x0123456789abcdefull, pack_only = 0x1111222233334444ull;

    const BuiltinTexture *b = builtin_texture_find(BUILTIN_JOINT_HASH);
    CHECK(b != NULL, "the joint is built in for 8f3dc7227bda2c45");
    CHECK(!builtin_texture_find(other) && !builtin_texture_find(0) && !builtin_texture_find(BUILTIN_JOINT_HASH ^ 1),
          "no built-in texture for other hashes");
    if (b) {
        CHECK(b->guest_code == 0x0C && b->guest_w == 16 && b->guest_h == 16, "it replaces a 16x16 DXT1 texture");
        CHECK(b->w == BUILTIN_JOINT_WIDTH && b->h == BUILTIN_JOINT_HEIGHT && !memcmp(b->texels, builtin_joint_texels, sizeof builtin_joint_texels),
              "it is the generated 64x64 joint (builtin_joint.h)");
        uint32_t levels = 0;
        const uint32_t *mips = builtin_texture_mips(b, &levels);
        CHECK(mips && levels == texpack_mip_count(b->w, b->h) && !memcmp(mips, b->texels, (size_t)b->w * b->h * 4),
              "mip chain of %u levels starting with the texels", levels);
        uint32_t again = 0;
        CHECK(builtin_texture_mips(b, &again) == mips && again == levels, "the mip chain is built once");
    }

    XTexInfo t = {0};
    t.code = 0x0C; t.w = 16; t.h = 16; t.dxt = 1;
    CHECK(builtin_texture_candidate(&t), "16x16 DXT1 is a candidate for the lookup");
    t.w = 32;
    CHECK(!builtin_texture_candidate(&t), "32x16 DXT1 is not");
    t.w = 16; t.code = 0x0E;
    CHECK(!builtin_texture_candidate(&t), "16x16 DXT3 is not");

    char dir[512];
    snprintf(dir, sizeof dir, "%s/builtin_tex_test.XXXXXX", parent);
    if (!mkdtemp(dir)) { perror(dir); return 1; }
    CHECK(!write_png(dir, BUILTIN_JOINT_HASH, 0xFFFF00FFu) && !write_png(dir, pack_only, 0xFF00FF00u), "synthetic pack written");
    CHECK(texpack_open(dir) == 2 && texpack_has(BUILTIN_JOINT_HASH), "the pack holds the joint's hash too");
    CHECK(builtin_texture_source(BUILTIN_JOINT_HASH, 1) == TEXSRC_BUILTIN, "the built-in joint wins over the pack");
    CHECK(builtin_texture_source(BUILTIN_JOINT_HASH, 0) == TEXSRC_BUILTIN, "and applies with no pack active");
    CHECK(builtin_texture_source(pack_only, 1) == TEXSRC_PACK, "other pack entries still come from the pack");
    CHECK(builtin_texture_source(other, 1) == TEXSRC_GUEST, "a texture in neither stays the guest's");
    builtin_textures_off = 1;
    CHECK(!builtin_texture_find(BUILTIN_JOINT_HASH) && builtin_texture_source(BUILTIN_JOINT_HASH, 1) == TEXSRC_PACK
          && !builtin_texture_candidate(&(XTexInfo){.code = 0x0C, .w = 16, .h = 16, .dxt = 1}),
          "the internal opt-out falls back to the pack");
    builtin_textures_off = 0;
    texpack_close();

    if (failures) printf("%d FAILED (files kept in %s)\n", failures, dir);
    else {
        nftw(dir, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
        printf("ALL OK\n");
    }
    return failures != 0;
}
