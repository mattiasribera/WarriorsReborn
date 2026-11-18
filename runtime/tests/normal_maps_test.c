/* Game-independent --normal-maps test: option parsing (defaults, ranges, other options untouched);
 * the dry highlights (--dry-highlights, --dry-highlights-strength); companion file names (made from a hash and from a base texture's file name, parsed back, base
 * colour textures and other files refused); the companion index over temporary packs (relief and
 * specular maps per hash, the first pack wins, the .dds wins within a pack only while .dds files
 * are offered, a later pack's .png backs a .dds-only hash, counts, forget, clear); --parallax options
 * and the height in a normal map's alpha (B8G8R8A8 levels, BC7, BC1), remembered per hash. */
#include "../src/normal_maps.h"
#include "../src/texpack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_args(void) {
    NormalMapsConfig c = NORMAL_MAPS_DEFAULTS;
    CHECK(!c.on && c.normal_strength == 1.0f && c.specular_strength == 1.0f && c.baked == 1.0f, "args: off by default, strengths 1.0, baked relief 1.0");
    CHECK(normal_maps_arg(&c, "--normal-maps", NULL) == 1 && c.on, "args: --normal-maps turns it on");
    CHECK(normal_maps_arg(&c, "--no-normal-maps", NULL) == 1 && !c.on, "args: --no-normal-maps turns it off");
    CHECK(normal_maps_arg(&c, "--normal-strength", "2.5") == 2 && c.normal_strength == 2.5f, "args: --normal-strength 2.5");
    CHECK(normal_maps_arg(&c, "--normal-strength", "0") == 2 && c.normal_strength == 0.0f, "args: --normal-strength 0 (flat)");
    CHECK(normal_maps_arg(&c, "--normal-strength", "4.5") == -1 && c.normal_strength == 0.0f, "args: strength above 4 refused, unchanged");
    CHECK(normal_maps_arg(&c, "--normal-strength", "-1") == -1, "args: a negative strength refused");
    CHECK(normal_maps_arg(&c, "--normal-strength", NULL) == -1, "args: missing strength refused");
    CHECK(normal_maps_arg(&c, "--normal-strength", "1x") == -1, "args: a non-number refused");
    CHECK(normal_maps_arg(&c, "--specular-strength", "0.5") == 2 && c.specular_strength == 0.5f, "args: --specular-strength 0.5");
    CHECK(normal_maps_arg(&c, "--specular-strength", "5") == -1 && c.specular_strength == 0.5f, "args: specular above 4 refused");
    CHECK(normal_maps_arg(&c, "--specular-strength", "") == -1, "args: an empty value refused");
    CHECK(normal_maps_arg(&c, "--normal-baked", "0") == 2 && c.baked == 0.0f, "args: --normal-baked 0 leaves the baked light untouched");
    CHECK(normal_maps_arg(&c, "--normal-baked", "1.5") == -1 && c.baked == 0.0f, "args: baked above 1 refused");
    CHECK(normal_maps_arg(&c, "--neon-glow", NULL) == 0 && normal_maps_arg(&c, "--normal", NULL) == 0, "args: other options are not consumed");
    CHECK(!c.on, "args: parsing values leaves the switch alone");
    NormalMapsConfig d = NORMAL_MAPS_DEFAULTS;
    CHECK(!d.dry && d.dry_strength == 1.0f, "args: dry highlights off by default, strength 1.0");
    CHECK(normal_maps_arg(&d, "--dry-highlights", NULL) == 1 && d.dry && !d.on, "args: --dry-highlights turns them on (not the relief)");
    CHECK(normal_maps_arg(&d, "--no-dry-highlights", NULL) == 1 && !d.dry, "args: --no-dry-highlights turns them off");
    CHECK(normal_maps_arg(&d, "--dry-highlights-strength", "2") == 2 && d.dry_strength == 2.0f, "args: --dry-highlights-strength 2");
    CHECK(normal_maps_arg(&d, "--dry-highlights-strength", "3.5") == -1 && d.dry_strength == 2.0f, "args: dry strength above 3 refused, unchanged");
    CHECK(normal_maps_arg(&d, "--dry-highlights-strength", "0.05") == -1, "args: dry strength below 0.1 refused");
    CHECK(normal_maps_arg(&d, "--dry-highlights-strength", NULL) == -1, "args: missing dry strength refused");
    CHECK(!d.dry && d.specular_strength == 1.0f, "args: the dry strength leaves the switch and the specular strength alone");
}

static void test_parallax_args(void) {
    NormalMapsConfig c = NORMAL_MAPS_DEFAULTS;
    CHECK(!c.parallax && c.parallax_depth == 0.015f, "parallax: off by default, depth 0.015");
    CHECK(normal_maps_arg(&c, "--parallax", NULL) == 1 && c.parallax && !c.on && !c.dry, "parallax: --parallax turns it on (not the relief)");
    CHECK(normal_maps_arg(&c, "--no-parallax", NULL) == 1 && !c.parallax, "parallax: --no-parallax turns it off");
    CHECK(normal_maps_arg(&c, "--parallax-depth", "0.05") == 2 && c.parallax_depth == 0.05f, "parallax: --parallax-depth 0.05");
    CHECK(normal_maps_arg(&c, "--parallax-depth", "0.2") == -1 && c.parallax_depth == 0.05f, "parallax: depth above 0.1 refused, unchanged");
    CHECK(normal_maps_arg(&c, "--parallax-depth", "0.001") == -1, "parallax: depth below 0.005 refused");
    CHECK(normal_maps_arg(&c, "--parallax-depth", NULL) == -1, "parallax: missing depth refused");
    CHECK(!c.parallax && c.normal_strength == 1.0f, "parallax: the depth leaves the switch and the other values alone");
}

/* A BC7 mode 6 block of one colour: 7-bit endpoints with p-bit 0 (8-bit value v & ~1), indices 0. */
static void bc7_solid(uint8_t b[16], uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
    memset(b, 0, 16);
    int pos = 0;
    #define PUT(v, n) do { for (int k_ = 0; k_ < (n); k_++, pos++) if (((v) >> k_) & 1) b[pos >> 3] |= (uint8_t)(1u << (pos & 7)); } while (0)
    PUT(1u << 6, 7);   /* mode 6 */
    const uint8_t ch[4] = {r, g, bl, a};
    for (int c = 0; c < 4; c++) {
        PUT(ch[c] >> 1, 7);
        PUT(ch[c] >> 1, 7);
    }
    #undef PUT
}

static void test_height(void) {
    /* B8G8R8A8 64 x 64 with its mip chain: alpha 255 everywhere -> no height; one deep corner -> height */
    uint32_t px[64 * 64];
    for (int i = 0; i < 64 * 64; i++) px[i] = 0xFF8080FFu;
    uint32_t levels;
    uint32_t *m = texpack_mips(px, 64, 64, &levels);
    CHECK(m && normal_maps_image_height(TEXPACK_BGRA8, 64, 64, levels, m) == 0, "height: alpha 255 everywhere has none");
    free(m);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) px[y * 64 + x] = 0x408080FFu;
    m = texpack_mips(px, 64, 64, &levels);
    CHECK(m && normal_maps_image_height(TEXPACK_BGRA8, 64, 64, levels, m) == 1, "height: a deep corner is a height");
    free(m);
    for (int i = 0; i < 64 * 64; i++) px[i] = 0xFB8080FFu;   /* 251: within the flat margin */
    m = texpack_mips(px, 64, 64, &levels);
    CHECK(m && normal_maps_image_height(TEXPACK_BGRA8, 64, 64, levels, m) == 0, "height: alpha 251 counts as flat");
    free(m);
    /* 256 x 256: the 64 x 64 level is read (a pit of 2 x 2 texels at level 0 averages away at 64 x 64
     * only partly: a 16 x 16 pit is seen) */
    static uint32_t big[256 * 256];
    for (int i = 0; i < 256 * 256; i++) big[i] = 0xFF8080FFu;
    for (int y = 100; y < 116; y++)
        for (int x = 100; x < 116; x++) big[y * 256 + x] = 0x008080FFu;
    m = texpack_mips(big, 256, 256, &levels);
    CHECK(m && normal_maps_image_height(TEXPACK_BGRA8, 256, 256, levels, m) == 1, "height: read from a small level of a big map");
    free(m);
    CHECK(normal_maps_image_height(TEXPACK_BGRA8, 256, 256, 1, big) == 1, "height: a single big level is read as it is");
    /* BC7 (relief packs' .dds): a 4 x 4 map of one block */
    uint8_t blk[16];
    uint32_t dec[16];
    bc7_solid(blk, 128, 128, 255, 255);
    texpack_bc7_block(blk, dec);
    CHECK(dec[0] == 0xFE8080FEu, "height: the test's BC7 block decodes as built (%08x)", dec[0]);
    CHECK(normal_maps_image_height(TEXPACK_BC7, 4, 4, 1, blk) == 0, "height: an opaque BC7 map has none");
    bc7_solid(blk, 128, 128, 255, 101);
    CHECK(normal_maps_image_height(TEXPACK_BC7, 4, 4, 1, blk) == 1, "height: a BC7 map with alpha 101 has one");
    uint8_t bc1[8] = {0};
    CHECK(normal_maps_image_height(TEXPACK_BC1, 4, 4, 1, bc1) == 0, "height: a BC1 map has none");
    CHECK(normal_maps_image_height(TEXPACK_BGRA8, 0, 0, 1, px) == 0 && normal_maps_image_height(TEXPACK_BGRA8, 4, 4, 1, NULL) == 0,
          "height: empty or missing data has none");
}

static void test_names(void) {
    char b[64];
    CHECK(normal_maps_name(0x0123456789ABCDEFull, NORMAL_MAPS_RELIEF, 0, b, sizeof b) == 0 && !strcmp(b, "0123456789abcdef_n.png"), "name: relief png %s", b);
    CHECK(normal_maps_name(0xFull, NORMAL_MAPS_SPECULAR, 1, b, sizeof b) == 0 && !strcmp(b, "000000000000000f_s.dds"), "name: specular dds %s", b);
    CHECK(normal_maps_name(1, NORMAL_MAPS_RELIEF, 0, b, 10) == -1, "name: a short buffer is refused");
    CHECK(normal_maps_name(1, 7, 0, b, sizeof b) == -1, "name: an unknown kind is refused");
    CHECK(normal_maps_companion("126b81c6b2820b74.png", NORMAL_MAPS_RELIEF, b, sizeof b) == 0 && !strcmp(b, "126b81c6b2820b74_n.png"),
          "companion: <hash>.png -> %s", b);
    CHECK(normal_maps_companion("126B81C6B2820B74.DDS", NORMAL_MAPS_SPECULAR, b, sizeof b) == 0 && !strcmp(b, "126B81C6B2820B74_s.DDS"),
          "companion: case kept, .dds -> %s", b);
    CHECK(normal_maps_companion("dir/126b81c6b2820b74.png", NORMAL_MAPS_RELIEF, b, sizeof b) == 0 && !strcmp(b, "dir/126b81c6b2820b74_n.png"),
          "companion: a path keeps its directory (%s)", b);
    CHECK(normal_maps_companion("126b81c6b2820b74_n.png", NORMAL_MAPS_RELIEF, b, sizeof b) == -1, "companion: a companion has no companion");
    CHECK(normal_maps_companion("126b81c6b2820b74.emit.png", NORMAL_MAPS_RELIEF, b, sizeof b) == -1, "companion: an emission mask is not a base");
    CHECK(normal_maps_companion("notahash.png", NORMAL_MAPS_RELIEF, b, sizeof b) == -1, "companion: not a hash");
    CHECK(normal_maps_companion("126b81c6b2820b74.png", NORMAL_MAPS_RELIEF, b, 8) == -1, "companion: a short buffer is refused");
    uint64_t h = 0;
    int kind = -1, dds = -1;
    CHECK(!normal_maps_parse_name("0123456789abcdef_n.png", &h, &kind, &dds) && h == 0x0123456789ABCDEFull && kind == NORMAL_MAPS_RELIEF && dds == 0,
          "parse: relief png");
    CHECK(!normal_maps_parse_name("0123456789ABCDEF_S.DDS", &h, &kind, &dds) && kind == NORMAL_MAPS_SPECULAR && dds == 1, "parse: upper case specular dds");
    CHECK(normal_maps_parse_name("0123456789abcdef.png", &h, &kind, &dds) == -1, "parse: a colour replacement is not a companion");
    CHECK(normal_maps_parse_name("0123456789abcdef.emit.png", &h, &kind, &dds) == -1, "parse: an emission mask is not a companion");
    CHECK(normal_maps_parse_name("0123456789abcdef_x.png", &h, &kind, &dds) == -1, "parse: unknown suffix refused");
    CHECK(normal_maps_parse_name("0123456789abcdef_n.jpg", &h, &kind, &dds) == -1, "parse: other extensions refused");
    CHECK(normal_maps_parse_name("0123456789abcdeg_n.png", &h, &kind, &dds) == -1, "parse: non-hex refused");
    CHECK(normal_maps_parse_name("0123456789abcdef_n.png.bak", &h, &kind, &dds) == -1, "parse: trailing text refused");
    CHECK(normal_maps_parse_name("0123456789abcde_n.png", &h, &kind, &dds) == -1, "parse: a short hash refused");
}

static void touch(const char *dir, const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) fclose(f);
}

static int ends_with(const char *s, const char *e) {
    size_t a = s ? strlen(s) : 0, b = strlen(e);
    return a >= b && !strcmp(s + a - b, e);
}

static void test_index(const char *root) {
    char a[400], b[400], c[400];
    snprintf(a, sizeof a, "%s/a", root);
    snprintf(b, sizeof b, "%s/b", root);
    snprintf(c, sizeof c, "%s/c", root);
    mkdir(a, 0700);
    mkdir(b, 0700);
    mkdir(c, 0700);
    /* pack a: hash 1 relief png + dds, hash 1 specular png, hash 2 relief dds only, a base texture
     * and an emission mask (ignored) */
    touch(a, "0000000000000001_n.png");
    touch(a, "0000000000000001_n.dds");
    touch(a, "0000000000000001_s.png");
    touch(a, "0000000000000002_n.dds");
    touch(a, "0000000000000001.png");
    touch(a, "0000000000000003.emit.png");
    touch(a, "readme.txt");
    /* pack b: hash 1 relief png (shadowed by a), hash 2 relief png (backs a's .dds), hash 4 specular */
    touch(b, "0000000000000001_n.png");
    touch(b, "0000000000000002_n.png");
    touch(b, "0000000000000004_S.PNG");
    normal_maps_index_clear();
    normal_maps_set_dds(0);
    CHECK(normal_maps_index_add(a) == 3, "index: pack a has 3 companions (relief 1, specular 1, relief 2)");
    CHECK(normal_maps_index_add(b) == 1, "index: pack b adds 1 (specular 4; relief 1 and 2 were in a)");
    CHECK(normal_maps_index_add(c) == 0, "index: an empty pack adds nothing");
    char missing[420];
    snprintf(missing, sizeof missing, "%s/nope", root);
    CHECK(normal_maps_index_add(missing) == -1, "index: an unreadable pack is -1");
    CHECK(normal_maps_has(1, NORMAL_MAPS_RELIEF) && normal_maps_has(1, NORMAL_MAPS_SPECULAR), "has: hash 1 has both maps");
    CHECK(!normal_maps_has(3, NORMAL_MAPS_RELIEF) && !normal_maps_has(1, 9), "has: masks and unknown kinds are not companions");
    char *p = normal_maps_path(1, NORMAL_MAPS_RELIEF);
    CHECK(p && !strncmp(p, a, strlen(a)) && ends_with(p, "/0000000000000001_n.png"), "path: .dds off -> pack a's .png (%s)", p ? p : "NULL");
    free(p);
    p = normal_maps_path(2, NORMAL_MAPS_RELIEF);
    CHECK(p && !strncmp(p, b, strlen(b)) && ends_with(p, "/0000000000000002_n.png"), "path: .dds off, a .dds-only hash falls back to pack b's .png");
    free(p);
    p = normal_maps_path(4, NORMAL_MAPS_SPECULAR);
    CHECK(p && ends_with(p, "/0000000000000004_S.PNG"), "path: the name as found (case kept)");
    free(p);
    CHECK(normal_maps_count(NORMAL_MAPS_RELIEF) == 2 && normal_maps_count(NORMAL_MAPS_SPECULAR) == 2, "count: 2 relief, 2 specular");
    normal_maps_set_dds(1);
    p = normal_maps_path(1, NORMAL_MAPS_RELIEF);
    CHECK(p && ends_with(p, "/0000000000000001_n.dds"), "path: .dds on -> the .dds wins within pack a");
    free(p);
    p = normal_maps_path(2, NORMAL_MAPS_RELIEF);
    CHECK(p && !strncmp(p, a, strlen(a)) && ends_with(p, "_n.dds"), "path: .dds on -> pack a's .dds before pack b's .png");
    free(p);
    p = normal_maps_path(1, NORMAL_MAPS_SPECULAR);
    CHECK(p && ends_with(p, "_s.png"), "path: .dds on, no .dds -> the .png");
    free(p);
    normal_maps_forget(2, NORMAL_MAPS_RELIEF);   /* its .dds failed: the .png of pack b next */
    p = normal_maps_path(2, NORMAL_MAPS_RELIEF);
    CHECK(p && !strncmp(p, b, strlen(b)) && ends_with(p, "_n.png"), "forget: a failed .dds leaves the .png");
    free(p);
    normal_maps_forget(2, NORMAL_MAPS_RELIEF);   /* and the .png failed too */
    CHECK(!normal_maps_has(2, NORMAL_MAPS_RELIEF) && !normal_maps_path(2, NORMAL_MAPS_RELIEF), "forget: then the hash has no relief");
    CHECK(normal_maps_has(1, NORMAL_MAPS_RELIEF), "forget: other hashes keep theirs");
    normal_maps_index_clear();
    CHECK(!normal_maps_has(1, NORMAL_MAPS_RELIEF) && normal_maps_count(NORMAL_MAPS_RELIEF) == 0, "clear: nothing indexed");
    normal_maps_set_dds(0);
}

/* .ktx2 companions: a BC file like a .dds (offered while .dds files are), the .dds wins within one
 * pack whatever the name order, a .ktx2 backs a hash whose only other file is a .png, forget drops
 * a failed .ktx2 to the .png. */
static void test_ktx2_index(const char *root) {
    char k[400];
    snprintf(k, sizeof k, "%s/k", root);
    mkdir(k, 0700);
    touch(k, "0000000000000011_n.ktx2");   /* 11 relief: .ktx2 and .dds -> the .dds */
    touch(k, "0000000000000011_n.dds");
    touch(k, "0000000000000010_n.dds");    /* 10 relief: the same, other name order */
    touch(k, "0000000000000010_n.KTX2");
    touch(k, "0000000000000012_s.ktx2");   /* 12 specular: .ktx2 + .png */
    touch(k, "0000000000000012_s.png");
    touch(k, "0000000000000013_n.ktx");    /* not a companion */
    uint64_t h;
    int kind, dds;
    CHECK(normal_maps_parse_name("0123456789ABCDEF_S.KTX2", &h, &kind, &dds) == 0 && kind == NORMAL_MAPS_SPECULAR && dds == 1,
          "parse: a .ktx2 companion is a BC file (kind %d, dds %d)", kind, dds);
    normal_maps_index_clear();
    normal_maps_set_dds(0);
    CHECK(normal_maps_index_add(k) == 3, "ktx2 index: 3 companions");
    char *p = normal_maps_path(0x12, NORMAL_MAPS_SPECULAR);
    CHECK(p && ends_with(p, "/0000000000000012_s.png"), "ktx2: .dds off -> the .png (%s)", p ? p : "NULL");
    free(p);
    CHECK(!normal_maps_has(0x10, NORMAL_MAPS_RELIEF), "ktx2: .dds off -> a BC-only hash is absent");
    normal_maps_set_dds(1);
    p = normal_maps_path(0x11, NORMAL_MAPS_RELIEF);
    char *q = normal_maps_path(0x10, NORMAL_MAPS_RELIEF);
    CHECK(p && q && ends_with(p, "/0000000000000011_n.dds") && ends_with(q, "/0000000000000010_n.dds"),
          "ktx2: the .dds wins over the .ktx2 within a pack (%s, %s)", p ? p : "NULL", q ? q : "NULL");
    free(p);
    free(q);
    p = normal_maps_path(0x12, NORMAL_MAPS_SPECULAR);
    CHECK(p && ends_with(p, "/0000000000000012_s.ktx2"), "ktx2: .dds on -> the .ktx2 before the .png (%s)", p ? p : "NULL");
    free(p);
    normal_maps_forget(0x12, NORMAL_MAPS_SPECULAR);
    p = normal_maps_path(0x12, NORMAL_MAPS_SPECULAR);
    CHECK(p && ends_with(p, "/0000000000000012_s.png"), "ktx2: forget a failed .ktx2 -> the .png (%s)", p ? p : "NULL");
    free(p);
    normal_maps_index_clear();
    normal_maps_set_dds(0);
}

/* The index remembers a loaded map's height per hash (unknown before; none for a hash it lacks). */
static void test_height_index(const char *root) {
    char d[512], f[600];
    snprintf(d, sizeof d, "%s/height", root);
    mkdir(d, 0755);
    snprintf(f, sizeof f, "%s/00000000000000aa_n.png", d);
    FILE *fp = fopen(f, "wb");
    if (fp) fclose(fp);
    normal_maps_index_clear();
    CHECK(normal_maps_index_add(d) == 1, "height index: one map indexed");
    CHECK(normal_maps_height(0xAA) == -1, "height index: unknown until loaded");
    normal_maps_set_height(0xAA, 1);
    CHECK(normal_maps_height(0xAA) == 1, "height index: remembered");
    normal_maps_set_height(0xAA, 0);
    CHECK(normal_maps_height(0xAA) == 0, "height index: and cleared");
    normal_maps_set_height(0xBB, 1);
    CHECK(normal_maps_height(0xBB) == -1, "height index: a hash without a map stays unknown");
    normal_maps_index_clear();
    CHECK(normal_maps_height(0xAA) == -1, "height index: cleared with the index");
}

/* Likewise whether a loaded specular map is glossy (--reflections sssr's glass). */
static void test_glossy_index(const char *root) {
    char d[512], f[600];
    snprintf(d, sizeof d, "%s/glossy", root);
    mkdir(d, 0755);
    snprintf(f, sizeof f, "%s/00000000000000aa_s.png", d);
    FILE *fp = fopen(f, "wb");
    if (fp) fclose(fp);
    normal_maps_index_clear();
    CHECK(normal_maps_index_add(d) == 1, "glossy index: one map indexed");
    CHECK(normal_maps_glossy(0xAA) == -1, "glossy index: unknown until loaded");
    normal_maps_set_glossy(0xAA, 1);
    CHECK(normal_maps_glossy(0xAA) == 1, "glossy index: remembered");
    normal_maps_set_glossy(0xAA, 0);
    CHECK(normal_maps_glossy(0xAA) == 0, "glossy index: and cleared");
    normal_maps_set_glossy(0xBB, 1);
    CHECK(normal_maps_glossy(0xBB) == -1, "glossy index: a hash without a map stays unknown");
    normal_maps_index_clear();
    CHECK(normal_maps_glossy(0xAA) == -1, "glossy index: cleared with the index");
}

/* --reflections sssr's glass: a specular map is glossy when some texel of its first level of at most
 * 64 a side mirrors (shine B and gloss A as wet.frag glass_k: above NORMAL_MAPS_GLOSSY_MIN). */
static void test_glossy(void) {
    uint32_t px[64 * 64];
    uint32_t levels;
    for (int i = 0; i < 64 * 64; i++) px[i] = 0xFF206000u;   /* ARGB: B 0 (no material) */
    uint32_t *m = texpack_mips(px, 64, 64, &levels);
    CHECK(m && normal_maps_image_glossy(TEXPACK_BGRA8, 64, 64, levels, m) == 0, "glossy: no material is not glossy");
    free(m);
    for (int i = 0; i < 64 * 64; i++) px[i] = 0x3320600Au;   /* brick: B 0.04, A 0.2 */
    m = texpack_mips(px, 64, 64, &levels);
    CHECK(m && normal_maps_image_glossy(TEXPACK_BGRA8, 64, 64, levels, m) == 0, "glossy: brick is not");
    free(m);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) px[y * 64 + x] = 0xF02060E6u;   /* a window: B 0.9, A 0.94 */
    m = texpack_mips(px, 64, 64, &levels);
    CHECK(m && normal_maps_image_glossy(TEXPACK_BGRA8, 64, 64, levels, m) == 1, "glossy: a window in a brick atlas is");
    free(m);
    uint8_t blk[16];
    bc7_solid(blk, 0x20, 0x60, 0xE6, 0xF0);
    CHECK(normal_maps_image_glossy(TEXPACK_BC7, 4, 4, 1, blk) == 1, "glossy: a BC7 glass block");
    bc7_solid(blk, 0x20, 0x60, 0x0A, 0x33);
    CHECK(normal_maps_image_glossy(TEXPACK_BC7, 4, 4, 1, blk) == 0, "glossy: a BC7 brick block");
    CHECK(normal_maps_image_glossy(TEXPACK_BC1, 4, 4, 1, blk) == 0 && normal_maps_image_glossy(TEXPACK_BGRA8, 4, 4, 1, NULL) == 0,
          "glossy: BC1 (no alpha) and no data are not");
}

int main(void) {
    char root[] = "/tmp/normal_maps_testXXXXXX";
    const char *tmp = getenv("TMPDIR");
    char buf[512];
    char *dir;
    if (tmp && *tmp) {
        snprintf(buf, sizeof buf, "%s/normal_maps_testXXXXXX", tmp);
        dir = mkdtemp(buf);
    } else {
        dir = mkdtemp(root);
    }
    if (!dir) {
        printf("FAIL cannot make a temporary directory\n");
        return 1;
    }
    test_args();
    test_parallax_args();
    test_height();
    test_glossy();
    test_names();
    test_index(dir);
    test_ktx2_index(dir);
    test_height_index(dir);
    test_glossy_index(dir);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) printf("note: could not remove %s\n", dir);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
