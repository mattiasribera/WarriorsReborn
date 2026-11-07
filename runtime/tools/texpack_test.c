/* Game-independent test of the texture dump / replacement pack helpers (src/texpack.c): the
 * content hash (level 0 texels, independent of guest addresses, linear row padding and the mip
 * count), PNG write/read round trip, mip chain generation, the replacement pack index (file
 * names, lookup, loading with mips), DDS parsing (DX10 and legacy BC1/BC3/BC7 headers, level
 * sizes, rejected files) and .dds/.png preference and fallback, the key -> hash memo, and the
 * dump directory (one PNG per
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

/* ---- DDS (BC packs) ---- */
enum { DDSD_MIPS = 0x20000, DDPF_FOURCC = 0x4, DDSCAPS2_CUBEMAP = 0x200 };

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* A DDS file in buf: header (DX10 when dxgi != 0, else FourCC), mips levels (0 = no mip count
 * flag), payload bytes p = 0, 1, 2 ... Returns the file size. */
static size_t make_dds(uint8_t *buf, uint32_t w, uint32_t h, uint32_t mips, uint32_t dxgi, const char *fourcc, size_t payload) {
    size_t hdr = 4 + 124 + (dxgi ? 20 : 0);
    memset(buf, 0, hdr);
    memcpy(buf, "DDS ", 4);
    uint8_t *d = buf + 4;
    put32(d + 0, 124);
    put32(d + 4, 0x1007 | (mips ? DDSD_MIPS : 0));
    put32(d + 8, h);
    put32(d + 12, w);
    put32(d + 24, mips);
    put32(d + 72, 32);
    put32(d + 76, DDPF_FOURCC);
    memcpy(d + 80, dxgi ? "DX10" : fourcc, 4);
    put32(d + 104, 0x1000 | (mips > 1 ? 0x400008 : 0));
    if (dxgi) {
        put32(d + 124, dxgi);
        put32(d + 128, 3);   /* TEXTURE2D */
        put32(d + 136, 1);   /* array size */
    }
    for (size_t i = 0; i < payload; i++) buf[hdr + i] = (uint8_t)i;
    return hdr + payload;
}

static void test_dds_parse(void) {
    static uint8_t buf[4096];
    TexpackDds d;
    CHECK(texpack_level_bytes(TEXPACK_BC1, 1, 1) == 8 && texpack_level_bytes(TEXPACK_BC7, 5, 3) == 32 &&
              texpack_level_bytes(TEXPACK_BC3, 4, 4) == 16 && texpack_level_bytes(TEXPACK_BC1, 8, 4) == 16 &&
              texpack_level_bytes(TEXPACK_BGRA8, 3, 2) == 24,
          "level bytes: BC blocks round up to 4x4 (1x1 BC1 = 8, 5x3 BC7 = 32), B8G8R8A8 4 per texel");
    /* BC7 8x4, full chain 8x4, 4x2, 2x1, 1x1 = 2 + 1 + 1 + 1 blocks of 16 bytes */
    size_t n = make_dds(buf, 8, 4, 4, 98, NULL, 80);
    const char *why = texpack_dds_parse(buf, n, &d);
    CHECK(!why && d.format == TEXPACK_BC7 && d.w == 8 && d.h == 4 && d.levels == 4 && d.offset == 148 && d.size == 80,
          "DX10 BC7 8x4 with 4 levels: format %u, %ux%u, %u levels, payload %zu bytes at %zu (%s)", d.format, d.w, d.h,
          d.levels, d.size, d.offset, why ? why : "ok");
    n = make_dds(buf, 5, 3, 3, 0, "DXT1", 32);   /* 5x3, 2x1, 1x1: 2 + 1 + 1 blocks of 8 */
    why = texpack_dds_parse(buf, n, &d);
    CHECK(!why && d.format == TEXPACK_BC1 && d.levels == 3 && d.offset == 128 && d.size == 32,
          "legacy DXT1 5x3 (not a multiple of 4) parses as BC1 (%s)", why ? why : "ok");
    n = make_dds(buf, 4, 4, 3, 0, "DXT5", 48);
    why = texpack_dds_parse(buf, n, &d);
    CHECK(!why && d.format == TEXPACK_BC3 && d.size == 48, "legacy DXT5 parses as BC3 (%s)", why ? why : "ok");
    n = make_dds(buf, 4, 4, 3, 71, NULL, 24);
    why = texpack_dds_parse(buf, n, &d);
    CHECK(!why && d.format == TEXPACK_BC1, "DX10 BC1_UNORM parses as BC1 (%s)", why ? why : "ok");
    n = make_dds(buf, 1, 1, 0, 98, NULL, 16);
    why = texpack_dds_parse(buf, n, &d);
    CHECK(!why && d.levels == 1, "1x1 without a mip count is a complete chain (%s)", why ? why : "ok");

    n = make_dds(buf, 8, 4, 4, 98, NULL, 80);
    CHECK(texpack_dds_parse(buf, n - 1, &d) != NULL, "truncated payload rejected");
    CHECK(texpack_dds_parse(buf, 100, &d) != NULL && texpack_dds_parse(buf, 3, &d) != NULL, "truncated header rejected");
    CHECK(texpack_dds_parse(buf, n + 1, &d) != NULL, "trailing bytes rejected");
    buf[0] = 'X';
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "bad magic rejected");
    make_dds(buf, 8, 4, 4, 98, NULL, 80);
    put32(buf + 4, 120);
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "header size other than 124 rejected");
    n = make_dds(buf, 8, 4, 4, 99, NULL, 80);
    CHECK((why = texpack_dds_parse(buf, n, &d)) != NULL, "BC7_UNORM_SRGB rejected (%s)", why ? why : "accepted");
    n = make_dds(buf, 8, 4, 4, 72, NULL, 40);
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "BC1_UNORM_SRGB rejected");
    n = make_dds(buf, 4, 4, 3, 0, "DXT3", 48);
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "DXT3 (BC2) rejected");
    n = make_dds(buf, 8, 4, 2, 98, NULL, 48);
    CHECK((why = texpack_dds_parse(buf, n, &d)) != NULL, "partial mip chain rejected (%s)", why ? why : "accepted");
    n = make_dds(buf, 8, 4, 0, 98, NULL, 32);
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "8x4 without mips rejected (PNG replacements always have a full chain)");
    n = make_dds(buf, 8, 4, 5, 98, NULL, 96);
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "more levels than the chain rejected");
    n = make_dds(buf, 0, 4, 1, 98, NULL, 0);
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "zero width rejected");
    n = make_dds(buf, 8, 4, 4, 98, NULL, 80);
    put32(buf + 4 + 128, 2);   /* DX10 array size */
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "texture array rejected");
    make_dds(buf, 8, 4, 4, 98, NULL, 80);
    put32(buf + 4 + 124 + 8, 4);   /* DX10 misc flag TEXTURECUBE */
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "DX10 cube rejected");
    make_dds(buf, 8, 4, 4, 98, NULL, 80);
    put32(buf + 4 + 108, DDSCAPS2_CUBEMAP);
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "legacy cube caps rejected");
    make_dds(buf, 8, 4, 4, 98, NULL, 80);
    put32(buf + 4 + 124 + 4, 4);   /* TEXTURE3D */
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "volume rejected");
    n = make_dds(buf, 65536, 1, 17, 98, NULL, 0);
    CHECK(texpack_dds_parse(buf, n, &d) != NULL, "65536-wide texture rejected (no huge size arithmetic)");
}

static void write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

static int ends_with(const char *s, const char *tail) {
    size_t a = s ? strlen(s) : 0, b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

/* A pack with .png and .dds files: preference, fallback when .dds files are off or a .dds is
 * dropped, loading either kind, and layering with a .dds-only top pack. */
static void test_dds_pack(const char *dir) {
    char sub[512], low[512], path[600];
    static uint8_t buf[4096];
    snprintf(sub, sizeof sub, "%s/bcpack", dir);
    snprintf(low, sizeof low, "%s/bclow", dir);
    mkdir(sub, 0755);
    mkdir(low, 0755);
    uint32_t px[4] = {0xFF102030u, 0xFF102030u, 0xFF102030u, 0xFF102030u};
    size_t n = make_dds(buf, 8, 4, 4, 98, NULL, 80);
    snprintf(path, sizeof path, "%s/00000000000000a1.png", sub);   /* a1: both */
    texpack_png_write(path, 2, 2, px);
    snprintf(path, sizeof path, "%s/00000000000000a1.dds", sub);
    write_file(path, buf, n);
    snprintf(path, sizeof path, "%s/00000000000000A2.DDS", sub);   /* a2: .dds only (upper case) */
    write_file(path, buf, n);
    snprintf(path, sizeof path, "%s/00000000000000a3.png", sub);   /* a3: .png only */
    texpack_png_write(path, 2, 2, px);
    snprintf(path, sizeof path, "%s/00000000000000a4.png", sub);   /* a4: both, the .dds malformed */
    texpack_png_write(path, 2, 2, px);
    snprintf(path, sizeof path, "%s/00000000000000a4.dds", sub);
    write_file(path, buf, n - 5);
    snprintf(path, sizeof path, "%s/00000000000000a5.dds", sub);   /* a5: .dds here, .png only in the lower pack */
    write_file(path, buf, n);
    snprintf(path, sizeof path, "%s/00000000000000a5.png", low);
    texpack_png_write(path, 2, 2, px);
    snprintf(path, sizeof path, "%s/00000000000000a3.dds", low);   /* a3: the lower pack's .dds never wins */
    write_file(path, buf, n);

    texpack_set_dds(0);
    int got = texpack_open(sub);
    CHECK(got == 5, "a .png and a .dds of one hash index one hash (%d of 5)", got);
    CHECK(texpack_add(low) == 0, "lower pack adds no new hash");
    char *p1 = texpack_path(0xa1), *p5 = texpack_path(0xa5);
    CHECK(ends_with(p1, "/00000000000000a1.png") && !texpack_has(0xa2) && texpack_has(0xa3) && texpack_dds_count() == 0,
          ".dds off: the .png is offered (%s), a .dds-only hash is absent", p1 ? p1 : "NULL");
    CHECK(p5 && strstr(p5, "bclow/00000000000000a5.png"), ".dds off: a .dds-only hash falls back to a lower pack's .png (%s)",
          p5 ? p5 : "NULL");
    free(p1);
    free(p5);
    texpack_set_dds(1);
    p1 = texpack_path(0xa1);
    char *p2 = texpack_path(0xa2), *p3 = texpack_path(0xa3);
    CHECK(ends_with(p1, "/00000000000000a1.dds") && ends_with(p2, "/00000000000000A2.DDS") && texpack_has(0xa2),
          ".dds on: the .dds is preferred (%s, %s)", p1 ? p1 : "NULL", p2 ? p2 : "NULL");
    CHECK(p3 && strstr(p3, "bcpack/00000000000000a3.png"), "a hash keeps the first pack's files (%s)", p3 ? p3 : "NULL");
    CHECK(texpack_dds_count() == 4, ".dds offered for a1, a2, a4, a5: %d", texpack_dds_count());
    TexpackImage im = {0};
    const char *why = NULL;
    int r = texpack_load_image(p1, &im, &why);
    CHECK(!r && im.format == TEXPACK_BC7 && im.w == 8 && im.h == 4 && im.levels == 4 && im.data &&
              ((uint8_t *)im.data)[0] == 0 && ((uint8_t *)im.data)[79] == 79,
          "a .dds loads as its payload, as is (format %u, %ux%u, %u levels)", im.format, im.w, im.h, im.levels);
    free(im.data);
    memset(&im, 0, sizeof im);
    r = texpack_load_image(p3, &im, &why);
    CHECK(!r && im.format == TEXPACK_BGRA8 && im.w == 2 && im.levels == 2 && ((uint32_t *)im.data)[4] == px[0],
          "a .png loads as B8G8R8A8 with its mip chain");
    free(im.data);
    char *p4 = texpack_path(0xa4);
    memset(&im, 0, sizeof im);
    why = NULL;
    r = texpack_load_image(p4, &im, &why);
    CHECK(r && why && !im.data, "a malformed .dds fails with a reason (%s)", why ? why : "none");
    CHECK(texpack_drop_dds(0xa4) == 1, "dropping its .dds leaves the .png");
    free(p4);
    p4 = texpack_path(0xa4);
    CHECK(ends_with(p4, "/00000000000000a4.png") && texpack_has(0xa4), "after the drop the .png is offered (%s)", p4 ? p4 : "NULL");
    CHECK(texpack_drop_dds(0xa2) == 0 && !texpack_has(0xa2), "dropping a .dds-only hash removes it");
    uint32_t w = 0, h = 0, levels = 0, *m = texpack_load(0xa1, &w, &h, &levels);
    CHECK(m && w == 2 && levels == 2, "texpack_load (B8G8R8A8 callers) reads the .png even with .dds on");
    free(m);
    CHECK(texpack_load_image("/nonexistent/00000000000000a1.dds", &im, &why) && why, "missing file fails (%s)", why ? why : "none");
    free(p1);
    free(p2);
    free(p3);
    free(p4);
    texpack_close();
    texpack_set_dds(0);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

/* BC7 decode for painting on a pack's .dds (the keyboard/mouse icons): one random block per mode
 * (bytes from a seeded generator, mode bits forced), texels from an independent BC7 decoder. */
static const struct {
    uint8_t b[16];
    uint32_t px[16];
} bc7_ref[] = {
    {{0x73, 0xdd, 0x8f, 0xdb, 0xec, 0xc7, 0x77, 0x73, 0x82, 0xda, 0x96, 0x30, 0x2f, 0xcd, 0x83, 0x79},
     {0xffc565aeu, 0xffd768a7u, 0xffb563b5u, 0xffe76aa0u, 0xffa78821u, 0xff733110u, 0xffccc52du, 0xffccc52du,
      0xffbba828u, 0xffa78821u, 0xff733110u, 0xffefff39u, 0xffd7d2a0u, 0xffd0e85fu, 0xffdebddeu, 0xffd0e85fu}},   /* mode 0 */
    {{0x7a, 0x44, 0xa8, 0x87, 0xaa, 0x59, 0x24, 0x61, 0x02, 0xff, 0x5f, 0x7b, 0x46, 0xa4, 0xcf, 0x75},
     {0xff43a45eu, 0xffa322eeu, 0xffcf1bd4u, 0xffa322eeu, 0xff879b26u, 0xff56a24fu, 0xff22a979u, 0xffcf1bd4u,
      0xffb11fe6u, 0xff56a24fu, 0xff779d34u, 0xff879b26u, 0xffb11fe6u, 0xffc11ddcu, 0xffa322eeu, 0xff43a45eu}},   /* mode 1 */
    {{0x44, 0x4a, 0xaf, 0xe8, 0xaf, 0xcd, 0xd9, 0xe3, 0x2f, 0xa6, 0x3b, 0xaa, 0x25, 0xde, 0x39, 0xa4},
     {0xff29de8cu, 0xffaee460u, 0xff29de8cu, 0xffaee460u, 0xffefe74au, 0xff6ae176u, 0xffefe74au, 0xff6ae176u,
      0xffad63bdu, 0xff427b18u, 0xff8a6b87u, 0xffad63bdu, 0xff909eb2u, 0xffffe7adu, 0xffc9c4b0u, 0xffc9c4b0u}},   /* mode 2 */
    {{0x18, 0x9d, 0xb3, 0xc1, 0xa6, 0x86, 0xf0, 0x3f, 0x45, 0xbc, 0x41, 0x4a, 0xbb, 0x12, 0xa8, 0x73},
     {0xffcf3523u, 0xff9b4f29u, 0xff8bc565u, 0xff938947u, 0xffbc178au, 0xffcf3523u, 0xffc52655u, 0xff83ff83u,
      0xffcf3523u, 0xffbc178au, 0xffbc178au, 0xffbc178au, 0xffb208bcu, 0xffcf3523u, 0xffb208bcu, 0xffc52655u}},   /* mode 3 */
    {{0xf0, 0x35, 0x02, 0x85, 0xbc, 0x09, 0x28, 0xe9, 0xfc, 0x3d, 0x04, 0x0e, 0xe1, 0xce, 0xae, 0x25},
     {0x75a4179au, 0x42ad0067u, 0x42ad0067u, 0xf78c5233u, 0x42ad009au, 0x75a41767u, 0x42ad0000u, 0xf78c5267u,
      0xde914633u, 0x5ba80c00u, 0x8e9f2300u, 0xf78c5200u, 0x75a41733u, 0x8e9f2300u, 0x5ba80c67u, 0x5ba80c9au}},   /* mode 4 */
    {{0x60, 0x78, 0x7b, 0x48, 0x6a, 0x3a, 0x72, 0x01, 0x68, 0xda, 0xa8, 0xb6, 0x42, 0x3c, 0xa2, 0x38},
     {0xf13e424cu, 0xf05c6262u, 0xed5ca58fu, 0xf13e424cu, 0xf05c6262u, 0xed00a58fu, 0xee008579u, 0xf05c6262u,
      0xf11e424cu, 0xf05c6262u, 0xf01e6262u, 0xf01e6262u, 0xed5ca58fu, 0xee1e8579u, 0xf0006262u, 0xf05c6262u}},   /* mode 5 */
    {{0xc0, 0x6a, 0xce, 0x4d, 0xf5, 0x5d, 0x4a, 0x2c, 0xc4, 0xb4, 0xb4, 0x8a, 0xa9, 0x47, 0x1c, 0xea},
     {0x4ca2d571u, 0x557db33eu, 0x4e9bce67u, 0x5481b643u, 0x4e9bce67u, 0x5481b643u, 0x5384b948u, 0x518cc053u,
      0x5289bd4eu, 0x5384b948u, 0x5190c457u, 0x4e9bce67u, 0x557db33eu, 0x4ba7d977u, 0x5384b948u, 0x5776ab33u}},   /* mode 6 */
    {{0x80, 0x1f, 0xa9, 0x72, 0x55, 0x3d, 0x32, 0xf3, 0x7d, 0xa0, 0x7e, 0x2e, 0x7a, 0x4b, 0x75, 0xe3},
     {0x4120aa61u, 0xebaad3f3u, 0x648154a7u, 0x648154a7u, 0xbe5dcf1cu, 0xb37dc6c3u, 0x4120aa61u, 0x648154a7u,
      0x648154a7u, 0x794db791u, 0xebaad3f3u, 0x648154a7u, 0xbe5dcf1cu, 0x389218ebu, 0xb37dc6c3u, 0xebaad3f3u}},   /* mode 7 */
};

static void test_bc_decode(const char *dir) {
    uint32_t px[16];
    for (int m = 0; m < 8; m++) {
        texpack_bc7_block(bc7_ref[m].b, px);
        CHECK(!memcmp(px, bc7_ref[m].px, sizeof px), "BC7 mode %d block decodes like the reference (texel 0 %08x, want %08x)", m, px[0],
              bc7_ref[m].px[0]);
    }
    uint8_t reserved[16] = {0, 0xff, 0xff};
    texpack_bc7_block(reserved, px);
    CHECK(px[0] == 0 && px[15] == 0, "BC7 reserved mode 8 decodes as transparent black");
    /* 6x5: 2x2 blocks, the right and bottom ones cropped; block i = mode-i reference */
    uint8_t blocks[64];
    for (int i = 0; i < 4; i++) memcpy(blocks + 16 * i, bc7_ref[i].b, 16);
    TexpackImage img = {TEXPACK_BC7, 6, 5, 1, blocks};
    uint32_t *out = texpack_image_bgra(&img);
    CHECK(out && out[0] == bc7_ref[0].px[0] && out[5] == bc7_ref[1].px[1] && out[4 * 6 + 3] == bc7_ref[2].px[3] &&
              out[4 * 6 + 5] == bc7_ref[3].px[1],
          "BC7 image level 0 -> B8G8R8A8, blocks row-major, partial edge blocks cropped");
    free(out);
    /* BC1 (DXT1, 4-colour mode): c0 = white, c1 = black, every index 1 -> black */
    uint8_t bc1[8] = {0xff, 0xff, 0, 0, 0x55, 0x55, 0x55, 0x55};
    img = (TexpackImage){TEXPACK_BC1, 4, 4, 1, bc1};
    out = texpack_image_bgra(&img);
    CHECK(out && out[0] == 0xFF000000u && out[15] == 0xFF000000u, "BC1 image decodes (%08x)", out ? out[0] : 0);
    free(out);
    uint32_t bgra[2] = {0x80112233u, 0xFF445566u};
    img = (TexpackImage){TEXPACK_BGRA8, 2, 1, 1, bgra};
    out = texpack_image_bgra(&img);
    CHECK(out && !memcmp(out, bgra, sizeof bgra), "B8G8R8A8 image is copied");
    free(out);
    /* through a file: a 4x4 BC7 .dds with its 3 levels, level 0 = the mode 6 reference */
    static uint8_t buf[512];
    char path[600];
    size_t n = make_dds(buf, 4, 4, 3, 98, NULL, 48);
    memcpy(buf + n - 48, bc7_ref[6].b, 16);
    snprintf(path, sizeof path, "%s/bc7.dds", dir);
    write_file(path, buf, n);
    const char *why = NULL;
    TexpackImage f;
    out = !texpack_load_image(path, &f, &why) ? texpack_image_bgra(&f) : NULL;
    CHECK(out && !memcmp(out, bc7_ref[6].px, sizeof px), "a loaded BC7 .dds decodes to its level 0 (%s)", why ? why : "loaded");
    free(out);
    if (!why) free(f.data);
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
    test_dds_parse();
    test_dds_pack(dir);
    test_bc_decode(dir);
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
