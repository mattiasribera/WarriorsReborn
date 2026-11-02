/* Game-independent test of the texture dump / replacement pack helpers (src/texpack.c): the
 * content hash (level 0 texels, independent of guest addresses, linear row padding and the mip
 * count), PNG write/read round trip, mip chain generation, the replacement pack index (file
 * names, lookup, loading with mips), the key -> hash memo, and the dump directory (one PNG per
 * hash, manifest.csv with draw counts, merged across runs). Usage: texpack_test [parent dir]
 * Each run works in a fresh directory made under the parent ($TMPDIR or /tmp by default), so a
 * stale manifest from an earlier run or the current directory cannot change the result; it is
 * removed when every check passes and kept (path printed) for a failing run. */
#define _XOPEN_SOURCE 700
#include "../src/texpack.h"
#include "../src/xtex.h"
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static uint32_t fmt2d(uint32_t code, uint32_t levels, uint32_t ulog, uint32_t vlog) {
    return (2u << 4) | (code << 8) | (levels << 16) | (ulog << 20) | (vlog << 24);
}

/* Reference FNV-1a 64, written independently of src/texpack.c. */
static uint64_t ref_fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static void test_hash(void) {
    XTexInfo t, t2;
    uint8_t data[64 * 4 * 2];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7 + 3);
    xtex_info(fmt2d(0x06, 1, 2, 2), 0, &t);   /* A8R8G8B8 4x4: 64 bytes */
    uint32_t hdr[4] = {0x06, 4, 4, 0};
    uint64_t want = ref_fnv(ref_fnv(0xcbf29ce484222325ull, hdr, sizeof hdr), data, 64);
    uint64_t h = texpack_hash(&t, data);
    CHECK(h == want, "hash = FNV-1a 64 over {code, w, h, 0} + level 0 bytes: %016llx (want %016llx)",
          (unsigned long long)h, (unsigned long long)want);
    CHECK(texpack_hash(&t, data) == h, "hash is deterministic");
    xtex_info(fmt2d(0x06, 3, 2, 2), 0, &t2);
    data[64] ^= 0xFF;   /* level 1 bytes */
    CHECK(texpack_hash(&t2, data) == h, "mip count and lower levels do not change the hash");
    data[5] ^= 1;
    CHECK(texpack_hash(&t, data) != h, "one changed level 0 byte changes the hash");
    data[5] ^= 1;
    xtex_info(fmt2d(0x07, 1, 2, 2), 0, &t2);   /* X8R8G8B8: same size, other format */
    CHECK(texpack_hash(&t2, data) != h, "another format code gives another hash");
    /* LIN_A8R8G8B8 4x2 with a 64-byte pitch: only the 16 texel bytes of each row count. */
    xtex_info((2u << 4) | (0x12u << 8) | (1u << 16), 3 | (1u << 12) | (0u << 24), &t);
    uint8_t lin[128];
    memset(lin, 0xAB, sizeof lin);
    for (int i = 0; i < 16; i++) lin[i] = lin[64 + i] = (uint8_t)i;
    uint64_t hl = texpack_hash(&t, lin);
    memset(lin + 16, 0x11, 48);
    memset(lin + 80, 0x22, 48);
    CHECK(texpack_hash(&t, lin) == hl, "linear row padding is not hashed (pitch %u)", t.pitch);
}

static void test_png(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/rt.png", dir);
    uint32_t px[6] = {0xFF102030, 0x80FFFFFF, 0x00000000, 0xFF0000FF, 0x7F00FF00, 0x01FF0000}, w = 0, h = 0;
    CHECK(texpack_png_write(path, 3, 2, px) == 0, "PNG written");
    uint32_t *back = texpack_png_read(path, &w, &h);
    CHECK(back && w == 3 && h == 2 && !memcmp(back, px, sizeof px), "PNG round trip keeps B8G8R8A8 texels and alpha (%ux%u)", w, h);
    if (back && w == 3 && h == 2) {
        FILE *f = fopen(path, "rb");
        unsigned char sig[8] = {0};
        if (f) { if (fread(sig, 1, 8, f) != 8) sig[0] = 0; fclose(f); }
        CHECK(sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G', "file has the PNG signature");
    }
    free(back);
    snprintf(path, sizeof path, "%s/missing.png", dir);
    CHECK(texpack_png_read(path, &w, &h) == NULL, "missing file reads as NULL");
}

static void test_mips(void) {
    CHECK(texpack_mip_count(1, 1) == 1 && texpack_mip_count(4, 2) == 3 && texpack_mip_count(1024, 256) == 11 &&
          texpack_mip_count(6, 3) == 3, "mip counts: 1x1 %u, 4x2 %u, 1024x256 %u, 6x3 %u", texpack_mip_count(1, 1),
          texpack_mip_count(4, 2), texpack_mip_count(1024, 256), texpack_mip_count(6, 3));
    /* 4x2: left 2x2 block black/white -> gray, right block all red; 1x1 = average. */
    uint32_t px[8] = {0xFF000000, 0xFFFFFFFF, 0xFFFF0000, 0xFFFF0000,
                      0xFFFFFFFF, 0x00000000, 0xFFFF0000, 0xFFFF0000};
    uint32_t levels = 0;
    uint32_t *m = texpack_mips(px, 4, 2, &levels);
    CHECK(m && levels == 3, "4x2 chain has 3 levels (%u)", levels);
    if (m && levels == 3) {
        CHECK(!memcmp(m, px, sizeof px), "level 0 is the image");
        CHECK(m[8] == 0xBF808080u && m[9] == 0xFFFF0000u, "level 1 = 2x2 box averages: %08x %08x", m[8], m[9]);
        CHECK(m[10] == 0xDFC04040u, "level 2 = average of level 1: %08x", m[10]);
    }
    free(m);
    /* Odd sizes: 3x1 -> 1x1 (edge texels clamp). */
    uint32_t odd[3] = {0xFF000000, 0xFF000000, 0xFFFFFFFF};
    m = texpack_mips(odd, 3, 1, &levels);
    CHECK(m && levels == 2 && m[3] == 0xFF000000u, "3x1 -> 1x1 from the first 2x1 block: levels %u texel %08x", levels, m ? m[3] : 0);
    free(m);
}

static void test_pack(const char *dir) {
    char sub[512], path[600];
    snprintf(sub, sizeof sub, "%s/pack", dir);
    mkdir(sub, 0755);
    uint32_t a[16], b[4];
    for (int i = 0; i < 16; i++) a[i] = 0xFF000000u | (uint32_t)i * 0x10101u;
    for (int i = 0; i < 4; i++) b[i] = 0x80FF00FFu;
    snprintf(path, sizeof path, "%s/00000000deadbeef.png", sub);
    texpack_png_write(path, 4, 4, a);
    snprintf(path, sizeof path, "%s/0123456789ABCDEF.png", sub);   /* upper-case hex accepted */
    texpack_png_write(path, 2, 2, b);
    snprintf(path, sizeof path, "%s/notahash.png", sub);
    texpack_png_write(path, 2, 2, b);
    snprintf(path, sizeof path, "%s/manifest.csv", sub);
    FILE *f = fopen(path, "w");
    if (f) { fputs("hash\n", f); fclose(f); }
    int n = texpack_open(sub);
    CHECK(n == 2, "pack indexes the two hash-named PNGs (%d)", n);
    CHECK(texpack_has(0xdeadbeefull) && texpack_has(0x0123456789abcdefull) && !texpack_has(0x1234ull),
          "lookup by hash: present, present, absent");
    uint32_t w = 0, h = 0, levels = 0;
    uint32_t *m = texpack_load(0xdeadbeefull, &w, &h, &levels);
    CHECK(m && w == 4 && h == 4 && levels == 3 && !memcmp(m, a, sizeof a) && m[16] == 0xFF030303u,
          "load: 4x4 with 3 mip levels, level 0 = file, level 1 first texel %08x", m ? m[16] : 0);
    free(m);
    CHECK(texpack_load(0x1234ull, &w, &h, &levels) == NULL, "absent hash loads nothing");
    texpack_disable(0xdeadbeefull);
    CHECK(!texpack_has(0xdeadbeefull) && !texpack_load(0xdeadbeefull, &w, &h, &levels) && texpack_has(0x0123456789abcdefull),
          "a disabled replacement is no longer offered, the others are");
    texpack_close();
    CHECK(!texpack_has(0xdeadbeefull), "closed pack has nothing");
    snprintf(path, sizeof path, "%s/nonexistent", dir);
    CHECK(texpack_open(path) < 0, "missing directory fails");
    texpack_close();
}

/* Layered packs: texpack_add puts a second directory under the first; a hash in both loads from
 * the first, a hash only in the second still loads, texpack_open starts over with one pack. */
static void test_layers(const char *dir) {
    char top[512], base[512], path[600];
    snprintf(top, sizeof top, "%s/top", dir);
    snprintf(base, sizeof base, "%s/base", dir);
    mkdir(top, 0755);
    mkdir(base, 0755);
    uint32_t t1[1] = {0xFF0000FFu}, b1[4] = {0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u}, b2[1] = {0xFFFF0000u};
    snprintf(path, sizeof path, "%s/00000000000000c1.png", top);
    texpack_png_write(path, 1, 1, t1);
    snprintf(path, sizeof path, "%s/00000000000000c1.png", base);
    texpack_png_write(path, 2, 2, b1);
    snprintf(path, sizeof path, "%s/00000000000000c2.png", base);
    texpack_png_write(path, 1, 1, b2);
    CHECK(texpack_open(top) == 1 && texpack_pack_count() == 1, "first pack: 1 texture, 1 pack");
    int n = texpack_add(base);
    CHECK(n == 1 && texpack_pack_count() == 2, "second pack adds only its new key (%d), 2 packs (%d)", n, texpack_pack_count());
    uint32_t w = 0, h = 0, levels = 0, *m = texpack_load(0xc1, &w, &h, &levels);
    CHECK(m && w == 1 && h == 1 && m[0] == t1[0], "key in both packs loads from the first (%ux%u %08x)", w, h, m ? m[0] : 0);
    free(m);
    m = texpack_load(0xc2, &w, &h, &levels);
    CHECK(m && w == 1 && m[0] == b2[0], "key only in the second pack still loads (%08x)", m ? m[0] : 0);
    free(m);
    snprintf(path, sizeof path, "%s/nonexistent", dir);
    CHECK(texpack_add(path) < 0 && texpack_pack_count() == 2 && texpack_has(0xc1), "an unreadable extra pack is skipped");
    CHECK(texpack_open(base) == 2 && texpack_pack_count() == 1, "open starts over with one pack");
    m = texpack_load(0xc1, &w, &h, &levels);
    CHECK(m && w == 2 && m[0] == b1[0], "single pack behaves as before (%ux%u)", w, h);
    free(m);
    texpack_close();
    CHECK(texpack_pack_count() == 0 && !texpack_active(), "closed: no packs");
}

static void test_memo(void) {
    uint64_t h = 0;
    CHECK(!texpack_key_hash(0x55, &h), "unknown key");
    for (uint64_t k = 1; k <= 5000; k++) texpack_key_set(k * 0x9E3779B97F4A7C15ull | 1, k * 3);
    int ok = 1;
    for (uint64_t k = 1; k <= 5000; k++) ok &= texpack_key_hash(k * 0x9E3779B97F4A7C15ull | 1, &h) && h == k * 3;
    CHECK(ok, "5000 keys remembered across table growth");
}

static int count_lines(const char *path, char *last, size_t n) {
    FILE *f = fopen(path, "r");
    char line[512];
    int c = 0;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        c++;
        if (strstr(line, "00000000000000aa")) snprintf(last, n, "%s", line);
    }
    fclose(f);
    return c;
}

static void test_dump(const char *dir) {
    char sub[512], path[600], line[512] = "";
    snprintf(sub, sizeof sub, "%s/dump", dir);
    CHECK(texpack_dump_open(sub) == 0 && texpack_dump_active(), "dump directory opened (created)");
    XTexInfo t;
    xtex_info(fmt2d(0x0C, 3, 2, 2), 0, &t);   /* DXT1 4x4 */
    uint32_t px[16];
    for (int i = 0; i < 16; i++) px[i] = 0xFF00FF00u;
    CHECK(texpack_dump_texture(0xaa, &t, px, 7) == 1, "new texture written");
    CHECK(texpack_dump_texture(0xaa, &t, px, 9) == 0, "same hash not written again");
    texpack_dump_draw(0xaa);
    texpack_dump_draw(0xaa);
    px[3] = 0x00000000;
    texpack_dump_texture(0xbb, &t, px, 11);
    CHECK(texpack_dump_close() == 2, "manifest lists 2 textures");
    snprintf(path, sizeof path, "%s/00000000000000aa.png", sub);
    uint32_t w = 0, h = 0, *back = texpack_png_read(path, &w, &h);
    CHECK(back && w == 4 && h == 4 && back[0] == 0xFF00FF00u, "dumped PNG is level 0 (%ux%u)", w, h);
    free(back);
    snprintf(path, sizeof path, "%s/manifest.csv", sub);
    int n = count_lines(path, line, sizeof line);
    CHECK(n == 3 && !strcmp(line, "00000000000000aa,0x0C,4,4,3,0,0,2,7\n"), "manifest: %d lines, row %s", n, line);
    /* A second run into the same directory: counts add up, first frame kept, no new rows. */
    texpack_dump_open(sub);
    CHECK(texpack_dump_texture(0xaa, &t, px, 3) == 0, "texture from an earlier run not rewritten");
    texpack_dump_draw(0xaa);
    texpack_dump_close();
    n = count_lines(path, line, sizeof line);
    CHECK(n == 3 && !strcmp(line, "00000000000000aa,0x0C,4,4,3,0,0,3,7\n"), "merged manifest: %d lines, row %s", n, line);
    snprintf(path, sizeof path, "%s/00000000000000bb.png", sub);
    back = texpack_png_read(path, &w, &h);
    free(back);
    CHECK(back != NULL, "second texture dumped");
    char bb[512] = "";
    snprintf(path, sizeof path, "%s/manifest.csv", sub);
    FILE *f = fopen(path, "r");
    char l2[512];
    while (f && fgets(l2, sizeof l2, f)) if (strstr(l2, "00000000000000bb")) snprintf(bb, sizeof bb, "%s", l2);
    if (f) fclose(f);
    CHECK(!strcmp(bb, "00000000000000bb,0x0C,4,4,3,0,1,0,11\n"), "texture with transparent texels has_alpha 1: %s", bb);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

int main(int argc, char **argv) {
    const char *parent = argc > 1 ? argv[1] : getenv("TMPDIR");
    if (!parent || !*parent) parent = "/tmp";
    char dir[512];
    snprintf(dir, sizeof dir, "%s/texpack_test.XXXXXX", parent);
    if (!mkdtemp(dir)) {
        perror(dir);
        return 1;
    }
    test_hash();
    test_png(dir);
    test_mips();
    test_pack(dir);
    test_layers(dir);
    test_memo();
    test_dump(dir);
    if (failures) {
        printf("%d FAILED (files kept in %s)\n", failures, dir);
        return 1;
    }
    nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    printf("ALL OK\n");
    return 0;
}
