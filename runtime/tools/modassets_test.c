/* Game-independent test of the mod asset engine (src/modassets.h): Python-compatible random
 * numbers, the assets.ini parser and its errors (file:line), cache keys, the PNG writer, the build
 * steps on a synthetic disc (two tiny characters, two textures and a skin table made here: a head
 * swap, a texture paste, headgear cut out as a rigid hat, a hide file, a colour mask with polish,
 * velvet, scaling, an atlas and the shadow-box frame) and the cache folder (hit, miss, invalidation,
 * --rebuild-mod-assets, failed builds leave nothing, budgets). No game data.
 * Usage: modassets_test [tmpdir] */
#include "../src/disc.h"
#include "../src/meshpack.h"
#include "../src/modassets.h"
#include "../src/pyrand.h"
#include "../src/texpack.h"
#include "../src/xtex.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;
#define CHECK(c, msg)                                                   \
    do {                                                                \
        if (!(c)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);        \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void test_pyrand(void) {
    PyRand r;
    pyrand_seed(&r, 7);   /* python3: random.Random(7).random() x3 */
    CHECK(pyrand_random(&r) == 0.32383276483316237, "Random(7) 1");
    CHECK(pyrand_random(&r) == 0.15084917392450192, "Random(7) 2");
    CHECK(pyrand_random(&r) == 0.6509344730398537, "Random(7) 3");
    pyrand_seed(&r, 11);
    CHECK(pyrand_uniform(&r, 0, 6.3) == 2.8499911871118573 && pyrand_uniform(&r, 0, 6.3) == 3.5265660323071244, "Random(11).uniform(0, 6.3)");
    pyrand_seed(&r, 1000);
    int ok = 1;
    const int want[5] = {6, 1, 6, 5, 1};
    for (int i = 0; i < 5; i++) ok &= pyrand_randrange(&r, 0, 10, 1) == want[i];
    CHECK(ok && pyrand_getrandbits(&r, 32) == 2009648469u, "Random(1000).randrange(10) x5, getrandbits(32)");
    pyrand_seed(&r, 0);
    CHECK(pyrand_random(&r) == 0.8444218515250481, "Random(0)");
    pyrand_seed(&r, (1ll << 40) + 5);
    CHECK(pyrand_random(&r) == 0.5043802970418443, "Random(2**40 + 5): two key words");
    pyrand_seed(&r, -3);
    CHECK(pyrand_random(&r) == 0.23796462709189137, "Random(-3) seeds with |seed|");
    pyrand_seed(&r, 5);
    const int dice[5] = {5, 3, 6, 3, 6}, steps[3] = {94, 80, 73};
    ok = 1;
    for (int i = 0; i < 5; i++) ok &= pyrand_randint(&r, 1, 6) == dice[i];
    for (int i = 0; i < 3; i++) ok &= pyrand_randrange(&r, 3, 100, 7) == steps[i];
    CHECK(ok, "Random(5).randint(1, 6) x5, randrange(3, 100, 7) x3");
}

static int parse_err(const char *text, const char *want) {
    MaRecipe r;
    char err[256] = "";
    int rc = ma_recipe_parse(text, strlen(text), "assets.ini", &r, err, sizeof err);
    if (!rc) ma_recipe_free(&r);
    if (rc == 0 || !strstr(err, want)) {
        printf("  got \"%s\" for want \"%s\"\n", err, want);
        return 0;
    }
    return 1;
}

static void test_parser(void) {
    const char *ok = "\xEF\xBB\xBF; a comment\nformat = 1\n\n[source a]   ; trailing comment\nchar = warr_cl ; Cleon\r\n[mesh b]\nfrom = a\n";
    MaRecipe r;
    char err[256];
    CHECK(ma_recipe_parse(ok, strlen(ok), "assets.ini", &r, err, sizeof err) == 0, "a valid recipe parses");
    CHECK(r.format == 1 && r.nsec == 2 && r.nlines == 2, "two sections, two lines");
    CHECK(!strcmp(r.sec[0].kind, "source") && !strcmp(r.sec[0].name, "a") && r.sec[0].line == 4, "section kind, name and line");
    CHECK(!strcmp(r.lines[0].key, "char") && !strcmp(r.lines[0].value, "warr_cl") && r.lines[0].line == 5, "comment and CR stripped");
    CHECK(r.sec[1].first == 1 && r.sec[1].n == 1, "lines per section");
    ma_recipe_free(&r);
    CHECK(parse_err("[source a]\nchar = x\n", "assets.ini:1: no `format = 1` line"), "no format line");
    CHECK(parse_err("format = 2\n", "assets.ini:1: this game reads assets.ini format 1, not 2"), "format 2");
    CHECK(parse_err("format = 1\nchar = x\n", "assets.ini:2: before the first section only `format = 1`; unknown key char"), "key before a section");
    CHECK(parse_err("format = 1\n[model a]\n", "assets.ini:2: unknown section kind model"), "unknown kind");
    CHECK(parse_err("format = 1\n[source a]\n[mesh a]\n", "assets.ini:3: a second section named a"), "duplicate name");
    CHECK(parse_err("format = 1\n[source a/b]\n", "assets.ini:2: a name is"), "bad name");
    CHECK(parse_err("format = 1\n[source {code}]\n", "assets.ini:2: a name is"), "braces only in a [layer] name");
    {
        MaRecipe lr;
        const char *t = "format = 1\n[set s]\nx = warr_x\n[layer {code}-{stage}]\nfor = s\n";
        CHECK(ma_recipe_parse(t, strlen(t), "assets.ini", &lr, err, sizeof err) == 0 && lr.nsec == 2 && !strcmp(lr.sec[1].name, "{code}-{stage}"),
              "[set] and [layer] sections parse");
        ma_recipe_free(&lr);
    }
    CHECK(parse_err("format = 1\n[source]\n", "assets.ini:2: a section line is [kind name]"), "section without a name");
    CHECK(parse_err("format = 1\n[source a]\nchar warr_cl\n", "assets.ini:3: expected key = value"), "no =");
    CHECK(parse_err("format = 1\n[source a]\nchar =\n", "assets.ini:3: expected key = value"), "empty value");
    char longline[800];
    snprintf(longline, sizeof longline, "format = 1\n[source a]\nchar = %0700d\n", 0);
    CHECK(parse_err(longline, "assets.ini:3:"), "line too long");
}

static void test_key(void) {
    const char *a = "format = 1\n[hide x]\nhost = dyn_dest_ltb\n";
    uint64_t k = ma_cache_key(a, strlen(a), "1.0", 42);
    CHECK(k == ma_cache_key(a, strlen(a), "1.0", 42), "the key is stable");
    CHECK(k != ma_cache_key(a, strlen(a) - 1, "1.0", 42), "the recipe bytes change it");
    CHECK(k != ma_cache_key(a, strlen(a), "1.1", 42), "the mod version changes it");
    CHECK(k != ma_cache_key(a, strlen(a), "1.0", 43), "the disc fingerprint changes it");
}

static void test_png(const char *tmp) {
    uint8_t px[2 * 3 * 4];
    for (int i = 0; i < 24; i++) px[i] = (uint8_t)(i * 10);
    char path[600];
    snprintf(path, sizeof path, "%s/ma_test.png", tmp);
    CHECK(ma_png_write(path, 2, 3, px) == 0, "PNG written");
    uint32_t w = 0, h = 0, *bgra = texpack_png_read(path, &w, &h);
    int same = bgra && w == 2 && h == 3;
    for (int i = 0; same && i < 6; i++)
        same = bgra[i] == ((uint32_t)px[4 * i + 3] << 24 | (uint32_t)px[4 * i] << 16 | (uint32_t)px[4 * i + 1] << 8 | px[4 * i + 2]);
    CHECK(same, "PNG reads back texel for texel");
    free(bgra);
    remove(path);
}

/* ---- a synthetic disc ---- */

typedef struct {
    uint8_t *b;
    size_t n, cap;
} Buf;

static void need(Buf *b, size_t n) {
    if (b->n + n <= b->cap) return;
    b->cap = (b->n + n) * 2 + 4096;
    b->b = realloc(b->b, b->cap);
}
static void put(Buf *b, const void *p, size_t n) {
    need(b, n);
    memcpy(b->b + b->n, p, n);
    b->n += n;
}
static void put32(Buf *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, x, 4);
}
static void putf(Buf *b, float f) { put(b, &f, 4); }
static void pad(Buf *b, size_t to, uint8_t v) {
    need(b, to > b->n ? to - b->n : 0);
    while (b->n < to) b->b[b->n++] = v;
}

typedef struct {
    float x, y, z, u, v;
} V;

/* A character payload: vertices vs (normal +z), triangles idx, inverse bind slot 4 a translation by -1.8 in z. */
static void character(Buf *b, const V *vs, int nv, const uint16_t *idx, int ni) {
    b->n = 0;
    const uint32_t d1 = (uint32_t)nv * 32, d2 = (d1 + (uint32_t)nv * 18 + 15) & ~15u, tail = d2 + (uint32_t)ni * 2;
    put32(b, 1), put32(b, 0x76), put32(b, 3), put32(b, tail);
    pad(b, 0x80, 0);
    for (int i = 0; i < nv; i++) {
        putf(b, vs[i].x), putf(b, vs[i].y), putf(b, vs[i].z);
        put32(b, 511u << 22);   /* NORMPACKED3 (0, 0, 1) */
        putf(b, vs[i].u), putf(b, vs[i].v), putf(b, 0), putf(b, 0);
    }
    pad(b, 0x80 + d1, 0xEE);
    for (int i = 0; i < nv; i++) {
        const int16_t sl[3] = {12, -3, -3};
        put(b, sl, 6);
        putf(b, 1), putf(b, 0), putf(b, 0);
    }
    pad(b, 0x80 + d2, 0xEE);
    put(b, idx, (size_t)ni * 2);
    put32(b, 0x00800001), put32(b, 0), put32(b, 0);
    put32(b, 0x00800001), put32(b, d1), put32(b, 0);
    put32(b, 0x00010001), put32(b, d2), put32(b, 0);
    pad(b, b->n + 3, 0xEE);
    put(b, "cter", 4);
    for (int k = 0; k < 32; k++)
        for (int e = 0; e < 16; e++) putf(b, e % 5 == 0 ? 1.0f : k == 4 && e == 14 ? -1.8f : 0.0f);
    put32(b, (uint32_t)ni / 3), put32(b, 0), put32(b, 12), put32(b, 24);
}

/* A 4x4 swizzled A8R8G8B8 texture payload of one colour (0xAARRGGBB). */
static void texture(Buf *b, uint32_t argb) {
    b->n = 0;
    put32(b, 0), put32(b, 0), put32(b, 0), put32(b, 0);
    pad(b, 0x80, 0);
    const uint32_t fmt = 0x06u << 8 | 2u << 4 | 1u << 16 | 2u << 20 | 2u << 24;
    put32(b, 0x00040001), put32(b, 0x20), put32(b, 0), put32(b, fmt), put32(b, 0);
    pad(b, 0xA0, 0);
    for (int i = 0; i < 16; i++) put32(b, argb);
}

static void item(Buf *pack, uint32_t item_hash, uint32_t cls, uint32_t hash, const Buf *payload) {
    put32(pack, 1), put32(pack, (uint32_t)payload->n), put32(pack, 0), put32(pack, item_hash);
    put32(pack, cls), put32(pack, (uint32_t)payload->n), put32(pack, 0), put32(pack, hash);
    put(pack, payload->b, payload->n);
}

static void write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(p, 1, n, f);
    fclose(f);
}

/* A static prop payload (FVF 0x152, stride 36, a triangle list): pairs of glasses lying along x,
 * two lens quads facing -y (x < 0 and x > 0) and a temple triangle facing +x at the right end. */
#define PROP_NV 11
#define PROP_NI 15
static const float prop_v[PROP_NV][8] = {
    /* x, y, z, nx, ny, nz, u, v */
    {-0.06f, 0, 0, 0, -1, 0, 0, 0},       {-0.01f, 0, 0, 0, -1, 0, 1, 0},     {-0.01f, 0, 0.04f, 0, -1, 0, 1, 1},
    {-0.06f, 0, 0.04f, 0, -1, 0, 0, 1},   {0.01f, 0, 0, 0, -1, 0, 0, 0},      {0.06f, 0, 0, 0, -1, 0, 1, 0},
    {0.06f, 0, 0.04f, 0, -1, 0, 1, 1},    {0.01f, 0, 0.04f, 0, -1, 0, 0, 1},  {0.06f, 0, 0, 1, 0, 0, 0.5f, 0.5f},
    {0.06f, 0.1f, 0, 1, 0, 0, 0.5f, 0.5f}, {0.06f, 0, 0.01f, 1, 0, 0, 0.5f, 0.5f},
};
static const uint16_t prop_i[PROP_NI] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10};
static void prop(Buf *b) {
    b->n = 0;
    const uint32_t ibd = (PROP_NV * 36 + 15) & ~15u, tail = ibd + PROP_NI * 2;
    put32(b, 2), put32(b, 0x6D), put32(b, 2), put32(b, tail);
    pad(b, 0x50, 0);
    putf(b, 0.1f);
    put32(b, 0x00800001), put32(b, 0), put32(b, 0);
    put32(b, 0x00010001), put32(b, ibd), put32(b, 0);
    put32(b, 0);
    put32(b, 0x152), put32(b, 5u | 36u << 8), put32(b, PROP_NI / 3);
    pad(b, 0x80, 0);
    for (int i = 0; i < PROP_NV; i++) {
        for (int k = 0; k < 6; k++) putf(b, prop_v[i][k]);
        put32(b, 0xff000000u);
        putf(b, prop_v[i][6]), putf(b, prop_v[i][7]);
    }
    pad(b, 0x80 + ibd, 0xEE);
    put(b, prop_i, sizeof prop_i);
}
#define PROP_HASH 0x3394B3BCu   /* the dyn_s_newspaper_c host's prop chunk: its draw hash comes from this disc */

#define TEX_A 0x7A7A0001u
#define TEX_B 0x7B7B0002u

static void make_disc(const char *dir) {
    mkdir(dir, 0755);
    /* char_a: a body triangle (z 0.9-1.2), a neck triangle on the body's top edge (same positions,
     * its own UVs: a UV seam) and a head triangle (z 1.8) floating 0.1 m in front of char_b's head
     * (z 1.8, y 0). char_b: the same body and its head, no neck. UVs in the game's -1..0 range. */
    const V a[9] = {{0, 0, 0.9f, -0.9f, -0.9f}, {0.2f, 0, 1.2f, -0.6f, -0.9f}, {0, 0, 1.2f, -0.9f, -0.6f},
                    {0, 0, 1.2f, -0.9f, -0.4f}, {0.2f, 0, 1.2f, -0.6f, -0.4f}, {0.1f, 0, 1.4f, -0.9f, -0.1f},
                    {0, 0.1f, 1.8f, -0.4f, -0.4f}, {0.2f, 0.1f, 1.8f, -0.1f, -0.4f}, {0, 0.1f, 2.0f, -0.4f, -0.1f}};
    const V bb[6] = {{0, 0, 0.9f, -0.9f, -0.9f}, {0.2f, 0, 1.2f, -0.6f, -0.9f}, {0, 0, 1.2f, -0.9f, -0.6f},
                     {0, 0, 1.8f, -0.4f, -0.4f}, {0.2f, 0, 1.8f, -0.1f, -0.4f}, {0, 0, 2.0f, -0.4f, -0.1f}};
    const uint16_t idx[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    Buf pack = {0}, p = {0}, glr = {0}, idxf = {0};
    put32(&pack, 5), put32(&pack, 0), put32(&pack, 0), put32(&pack, DISC_PACK_MAGIC);
    character(&p, a, 9, idx, 9);
    item(&pack, 0xA000, DISC_CLASS_MESH, disc_model_hash("char_a"), &p);
    character(&p, bb, 6, idx, 6);
    item(&pack, 0xB000, DISC_CLASS_MESH, disc_model_hash("char_b"), &p);
    texture(&p, 0xFFFF0000u);   /* char_a: red */
    item(&pack, 0xA001, DISC_CLASS_TEXTURE, TEX_A, &p);
    texture(&p, 0xFF0000FFu);   /* char_b: blue */
    item(&pack, 0xB001, DISC_CLASS_TEXTURE, TEX_B, &p);
    prop(&p);
    item(&pack, PROP_HASH, DISC_CLASS_MESH, PROP_HASH, &p);
    const size_t pack_len = pack.n;
    pad(&pack, (pack.n + 1023) & ~(size_t)1023, 0);
    const uint32_t glr_at = (uint32_t)(pack.n >> 10);
    /* the skin table (a bare item): char_a -> TEX_A, char_b -> TEX_B */
    put32(&glr, 1), put32(&glr, 16 + 64), put32(&glr, 0), put32(&glr, 0x6C72);
    put32(&glr, DISC_CLASS_SKINS), put32(&glr, 16 + 64), put32(&glr, 0), put32(&glr, 0);
    put32(&glr, 2), put32(&glr, 0), put32(&glr, 0), put32(&glr, 0);
    put32(&glr, disc_model_hash("char_a")), put32(&glr, 1), put32(&glr, 0xA000), put32(&glr, TEX_A), put32(&glr, 0), put32(&glr, 9), put32(&glr, 9), put32(&glr, 0);
    put32(&glr, disc_model_hash("char_b")), put32(&glr, 2), put32(&glr, 0xB000), put32(&glr, TEX_B), put32(&glr, 0), put32(&glr, 9), put32(&glr, 9), put32(&glr, 0);
    put(&pack, glr.b, glr.n);
    char path[700];
    snprintf(path, sizeof path, "%s/A.wad", dir);
    write_file(path, pack.b, pack.n);
    put32(&idxf, 1), put32(&idxf, 2), put32(&idxf, 0), put32(&idxf, 20), put32(&idxf, 56);
    char name[32] = "A.wad";
    put(&idxf, name, 32), put32(&idxf, 0);
    put32(&idxf, 0), put32(&idxf, (uint32_t)pack_len), put32(&idxf, 0), put32(&idxf, disc_name_hash("chars.pak"));
    put32(&idxf, glr_at), put32(&idxf, (uint32_t)glr.n), put32(&idxf, 0), put32(&idxf, disc_name_hash("warriors.glr"));
    snprintf(path, sizeof path, "%s/XBoxWad.idx", dir);
    write_file(path, idxf.b, idxf.n);
    free(pack.b), free(p.b), free(glr.b), free(idxf.b);
}

static int file_exists(const char *path) {
    struct stat st;
    return !stat(path, &st);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len + 1);
    *n = fread(b, 1, (size_t)len, f);
    fclose(f);
    return b;
}

static const char *const recipe =
    "format = 1\n"
    "[source a]\nchar = char_a\n"
    "[source b]\nchar = char_b\n"
    "[texture skin2]\nbase = a.skin0\nrequire = same_size b.skin0\npaste = b.skin0, uv_of b.islands(above 1.6), dilate 0, keep a.islands(below 1.6)\n"
    "[mesh body]\nfrom = a\nhead_swap = b, above 1.6, closed_seam required\ntarget = a skin 0\ntexture = skin2\ngroup = off\n"
    "[mesh hat]\nfrom = a\ntake = headgear(b, above 1.6, tolerance 0.005)\nrigid = joint bip_sw_head\nhost = dyn_dest_ltb\ncolour = ff000000\ntexture = a.skin0\ngroup = off\n"
    "[hide worn]\nhost = dyn_dest_ltb\n";

static int run_build(const char *disc, const char *text, const char *out, char *err, size_t cap) {
    MaRecipe r;
    if (ma_recipe_parse(text, strlen(text), "assets.ini", &r, err, cap)) return -1;
    int files = 0;
    uint64_t bytes = 0;
    int n = ma_build(&r, disc, out, &files, &bytes, err, cap);
    ma_recipe_free(&r);
    return n;
}

static void test_build(const char *tmp, const char *disc) {
    char out[700], path[900], err[512];
    snprintf(out, sizeof out, "%s/ma_build", tmp);
    mkdir(out, 0755);
    int n = run_build(disc, recipe, out, err, sizeof err);
    CHECK(n == 3, "3 output sections built");
    if (n != 3) printf("  %s\n", err);
    /* the head swap: char_a's body + char_b's head, named <char_a's draw hash>-<its skin's hash> */
    Disc d;
    disc_open(&d, disc);
    disc_scan(&d);
    uint8_t *pa = NULL;
    size_t pn = 0;
    for (uint32_t i = 0; i < d.nchunks; i++)
        if (d.chunks[i].hash == disc_model_hash("char_a")) pa = disc_chunk_read(&d, &d.chunks[i]), pn = d.chunks[i].size;
    DiscChar ca;
    CHECK(pa && !disc_char_parse(pa, pn, &ca, err, sizeof err), "synthetic character parses");
    const uint16_t ix[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t mh = meshpack_hash(ix, 9, 0, 8, ca.s0, 32, ca.s1, 18);
    XTexInfo t;
    xtex_info(0x06u << 8 | 2u << 4 | 1u << 16 | 2u << 20 | 2u << 24, 0, &t);
    uint8_t texels[64];
    for (int i = 0; i < 16; i++) memcpy(texels + 4 * i, "\x00\x00\xff\xff", 4);
    uint64_t th = texpack_hash(&t, texels);
    snprintf(path, sizeof path, "%s/off/%016llx-%016llx.wmesh", out, (unsigned long long)mh, (unsigned long long)th);
    size_t len = 0;
    uint8_t *w = slurp(path, &len);
    MeshPackMesh m;
    CHECK(w && !meshpack_parse(w, len, mh, &m), "head swap .wmesh in the group folder, character layout");
    if (w && !meshpack_parse(w, len, mh, &m)) {
        CHECK(m.nv == 9 && m.ni == 9, "9 vertices: a's body and neck, b's head");
        float y;
        memcpy(&y, m.s0 + 6 * 32 + 4, 4);
        CHECK(y == 0.0f && !memcmp(m.s0, ca.s0, 6 * 32) && !memcmp(m.s1, ca.s1, 6 * 18), "body vertices copied from a, head from b (y 0)");
        meshpack_free(&m);
    }
    free(w);
    snprintf(path, sizeof path, "%s/off/%016llx-%016llx.png", out, (unsigned long long)mh, (unsigned long long)th);
    uint32_t tw, tht, *px = texpack_png_read(path, &tw, &tht);
    int blue = 0, red = 0;
    for (uint32_t i = 0; px && i < tw * tht; i++) blue += px[i] == 0xFF0000FFu, red += px[i] == 0xFFFF0000u;
    CHECK(px && tw == 4 && tht == 4 && blue > 0 && red > 0 && blue + red == 16, "the pasted skin: b's head texels (blue) on a's (red)");
    free(px);
    /* the hat: a's head island (0.1 m from b's head) in the head joint's frame (z - 1.8), host layout */
    snprintf(path, sizeof path, "%s/off/6830d7eb9625cfad.wmesh", out);
    w = slurp(path, &len);
    CHECK(w && !meshpack_parse_any(w, len, 0x6830D7EB9625CFADull, &m), "hat .wmesh in the host's layout");
    if (w && !meshpack_parse_any(w, len, 0x6830D7EB9625CFADull, &m)) {
        float z, y;
        memcpy(&z, m.s0 + 8, 4);
        memcpy(&y, m.s0 + 4, 4);
        CHECK(m.nv == 3 && m.stride0 == 36 && m.stride1 == 0 && fabsf(z) < 1e-6f && fabsf(y - 0.1f) < 1e-6f, "3 vertices, rigid in the head frame");
        CHECK(!memcmp(m.s0 + 24, "\x00\x00\x00\xff", 4), "colour ff000000 at the host's diffuse offset");
        meshpack_free(&m);
    }
    free(w);
    snprintf(path, sizeof path, "%s/6830d7eb9625cfad.hide", out);
    CHECK(file_exists(path), "the hide file, top level");
    free(pa);
    disc_close(&d);

    /* errors name the line */
    n = run_build(disc, "format = 1\n[source x]\nchar = nobody\n[mesh m]\nfrom = x\nhead_swap = x, above 1\ntarget = x skin 0\ntexture = x.skin0\n", out, err,
                  sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:3: character nobody not found on the disc"), "a missing character");
    n = run_build(disc, "format = 1\n[source a]\nchar = char_a\n[source b]\nchar = char_b\n[mesh m]\nfrom = a\nhead_swap = b, above 1.0, closed_seam required\ntarget = a skin 0\ntexture = a.skin0\n",
                  out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:8: head_swap: char_b's head does not close char_a's neck seam (1 open edges)"), "an open seam is refused");
    n = run_build(disc, "format = 1\n[source a]\nchar = char_a\n[mesh m]\nfrom = a\nhead_swap = a, above 1.6\ntarget = a skin 3\ntexture = a.skin0\n", out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:7: char_a has no skin 3"), "a missing skin");
    n = run_build(disc, "format = 1\n[hide h]\nhost = dyn_nothing\n", out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:3: unknown host object dyn_nothing"), "an unknown host");
    n = run_build(disc, "format = 1\n[texture t]\nbase = a.skin0\n", out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:3: no [source a] before this line"), "a source must come first");
    n = run_build(disc, "format = 1\n[texture t]\ncanvas = 4 4, colour 1 2 3\nsharpen = 2\n", out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:4: unknown key sharpen in [texture t]"), "an unknown key");
    n = run_build("/nonexistent-disc", recipe, out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "no game files"), "no disc");
}

/* The necklace's steps on a synthetic texture: a silver blob inside, a silver speck on the border. */
static void test_trophy(const char *tmp, const char *disc) {
    char out[700], path[900], err[512];
    snprintf(out, sizeof out, "%s/ma_trophy", tmp);
    mkdir(out, 0755);
    const char *t = "format = 1\n"
                    "[texture board]\ncanvas = 8 8, colour 200 60 40\nplace = silver, at 2 2\nmask = colour(r-b <= 10, sum >= 135), parts >= 4, inside, at_least 4\n"
                    "polish = keep 0.6, grey 0.4, gain 1.55, add 12\nfill = velvet 124 26 40, noise 0.08, seed 7, shadow\nscale = 2 bilinear\n"
                    "[texture silver]\ncanvas = 2 2, colour 150 150 150\n"
                    "[texture trophy]\ncanvas = 32 32, colour 124 26 40\nplace = board, at 0 0\nplace = wood 132 88 52 seed 11, at 0 24 32 8, as wood\n"
                    "[mesh frame]\nshape = shadow_box, board 8 8 texels 0.01, bar 0.03, depth 0.04, board_depth 0.012\nuv = board trophy.board, wood trophy.wood\n"
                    "host = dyn_s_paper_list\ncolour = ff000000\ntexture = trophy\n";
    int n = run_build(disc, t, out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:4: no texture silver"), "a texture must be defined before it is placed");
    /* silver defined first */
    const char *t2 = "format = 1\n"
                     "[texture silver]\ncanvas = 2 2, colour 150 150 150\n"
                     "[texture board]\ncanvas = 8 8, colour 200 60 40\nplace = silver, at 2 2\nmask = colour(r-b <= 10, sum >= 135), parts >= 4, inside, at_least 4\n"
                     "polish = keep 0.6, grey 0.4, gain 1.55, add 12\nfill = velvet 124 26 40, noise 0.08, seed 7, shadow\nscale = 2 bilinear\n"
                     "[texture trophy]\ncanvas = 32 32, colour 124 26 40\nplace = board, at 0 0\nplace = wood 132 88 52 seed 11, at 0 24 32 8, as wood\n"
                     "[mesh frame]\nshape = shadow_box, board 8 8 texels 0.01, bar 0.03, depth 0.04, board_depth 0.012\nuv = board trophy.board, wood trophy.wood\n"
                     "host = dyn_s_paper_list\ncolour = ff000000\ntexture = trophy\n";
    n = run_build(disc, t2, out, err, sizeof err);
    CHECK(n == 1, "the trophy builds");
    if (n != 1) printf("  %s\n", err);
    snprintf(path, sizeof path, "%s/7b6f89aa421da125.wmesh", out);
    size_t len = 0;
    uint8_t *w = slurp(path, &len);
    MeshPackMesh m;
    CHECK(w && !meshpack_parse_any(w, len, 0x7B6F89AA421DA125ull, &m) && m.nv == 120 && m.ni == 180, "5 boxes: 120 vertices, 60 triangles");
    if (w && !meshpack_parse_any(w, len, 0x7B6F89AA421DA125ull, &m)) {
        float top = -1;
        for (uint32_t v = 0; v < m.nv; v++) {
            float z;
            memcpy(&z, m.s0 + v * 36 + 8, 4);
            if (z > top) top = z;
        }
        CHECK(fabsf(top - (0.08f + 0.06f)) < 1e-6f, "frame height = board + 2 bars");
        meshpack_free(&m);
    }
    free(w);
    snprintf(path, sizeof path, "%s/7b6f89aa421da125.png", out);
    uint32_t tw, th, *px = texpack_png_read(path, &tw, &th);
    CHECK(px && tw == 32 && th == 32, "the atlas texture");
    if (px) {
        /* the polished silver (150 -> min(255, int((0.6 * 150 + 0.4 * 150) * 1.55 + 12)) = 244), scaled 2x at (4..7, 4..7) */
        CHECK(px[5 * 32 + 5] == 0xFFF4F4F4u, "polished silver in the board");
        CHECK((px[0] & 0xFF) < 60 && ((px[0] >> 16) & 0xFF) > 100, "velvet around it");
        CHECK(px[28 * 32 + 3] != 0xFF7C1A28u, "wood strip drawn");
        free(px);
    }
    const char *t3 = "format = 1\n[texture s]\ncanvas = 8 8, colour 150 150 150\nmask = colour(r-b <= 10, sum >= 135), inside, at_least 1\n";
    n = run_build(disc, t3, out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:4: mask: only 0 texels found, at least 1 needed"), "a mask touching the border is dropped");
}

/* dirty-fights' steps: a piece apart from the body as a hat and the face without it (an instance
 * group), a prop source painted, broken and cracked on a host whose draw hash comes from the disc. */
static void test_fights(const char *tmp, const char *disc) {
    char out[700], path[900], err[512];
    snprintf(out, sizeof out, "%s/ma_fights", tmp);
    mkdir(out, 0755);
    const char *text =
        "format = 1\n"
        "[source a]\nchar = char_a\n"
        "[source g]\nprop = 0x3394b3bc\n"
        "[mesh glasses]\nfrom = a\ntake = largest_apart(min_tris 1, min_width 0.1, top 0.4)\nrigid = joint bip_sw_head\nhost = dyn_civl_hl_ho\n"
        "colour = ff000000\ntexture = a.skin0\ngroup = glasses-a\nworn = yes\n"
        "[mesh face]\nfrom = a\ndrop = largest_apart(min_tris 1, min_width 0.1, top 0.4)\ntarget = a\ngroup = glasses-a\n"
        "[mesh broken]\nfrom = g\ntake = all\npaint = frame 200 150 50, lens 20 30 40, size 8\nbreak = turn 28, gap 0.035, lens 35, shards 9, seed 3\n"
        "host = dyn_s_newspaper_c\ncolour = ff000000\ncrack = lines 7, seed 3\ngroup = broken-g\nplaced = yes\n";
    int n = run_build(disc, text, out, err, sizeof err);
    CHECK(n == 3, "3 output sections built");
    if (n != 3) printf("  %s\n", err);
    /* char_a's welded pieces: body + neck (2 triangles, same positions on the seam), the head triangle
     * (0.2 m wide, in the top 0.4 m): that one is the piece apart */
    Disc d;
    disc_open(&d, disc);
    disc_scan(&d);
    uint8_t *pa = NULL;
    size_t pn = 0;
    for (uint32_t i = 0; i < d.nchunks; i++)
        if (d.chunks[i].hash == disc_model_hash("char_a")) pa = disc_chunk_read(&d, &d.chunks[i]), pn = d.chunks[i].size;
    DiscChar ca;
    CHECK(pa && !disc_char_parse(pa, pn, &ca, err, sizeof err), "synthetic character parses");
    const uint16_t ix[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    const uint64_t mh = meshpack_hash(ix, 9, 0, 8, ca.s0, 32, ca.s1, 18);
    size_t len = 0;
    MeshPackMesh m;
    snprintf(path, sizeof path, "%s/glasses-a/%016llx.wmesh", out, (unsigned long long)mh);
    uint8_t *w = slurp(path, &len);
    CHECK(w && !meshpack_parse(w, len, mh, &m), "the face: <draw hash>.wmesh (every skin), character layout");
    if (w && !meshpack_parse(w, len, mh, &m)) {
        CHECK(m.nv == 9 && m.ni == 6 && !memcmp(m.idx, ix, 12) && !memcmp(m.s0, ca.s0, 9 * 32), "all vertices kept, the head triangle left out");
        meshpack_free(&m);
    }
    free(w);
    snprintf(path, sizeof path, "%s/glasses-a/%016llx.png", out, (unsigned long long)mh);
    CHECK(!file_exists(path), "no texture for a draw of every skin");
    snprintf(path, sizeof path, "%s/glasses-a/5399c5a77dd5dd91.wmesh", out);
    w = slurp(path, &len);
    CHECK(w && !meshpack_parse_any(w, len, 0x5399C5A77DD5DD91ull, &m), "the glasses on the host absent from this disc: the --mesh-log hash");
    if (w && !meshpack_parse_any(w, len, 0x5399C5A77DD5DD91ull, &m)) {
        CHECK(m.nv == 3 && m.ni == 3 && m.stride0 == 36, "the head triangle as an object");
        meshpack_free(&m);
    }
    free(w);
    snprintf(path, sizeof path, "%s/glasses-a/5399c5a77dd5dd91.worn", out);
    CHECK(file_exists(path), "worn = yes: the group's instance marker");
    snprintf(path, sizeof path, "%s/glasses-a/5399c5a77dd5dd91.png", out);
    CHECK(file_exists(path), "the glasses' texture");
    free(pa);
    /* the broken prop: drawn as the host this disc has (hash of its prop chunk's bytes) */
    uint8_t *pp = NULL;
    for (uint32_t i = 0; i < d.nchunks; i++)
        if (d.chunks[i].hash == PROP_HASH) pp = disc_chunk_read(&d, &d.chunks[i]), pn = d.chunks[i].size;
    DiscProp pr;
    CHECK(pp && !disc_prop_parse(pp, pn, &pr, err, sizeof err) && pr.nv == PROP_NV && pr.nidx == PROP_NI && pr.stride == 36 && pr.normal == 12 &&
              pr.colour == 24 && pr.uv0 == 28 && pr.prim == 5,
          "synthetic prop parses: FVF 0x152 layout");
    const uint64_t ph = pp ? meshpack_hash(prop_i, PROP_NI, 0, PROP_NV - 1, pr.vb, 36, NULL, 0) : 0;
    free(pp);
    disc_close(&d);
    snprintf(path, sizeof path, "%s/broken-g/%016llx.wmesh", out, (unsigned long long)ph);
    w = slurp(path, &len);
    CHECK(ph != 0xC52A15938C5F92B8ull && w && !meshpack_parse_any(w, len, ph, &m), "the broken pair on the host's draw hash from this disc");
    if (w && !meshpack_parse_any(w, len, ph, &m)) {
        /* kept: the left lens (2) and the turned temple (1); the right lens gone; 9 shards */
        CHECK(m.ni == 3 * (3 + 9), "left lens + temple + 9 shards");
        int below = 0, shards_flat = 1;
        for (uint32_t v = 0; v < m.nv; v++) {
            float z;
            memcpy(&z, m.s0 + v * 36 + 8, 4);
            below += z < 0.0f;
        }
        for (uint32_t v = m.nv - 27; v < m.nv; v++) {
            float z, nz;
            memcpy(&z, m.s0 + v * 36 + 8, 4);
            memcpy(&nz, m.s0 + v * 36 + 20, 4);
            shards_flat &= fabsf(z - 0.002f) < 1e-6f && nz == 1.0f;
        }
        CHECK(!below && shards_flat, "lying on the ground; the shards flat at 2 mm");
        meshpack_free(&m);
    }
    free(w);
    snprintf(path, sizeof path, "%s/broken-g/%016llx.png", out, (unsigned long long)ph);
    uint32_t tw, th, *px = texpack_png_read(path, &tw, &th);
    int lensc = 0, frame = 0, light = 0;
    for (uint32_t i = 0; px && i < tw * th; i++) {
        lensc += px[i] == 0xFF141E28u, frame += px[i] == 0xFFC89632u;
        light += (px[i] & 0xff) > 0x28 && (i % tw) < tw / 2;
    }
    CHECK(px && tw == 8 && th == 8 && frame == 32 && lensc + light == 32 && light > 0, "paint: lens colour left, frame right; cracks over the lens");
    free(px);
    snprintf(path, sizeof path, "%s/broken-g/placed", out);
    CHECK(file_exists(path), "placed = yes: the group's marker");

    /* errors */
    n = run_build(disc, "format = 1\n[source g]\nprop = 0x3394b3bc\n[mesh m]\nfrom = g\ntake = all\nrigid = joint bip_sw_head\nhost = dyn_pl_dr\ntexture = g.skin0\n",
                  out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:7: a prop's vertices are in its own frame: no rigid ="), "a prop takes no rigid");
    n = run_build(disc, "format = 1\n[source g]\nprop = 0x3394b3bc\n[mesh m]\nfrom = g\ntake = largest_apart(min_tris 1, min_width 0, top 1)\nhost = dyn_pl_dr\ntexture = g.skin0\n",
                  out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:6: a prop is taken whole: take = all"), "a prop is taken whole");
    n = run_build(disc, "format = 1\n[source a]\nchar = char_a\n[mesh m]\nfrom = a\ntake = largest_apart(min_tris 5, min_width 0.1, top 0.4)\nrigid = joint bip_sw_head\nhost = dyn_pl_dr\ntexture = a.skin0\n",
                  out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:6: char_a: no piece apart from the body with at least 5 triangles"), "no piece apart");
    n = run_build(disc, "format = 1\n[source a]\nchar = char_a\n[mesh m]\nfrom = a\ndrop = largest_apart(min_tris 1, min_width 0.1, top 0.4)\ntarget = a\ntexture = a.skin0\n",
                  out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:8: target = a replaces the draw of every skin: no texture ="), "a draw of every skin takes no texture");
    n = run_build(disc, "format = 1\n[source g]\nprop = 0x3394b3bc\n[mesh m]\nfrom = g\ntake = all\nhost = dyn_pl_dr\npaint = frame 1 2 3, lens 1 2 3, size 8\ncrack = lines 7, seed 1\n",
                  out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:9: crack = needs break ="), "crack needs break");
    n = run_build(disc, "format = 1\n[source g]\nprop = 0x3394b3bc\n[mesh m]\nfrom = g\ntake = all\nhost = dyn_pl_dr\npaint = frame 1 2 3, lens 1 2 3, size 8\nworn = yes\n",
                  out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:9: worn = yes (a host mesh in a group"), "worn needs a group");
    n = run_build(disc, "format = 1\n[source g]\nprop = dyn_sun\n", out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:3: prop = 0xHASH"), "a prop by its chunk hash");
    n = run_build(disc, "format = 1\n[source g]\nprop = 0x12345678\n[mesh m]\nfrom = g\ntake = all\nhost = dyn_pl_dr\npaint = frame 1 2 3, lens 1 2 3, size 8\n",
                  out, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:3: prop 0x12345678 not found on the disc"), "a missing prop");
}

static int count_entries(const char *dir) {
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) n += e->d_name[0] != '.';
    if (d) closedir(d);
    return n;
}

static void test_cache(const char *tmp, const char *disc) {
    char mod[700], hdd[700], path[1400], err[512];
    snprintf(mod, sizeof mod, "%s/ma_mod", tmp);
    snprintf(hdd, sizeof hdd, "%s/ma_hdd", tmp);
    mkdir(mod, 0755);
    mkdir(hdd, 0755);
    MaResult r;
    snprintf(path, sizeof path, "%s/assets.ini", mod);
    remove(path);
    CHECK(modassets_prepare(mod, "m", "1.0", disc, hdd, 0, &r, err, sizeof err) == 0, "no assets.ini: nothing to do");
    CHECK(modassets_current(mod, "m", "1.0", disc, hdd) == -1, "current: no assets.ini");
    write_file(path, recipe, strlen(recipe));
    CHECK(modassets_current(mod, "m", "1.0", disc, hdd) == 0, "current: not built yet");
    CHECK(modassets_prepare(mod, "m", "1.0", disc, hdd, 0, &r, err, sizeof err) == 1 && r.built && r.outputs == 3 && r.files == 5, "miss: built");
    CHECK(modassets_current(mod, "m", "1.0", disc, hdd) == 1, "current: built");
    CHECK(modassets_current(mod, "m", "1.1", disc, hdd) == 0 && modassets_current(mod, "m", "1.0", "/nonexistent-disc", hdd) == 0,
          "current: another version or no disc needs a build");
    /* (the disc's chunk index file is written by the first scan of a process: disc_test covers it;
     * this process reuses the scan of the earlier tests) */
    snprintf(path, sizeof path, "%s/assets.ini", mod);
    char first[1200];
    snprintf(first, sizeof first, "%s", r.cache);
    snprintf(path, sizeof path, "%s/done", r.cache);
    CHECK(file_exists(path), "done written");
    snprintf(path, sizeof path, "%s/build.json", r.cache);
    CHECK(file_exists(path), "build.json written");
    snprintf(path, sizeof path, "%s/%016llx", hdd, 0ull);
    CHECK(strstr(r.cache, "/mods-cache/m/") != NULL, "under <hdd>/mods-cache/<id>/");
    CHECK(modassets_prepare(mod, "m", "1.0", disc, hdd, 0, &r, err, sizeof err) == 1 && !r.built && r.outputs == 3 && !strcmp(r.cache, first), "hit: not rebuilt");
    snprintf(path, sizeof path, "%s/off/6830d7eb9625cfad.png", first);
    remove(path);
    CHECK(modassets_prepare(mod, "m", "1.0", disc, hdd, 1, &r, err, sizeof err) == 1 && r.built && file_exists(path), "--rebuild-mod-assets builds again");
    CHECK(modassets_prepare(mod, "m", "1.1", disc, hdd, 0, &r, err, sizeof err) == 1 && r.built && strcmp(r.cache, first), "a new mod version: a new key");
    CHECK(!file_exists(first), "the stale key is deleted");
    snprintf(path, sizeof path, "%s/mods-cache/m", hdd);
    CHECK(count_entries(path) == 1, "one key left");
    /* a failing build leaves no folder and no .partial; the good key stays */
    snprintf(path, sizeof path, "%s/assets.ini", mod);
    const char *bad = "format = 1\n[hide worn]\nhost = dyn_dest_ltb\n[source x]\nchar = nobody\n[mesh m]\nfrom = x\nhead_swap = x, above 1\ntarget = x skin 0\ntexture = x.skin0\n";
    write_file(path, bad, strlen(bad));
    CHECK(modassets_prepare(mod, "m", "1.1", disc, hdd, 0, &r, err, sizeof err) == -1 && strstr(err, "assets.ini:5: character nobody"), "failed build: the reason");
    CHECK(!r.cache[0], "failed build: no cache folder");
    snprintf(path, sizeof path, "%s/mods-cache/m", hdd);
    CHECK(count_entries(path) == 1, "failed build: no partial folder, the earlier key kept");
    /* budgets */
    snprintf(path, sizeof path, "%s/assets.ini", mod);
    write_file(path, recipe, strlen(recipe));
    modassets_files_max = 2;
    CHECK(modassets_prepare(mod, "m2", "1", disc, hdd, 0, &r, err, sizeof err) == -1 && strstr(err, "more than 2 output files"), "file budget");
    modassets_files_max = MODASSETS_FILES_MAX;
    modassets_bytes_max = 64;
    CHECK(modassets_prepare(mod, "m2", "1", disc, hdd, 0, &r, err, sizeof err) == -1 && strstr(err, "bytes of output"), "size budget");
    modassets_bytes_max = MODASSETS_BYTES_MAX;
    modassets_time_ms = 0;
    CHECK(modassets_prepare(mod, "m2", "1", disc, hdd, 0, &r, err, sizeof err) == -1 && strstr(err, "budget"), "time budget");
    modassets_time_ms = MODASSETS_TIME_MS;
    snprintf(path, sizeof path, "%s/mods-cache/m2", hdd);
    CHECK(count_entries(path) == 0, "budget aborts leave nothing");
    CHECK(modassets_prepare(mod, "m2", "1", "/nonexistent-disc", hdd, 0, &r, err, sizeof err) == -1 && strstr(err, "XBoxWad.idx"), "no disc");
    snprintf(path, sizeof path, "%s/assets.ini", mod);
    write_file(path, "format = 3\n", 11);
    CHECK(modassets_prepare(mod, "m2", "1", disc, hdd, 0, &r, err, sizeof err) == -1 && strstr(err, "assets.ini:1:"), "parse errors pass through");
}

/* ---- [layer]: surface effects over a character's clothes ---- */

/* A 32x32 swizzled A8R8G8B8 texture: the colour of each 16x16 quarter (top left, top right,
 * bottom left, bottom right). */
static void texture4(Buf *b, const uint32_t q[4]) {
    b->n = 0;
    put32(b, 0), put32(b, 0), put32(b, 0), put32(b, 0);
    pad(b, 0x80, 0);
    const uint32_t fmt = 0x06u << 8 | 2u << 4 | 1u << 16 | 5u << 20 | 5u << 24;
    put32(b, 0x00040001), put32(b, 0x20), put32(b, 0), put32(b, fmt), put32(b, 0);
    pad(b, 0xA0, 0);
    uint32_t px[1024];
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++) px[xbox_swizzle_offset(x, y, 32, 32)] = q[(y >= 16) * 2 + (x >= 16)];
    put(b, px, sizeof px);
}

/* A quad (2 triangles) at x0..x1, y 0, z0..z1 with UVs u0..u1 x v0..v1 (its own island). */
static void quad(V *vs, uint16_t *idx, int *nv, int *ni, float x0, float x1, float z0, float z1, float u0, float u1, float v0, float v1) {
    const int b = *nv;
    vs[b] = (V){x0, 0, z0, u0, v1}, vs[b + 1] = (V){x1, 0, z0, u1, v1}, vs[b + 2] = (V){x1, 0, z1, u1, v0}, vs[b + 3] = (V){x0, 0, z1, u0, v0};
    const uint16_t t[6] = {(uint16_t)b, (uint16_t)(b + 1), (uint16_t)(b + 2), (uint16_t)b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
    memcpy(idx + *ni, t, sizeof t);
    *nv += 4, *ni += 6;
}

#define TEX_X 0x7C000001u   /* warr_x: khaki trousers, a red vest, skin-coloured face and arms */
#define TEX_Y 0x7C000002u   /* warr_y: all khaki (the torso is the face's colour: skin) */
#define TEX_G 0x7C000003u   /* blue: a ghost skin */
#define TEX_K 0x7C000004u   /* black: not this model's layout */
#define KHAKI 0xFF6E6450u
#define RED 0xFFB01818u
#define FACE 0xFFC08C6Eu

/* warr_x and warr_y: trousers (z 0.2-0.8, texels x 0-7 y 0-7), a torso (z 1.25-1.45, y 16-23), a
 * head (z 1.6-1.9, x 16-23 y 0-7) and an arm (|x| 0.5, x 16-23 y 16-23). warr_x's skins: TEX_X
 * (its own), TEX_G and TEX_K (by its item); warr_y's: TEX_Y and TEX_X (by its item: warr_x's). */
static void make_cloth_disc(const char *dir) {
    mkdir(dir, 0755);
    V vs[16];
    uint16_t idx[24];
    int nv = 0, ni = 0;
    quad(vs, idx, &nv, &ni, -0.1f, 0.1f, 0.2f, 0.8f, -1.0f, -0.75f, -1.0f, -0.75f);
    quad(vs, idx, &nv, &ni, -0.1f, 0.1f, 1.25f, 1.45f, -1.0f, -0.75f, -0.5f, -0.25f);
    quad(vs, idx, &nv, &ni, -0.1f, 0.1f, 1.6f, 1.9f, -0.5f, -0.25f, -1.0f, -0.75f);
    quad(vs, idx, &nv, &ni, 0.4f, 0.6f, 1.3f, 1.4f, -0.5f, -0.25f, -0.5f, -0.25f);
    Buf pack = {0}, p = {0}, glr = {0}, idxf = {0};
    put32(&pack, 6), put32(&pack, 0), put32(&pack, 0), put32(&pack, DISC_PACK_MAGIC);
    character(&p, vs, nv, idx, ni);
    item(&pack, 0xA000, DISC_CLASS_MESH, disc_model_hash("warr_x"), &p);
    item(&pack, 0xC000, DISC_CLASS_MESH, disc_model_hash("warr_y"), &p);
    const uint32_t qx[4] = {KHAKI, FACE, RED, FACE}, qy[4] = {KHAKI, KHAKI, KHAKI, KHAKI};
    const uint32_t qg[4] = {0xFF2020C0u, 0xFF2020C0u, 0xFF2020C0u, 0xFF2020C0u}, qk[4] = {0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u};
    texture4(&p, qx), item(&pack, 0xA001, DISC_CLASS_TEXTURE, TEX_X, &p);
    texture4(&p, qy), item(&pack, 0xC001, DISC_CLASS_TEXTURE, TEX_Y, &p);
    texture4(&p, qg), item(&pack, 0xA002, DISC_CLASS_TEXTURE, TEX_G, &p);
    texture4(&p, qk), item(&pack, 0xA003, DISC_CLASS_TEXTURE, TEX_K, &p);
    const size_t pack_len = pack.n;
    pad(&pack, (pack.n + 1023) & ~(size_t)1023, 0);
    const uint32_t glr_at = (uint32_t)(pack.n >> 10);
    const uint32_t rec[5][3] = {{disc_model_hash("warr_x"), 0xA000, TEX_X}, {0x11111111u, 0xA000, TEX_G}, {0x22222222u, 0xA000, TEX_K},
                                {disc_model_hash("warr_y"), 0xC000, TEX_Y}, {0x33333333u, 0xC000, TEX_X}};
    put32(&glr, 1), put32(&glr, 16 + 5 * 32), put32(&glr, 0), put32(&glr, 0x6C72);
    put32(&glr, DISC_CLASS_SKINS), put32(&glr, 16 + 5 * 32), put32(&glr, 0), put32(&glr, 0);
    put32(&glr, 5), put32(&glr, 0), put32(&glr, 0), put32(&glr, 0);
    for (int i = 0; i < 5; i++)
        put32(&glr, rec[i][0]), put32(&glr, (uint32_t)i + 1), put32(&glr, rec[i][1]), put32(&glr, rec[i][2]), put32(&glr, 0), put32(&glr, 9), put32(&glr, 9),
            put32(&glr, 0);
    put(&pack, glr.b, glr.n);
    char path[700];
    snprintf(path, sizeof path, "%s/A.wad", dir);
    write_file(path, pack.b, pack.n);
    put32(&idxf, 1), put32(&idxf, 2), put32(&idxf, 0), put32(&idxf, 20), put32(&idxf, 56);
    char name[32] = "A.wad";
    put(&idxf, name, 32), put32(&idxf, 0);
    put32(&idxf, 0), put32(&idxf, (uint32_t)pack_len), put32(&idxf, 0), put32(&idxf, disc_name_hash("chars.pak"));
    put32(&idxf, glr_at), put32(&idxf, (uint32_t)glr.n), put32(&idxf, 0), put32(&idxf, disc_name_hash("warriors.glr"));
    snprintf(path, sizeof path, "%s/XBoxWad.idx", dir);
    write_file(path, idxf.b, idxf.n);
    free(pack.b), free(p.b), free(glr.b), free(idxf.b);
}

static const char *const cloth_recipe =
    "format = 1\n"
    "[set crew]\nx = warr_x\ny = warr_y\n"
    "[layer {code}-{stage}]\n"
    "for = crew\nskins = claimed _gen\nskip = ghost, painted < 0.85\nseed = 1000 + 37 * {index}\nstages = 1, 2, 3\n"
    "region = cloth(mean_z 0.06..1.66, abs_x <= 0.3, legs below 1.2, skin_island 0.3, head above 1.55)\n"
    "protect = face_colour near 3 share 0.25\nvest = red 0.4, patch 1.45, edge 6, patch_keep 0.12\n"
    "effect = grime tint 0.48 0.43 0.36, scale 7, legs_bias 0.45 0.9, torso_bias 1.3 0.8, spread 0.35 0.65 1.0, dark 0.25 0.42 0.55, "
    "dark_cloth 0.30 0.45 0.55, desat 0 0.10 0.18\n"
    "effect = dust colour 128 118 100, scale 11, edge 0.16 0.22 0.28, legs 0.55, below 0.7\n"
    "effect = scuff 0.30 0.40 0.50\neffect = stains 0 3 6\neffect = tears 0 2 7\neffect = damp stage 3, above 1.38, amount 0.3\n"
    "effect = wear 0 0.03 0.07\ngrow = 2\n";

/* The texels of the file <tex>@<label> (one per label here), or NULL. */
static uint32_t *layer_png(const char *out, const char *label, uint32_t *w, uint32_t *h) {
    DIR *d = opendir(out);
    struct dirent *e;
    uint32_t *found = NULL;
    while (d && !found && (e = readdir(d))) {
        const char *at = strchr(e->d_name, '@');
        if (!at || strcmp(at + 1, label)) continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", out, e->d_name);
        uint32_t *px = texpack_png_read(path, w, h);
        if (px && *w == 32 && *h == 32) found = px;
        else free(px);
    }
    if (d) closedir(d);
    return found;
}

static int count_suffix(const char *dir, const char *suffix) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    const size_t L = strlen(suffix);
    while (d && (e = readdir(d))) {
        const size_t k = strlen(e->d_name);
        n += k > L && !strcmp(e->d_name + k - L, suffix);
    }
    if (d) closedir(d);
    return n;
}

static int opaque_in(const uint32_t *px, int x0, int x1, int y0, int y1) {
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) n += (px[y * 32 + x] >> 24) == 255;
    return n;
}

static double mean_sum(const uint32_t *px, int x0, int x1, int y0, int y1) {
    double s = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) s += (px[y * 32 + x] >> 16 & 255) + (px[y * 32 + x] >> 8 & 255) + (px[y * 32 + x] & 255);
    return s / ((x1 - x0 + 1) * (y1 - y0 + 1));
}

static void test_layers(const char *tmp) {
    char disc[700], out[700], out2[700], err[512];
    snprintf(disc, sizeof disc, "%s/ma_cloth_disc", tmp);
    make_cloth_disc(disc);
    snprintf(out, sizeof out, "%s/ma_layers", tmp);
    snprintf(out2, sizeof out2, "%s/ma_layers2", tmp);
    mkdir(out, 0755), mkdir(out2, 0755);
    int n = run_build(disc, cloth_recipe, out, err, sizeof err);
    CHECK(n == 1, "the [layer] section builds");
    if (n != 1) printf("  %s\n", err);
    CHECK(count_suffix(out, "@x-1.png") == 1 && count_suffix(out, "@x-2.png") == 1 && count_suffix(out, "@x-3.png") == 1,
          "warr_x: one skin, three stages (the ghost and the black skin skipped)");
    CHECK(count_suffix(out, "@y-1.png") == 1 && count_suffix(out, "@y-3.png") == 1 && count_suffix(out, ".png") == 6,
          "warr_y: its own skin only (TEX_X is warr_x's): 6 files");
    uint32_t w = 0, h = 0, *x1 = layer_png(out, "x-1.png", &w, &h), *x3 = layer_png(out, "x-3.png", &w, &h);
    uint32_t *y3 = layer_png(out, "y-3.png", &w, &h);
    CHECK(x1 && x3 && y3, "the layers are 32x32 PNGs");
    if (x1 && x3 && y3) {
        CHECK(opaque_in(x3, 0, 7, 0, 7) == 64 && opaque_in(x3, 0, 7, 16, 23) == 64, "trousers and vest texels are painted");
        CHECK(opaque_in(x3, 8, 9, 0, 7) == 16 && opaque_in(x3, 10, 15, 0, 7) == 0, "grown by 2 texels, clear beyond");
        CHECK(opaque_in(x3, 16, 31, 0, 31) == 0, "the face and the arm stay clear");
        CHECK(opaque_in(y3, 0, 7, 0, 7) == 64 && opaque_in(y3, 0, 9, 10, 31) == 0, "a torso of the face's colour (skin) stays clear");
        CHECK(mean_sum(x3, 0, 7, 0, 7) < mean_sum(x1, 0, 7, 0, 7) && mean_sum(x1, 0, 7, 0, 7) <= 110 + 100 + 80 + 1e-9,
              "stage 3 darker than stage 1, stage 1 no lighter than the cloth");
        int red = 0;
        for (int y = 16; y < 24; y++)
            for (int x = 0; x < 8; x++) {
                const uint32_t c = x3[y * 32 + x];
                const int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
                red += r >= 3 * g && r >= 3 * b;
            }
        CHECK(red >= 48, "the vest stays red at stage 3 (hue protection; tears show skin)");
        if (red < 48) printf("  %d of 64 red\n", red);
    }
    free(x1), free(x3), free(y3);
    n = run_build(disc, cloth_recipe, out2, err, sizeof err);
    int same = n == 1;
    DIR *d = opendir(out);
    struct dirent *e;
    while (same && d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char a[1200], b[1200];
        size_t na = 0, nb = 0;
        snprintf(a, sizeof a, "%s/%s", out, e->d_name), snprintf(b, sizeof b, "%s/%s", out2, e->d_name);
        uint8_t *pa = slurp(a, &na), *pb = slurp(b, &nb);
        same = pa && pb && na == nb && !memcmp(pa, pb, na);
        free(pa), free(pb);
    }
    if (d) closedir(d);
    CHECK(same, "a second build gives the same files (seeded)");
    /* errors */
    char text[4096];
    snprintf(text, sizeof text, "format = 1\n[layer {code}-{stage}]\nfor = nobody\nstages = 1\neffect = wear 0.1 0.1 0.1\n");
    n = run_build(disc, text, out2, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:3: no [set nobody] before this line"), "for = needs a set");
    snprintf(text, sizeof text, "format = 1\n[set s]\nx = warr_x\n[layer {code}-{stage}]\nfor = s\nstages = 1, 4\neffect = wear 0.1 0.1 0.1\n");
    n = run_build(disc, text, out2, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:6: stages are 1, 2 or 3"), "stage 4 refused");
    snprintf(text, sizeof text, "format = 1\n[set s]\nx = warr_x\n[layer {code}]\nfor = s\nstages = 1, 2\neffect = wear 0.1 0.1 0.1\n");
    n = run_build(disc, text, out2, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:4: the layer name needs {stage} (more than one stage)"), "a name per stage");
    snprintf(text, sizeof text, "format = 1\n[set s]\nx = warr_x\n[layer {code}-{stage}]\nfor = s\nstages = 1\neffect = shine 1\n");
    n = run_build(disc, text, out2, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:7: unknown effect shine"), "unknown effect");
    snprintf(text, sizeof text, "format = 1\n[set s]\nx = warr_x\n[layer {code}-{stage}]\nfor = s\nstages = 1\n");
    n = run_build(disc, text, out2, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:4: [layer {code}-{stage}] needs at least one effect ="), "no effect");
    snprintf(text, sizeof text, "format = 1\n[set s]\nx = warr_nobody\n[layer {code}-{stage}]\nfor = s\nstages = 1\neffect = wear 0.1 0.1 0.1\n");
    n = run_build(disc, text, out2, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:3: character warr_nobody not found on the disc"), "a missing model");
    snprintf(text, sizeof text, "format = 1\n[set s]\nx = warr_x\n[layer {code}-{stage}]\nfor = s\nstages = 1\nskip = painted < 1.5\neffect = wear 0.1 0.1 0.1\n");
    n = run_build(disc, text, out2, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:4: no skin of set s got a layer"), "nothing built is an error");
    snprintf(text, sizeof text, "format = 1\n[set s]\nx = warr_x\n[layer {code}-{stage}]\nfor = s\nstages = 1\neffect = grime scale 7, colour 1\n");
    n = run_build(disc, text, out2, err, sizeof err);
    CHECK(n < 0 && strstr(err, "assets.ini:7: unknown grime value colour"), "unknown effect value");
}


/* ---- own assets: .glb meshes, PNG / DDS textures, WAV / OGG and procedural sounds ---- */

/* A tiny glTF 2.0 binary written here. Each mesh is one triangle-list primitive with POSITION,
 * NORMAL (+y in glTF = +z in the game), TEXCOORD_0 (unless no_uv) and, when skinned, JOINTS_0 /
 * WEIGHTS_0 over a skin whose joints are nodes named jn[]. A node may carry a translation. */
typedef struct {
    const char *name;
    int nv, ni;
    const float (*pos)[3];
    const uint16_t *idx;
    const uint8_t (*joints)[4];   /* NULL: static */
    const float (*weights)[4];
    float tx;                     /* node translation x (glTF) */
} GlbMesh;

static void jsonf(Buf *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void jsonf(Buf *j, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    put(j, tmp, (size_t)n);
}

/* jn: joint names (NULL: no skin); uri: an external buffer instead of the BIN chunk; no_uv. */
static void make_glb(Buf *out, const GlbMesh *ms, int nm, const char *const *jn, int njn, const char *uri, int no_uv, const uint8_t *png, size_t png_n) {
    Buf bin = {0}, j = {0};
    out->n = 0;
    jsonf(&j, "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[");
    for (int m = 0; m < nm; m++) jsonf(&j, "%s%d", m ? "," : "", m);
    jsonf(&j, "]}],\"nodes\":[");
    for (int m = 0; m < nm; m++)
        jsonf(&j, "%s{\"name\":\"%s\",\"mesh\":%d%s,\"translation\":[%g,0,0]}", m ? "," : "", ms[m].name, m, ms[m].joints ? ",\"skin\":0" : "", ms[m].tx);
    for (int k = 0; k < njn; k++) jsonf(&j, ",{\"name\":\"%s\"}", jn[k]);
    jsonf(&j, "],");
    if (njn) {
        jsonf(&j, "\"skins\":[{\"joints\":[");
        for (int k = 0; k < njn; k++) jsonf(&j, "%s%d", k ? "," : "", nm + k);
        jsonf(&j, "]}],");
    }
    Buf acc = {0}, views = {0};
    int na = 0, nviews = 0;
    jsonf(&j, "\"meshes\":[");
    for (int m = 0; m < nm; m++) {
        const GlbMesh *g = &ms[m];
        const int a0 = na;
        /* positions, normals, uvs, (joints, weights), indices */
        size_t at = bin.n;
        for (int v = 0; v < g->nv; v++) putf(&bin, g->pos[v][0]), putf(&bin, g->pos[v][1]), putf(&bin, g->pos[v][2]);
        jsonf(&views, "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", nviews ? "," : "", at, bin.n - at);
        jsonf(&acc, "%s{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"}", na ? "," : "", nviews++, g->nv), na++;
        at = bin.n;
        for (int v = 0; v < g->nv; v++) putf(&bin, 0), putf(&bin, 1), putf(&bin, 0);
        jsonf(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", at, bin.n - at);
        jsonf(&acc, ",{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"}", nviews++, g->nv), na++;
        if (!no_uv) {
            at = bin.n;
            for (int v = 0; v < g->nv; v++) putf(&bin, 0.25f * (float)v), putf(&bin, 0.5f);
            jsonf(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", at, bin.n - at);
            jsonf(&acc, ",{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC2\"}", nviews++, g->nv), na++;
        }
        if (g->joints) {
            at = bin.n;
            for (int v = 0; v < g->nv; v++) put(&bin, g->joints[v], 4);
            jsonf(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", at, bin.n - at);
            jsonf(&acc, ",{\"bufferView\":%d,\"componentType\":5121,\"count\":%d,\"type\":\"VEC4\"}", nviews++, g->nv), na++;
            at = bin.n;
            for (int v = 0; v < g->nv; v++)
                for (int k = 0; k < 4; k++) putf(&bin, g->weights[v][k]);
            jsonf(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", at, bin.n - at);
            jsonf(&acc, ",{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC4\"}", nviews++, g->nv), na++;
        }
        at = bin.n;
        put(&bin, g->idx, (size_t)g->ni * 2);
        pad(&bin, (bin.n + 3) & ~(size_t)3, 0);
        jsonf(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%d}", at, g->ni * 2);
        jsonf(&acc, ",{\"bufferView\":%d,\"componentType\":5123,\"count\":%d,\"type\":\"SCALAR\"}", nviews++, g->ni), na++;
        int a = a0;
        jsonf(&j, "%s{\"name\":\"%s\",\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d", m ? "," : "", g->name, a, a + 1);
        a += 2;
        if (!no_uv) jsonf(&j, ",\"TEXCOORD_0\":%d", a++);
        if (g->joints) jsonf(&j, ",\"JOINTS_0\":%d,\"WEIGHTS_0\":%d", a, a + 1), a += 2;
        jsonf(&j, "},\"indices\":%d%s}]}", a, png && m == 0 ? ",\"material\":0" : "");
    }
    jsonf(&j, "],");
    if (png) {
        size_t at = bin.n;
        put(&bin, png, png_n);
        pad(&bin, (bin.n + 3) & ~(size_t)3, 0);
        jsonf(&views, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", at, png_n);
        jsonf(&j, "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],\"textures\":[{\"source\":0}],"
                  "\"images\":[{\"bufferView\":%d,\"mimeType\":\"image/png\"}],", nviews++);
    }
    put(&j, "\"accessors\":[", 13), put(&j, acc.b, acc.n), put(&j, "],\"bufferViews\":[", 17), put(&j, views.b, views.n);
    if (uri) jsonf(&j, "],\"buffers\":[{\"uri\":\"%s\",\"byteLength\":%zu}]}", uri, bin.n);
    else jsonf(&j, "],\"buffers\":[{\"byteLength\":%zu}]}", bin.n);
    pad(&j, (j.n + 3) & ~(size_t)3, ' ');
    const uint32_t total = 12 + 8 + (uint32_t)j.n + (uri ? 0 : 8 + (uint32_t)bin.n);
    put(out, "glTF", 4), put32(out, 2), put32(out, total);
    put32(out, (uint32_t)j.n), put(out, "JSON", 4), put(out, j.b, j.n);
    if (!uri) put32(out, (uint32_t)bin.n), put(out, "BIN\0", 4), put(out, bin.b, bin.n);
    free(bin.b), free(j.b), free(acc.b), free(views.b);
}

static const float quad_p[4][3] = {{0, 0, 0}, {0.5f, 0, 0}, {0.5f, 1, 0}, {0, 1, 0}};   /* glTF: y up */
static const uint16_t quad_i[6] = {0, 1, 2, 0, 2, 3};
static const float tri_p[3][3] = {{0, 1.8f, 0}, {0.1f, 1.8f, 0}, {0, 1.9f, 0}};         /* near char_a's head */
static const uint16_t tri_i[3] = {0, 1, 2};
static const uint8_t tri_j[3][4] = {{0, 1, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}};
static const float tri_w[3][4] = {{0.75f, 0.25f, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
static const float coll_p[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}};

static const uint8_t tone_ogg[] = {   /* a 440 Hz sine, 0.25 s, 8 kHz mono, made with ffmpeg's libvorbis */
    0x4f,0x67,0x67,0x53,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x39,0x69,0xfd,0x51,0x00,0x00,
    0x00,0x00,0x0f,0x6e,0x3f,0x54,0x01,0x1e,0x01,0x76,0x6f,0x72,0x62,0x69,0x73,0x00,0x00,0x00,0x00,0x01,
    0x40,0x1f,0x00,0x00,0x00,0x00,0x00,0x00,0xb0,0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x99,0x01,0x4f,0x67,
    0x67,0x53,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x39,0x69,0xfd,0x51,0x01,0x00,0x00,0x00,
    0x51,0xab,0x65,0x47,0x0b,0x3f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xb5,0x03,0x76,0x6f,0x72,
    0x62,0x69,0x73,0x0c,0x00,0x00,0x00,0x4c,0x61,0x76,0x66,0x36,0x32,0x2e,0x33,0x2e,0x31,0x30,0x30,0x01,
    0x00,0x00,0x00,0x1f,0x00,0x00,0x00,0x65,0x6e,0x63,0x6f,0x64,0x65,0x72,0x3d,0x4c,0x61,0x76,0x63,0x36,
    0x32,0x2e,0x31,0x31,0x2e,0x31,0x30,0x30,0x20,0x6c,0x69,0x62,0x76,0x6f,0x72,0x62,0x69,0x73,0x01,0x05,
    0x76,0x6f,0x72,0x62,0x69,0x73,0x12,0x42,0x43,0x56,0x01,0x00,0x00,0x01,0x00,0x0c,0x52,0x14,0x21,0x25,
    0x19,0x53,0x4a,0x63,0x08,0x95,0x52,0x52,0x29,0x05,0x1d,0x63,0x50,0x5b,0x47,0x1d,0x63,0xd4,0x39,0x46,
    0x21,0x64,0x10,0x53,0x88,0x49,0x19,0xa5,0x7b,0x4f,0x2a,0x95,0x58,0x4a,0xc8,0x11,0x52,0x58,0x29,0x45,
    0x1d,0x53,0x4c,0x53,0x49,0x95,0x52,0x96,0x29,0x45,0x1d,0x63,0x14,0x53,0x48,0x21,0x53,0xd6,0x31,0x65,
    0xa1,0x73,0x14,0x4b,0x86,0x49,0x09,0x25,0x6c,0x4d,0xae,0x74,0x16,0x4b,0xe8,0x99,0x63,0x96,0x31,0x46,
    0x1d,0x63,0xce,0x5a,0x4a,0x9d,0x63,0xd6,0x31,0x45,0x1d,0x63,0x52,0x52,0x49,0xa1,0x73,0x18,0x3a,0x66,
    0x25,0x64,0x14,0x3a,0x46,0xc5,0xe8,0x62,0x7c,0x30,0x3a,0x95,0xa2,0x42,0x28,0xbe,0xc7,0xde,0x52,0xe9,
    0x2d,0x85,0x8a,0x5b,0x8a,0xbd,0xd7,0x1a,0x53,0xeb,0x2d,0x84,0x18,0x4b,0x69,0xc1,0x08,0x61,0x73,0xed,
    0xb5,0xd5,0xdc,0x4a,0x6a,0xc5,0x18,0x63,0x8c,0x31,0xc6,0xc5,0xe2,0x53,0x28,0x82,0xd0,0x90,0x55,0x00,
    0x00,0x01,0x00,0x00,0x40,0x04,0x01,0x42,0x43,0x56,0x01,0x00,0x0a,0x00,0x00,0xc2,0x50,0x0c,0x45,0x51,
    0x80,0xd0,0x90,0x55,0x00,0x40,0x06,0x00,0x80,0x00,0x14,0x45,0x71,0x14,0xc7,0x71,0x1c,0x47,0x92,0x24,
    0xcb,0x02,0x42,0x43,0x56,0x01,0x00,0x40,0x00,0x00,0x02,0x00,0x00,0x28,0x8e,0xe1,0x28,0x92,0x23,0x49,
    0x92,0x64,0x59,0x96,0x65,0x59,0x96,0xa6,0x79,0x96,0xa8,0xb9,0xaa,0x2f,0xfb,0xae,0x2e,0xeb,0xae,0xed,
    0xea,0xba,0x0e,0x84,0x86,0xac,0x04,0x00,0xc8,0x00,0x00,0x18,0x86,0x21,0x87,0xde,0x49,0xcc,0x90,0x53,
    0x90,0x49,0x26,0x29,0x55,0xcc,0x39,0x08,0xa1,0xf5,0x0e,0x39,0xe5,0x14,0x64,0xd2,0x52,0xc6,0x98,0x62,
    0x8c,0x51,0xce,0x90,0x53,0x0c,0x31,0x05,0x31,0x86,0xd0,0x29,0x85,0x10,0xd4,0x4e,0x39,0xa5,0x0c,0x22,
    0x08,0x43,0x48,0x9d,0x64,0xce,0x20,0x4b,0x3d,0xe8,0xe0,0x62,0xe7,0x38,0x10,0x1a,0xb2,0x22,0x00,0x88,
    0x02,0x00,0x00,0x8c,0x41,0x8c,0x21,0xc6,0x90,0x73,0x0c,0x4a,0x06,0x21,0x72,0x8e,0x49,0xc8,0x20,0x44,
    0xce,0x39,0x29,0x9d,0x94,0x4c,0x4a,0x28,0xad,0xb4,0x96,0x49,0x09,0x2d,0x95,0xd6,0x22,0xe7,0x9c,0x94,
    0x4e,0x4a,0x26,0xa5,0xb4,0x16,0x52,0xcb,0xa4,0x94,0xd6,0x42,0x2b,0x05,0x00,0x00,0x04,0x38,0x00,0x00,
    0x04,0x58,0x08,0x85,0x86,0xac,0x08,0x00,0xa2,0x00,0x00,0x10,0x83,0x90,0x52,0x48,0x29,0xc4,0x94,0x62,
    0x4e,0x31,0x87,0x94,0x52,0x8e,0x29,0xc7,0x90,0x52,0xcc,0x39,0xc5,0x98,0x72,0x8c,0x31,0xe8,0x20,0x54,
    0xcc,0x31,0xc8,0x1c,0x84,0x48,0x29,0xc5,0x18,0x73,0x4e,0x39,0xe6,0x20,0x64,0x0c,0x2a,0xe6,0x1c,0x84,
    0x0c,0x32,0x01,0x00,0x00,0x01,0x0e,0x00,0x00,0x01,0x16,0x42,0xa1,0x21,0x2b,0x02,0x80,0x38,0x01,0x00,
    0x83,0x24,0x69,0x9a,0xa5,0x69,0xa2,0x68,0x69,0x9a,0x28,0x7a,0xa6,0xa8,0xaa,0xa2,0x28,0xaa,0xaa,0xe5,
    0x79,0xa6,0xe9,0x99,0xa6,0xaa,0x7a,0xa2,0xa9,0xaa,0xa6,0xaa,0xba,0xae,0xa9,0xaa,0xae,0x6c,0x79,0x9e,
    0x69,0x7a,0xa6,0xa8,0xaa,0x9e,0x29,0xaa,0xaa,0xa9,0xaa,0xae,0x6b,0xaa,0xaa,0xeb,0x8a,0xaa,0x6a,0xcb,
    0xa6,0xab,0xda,0xb6,0xe9,0xaa,0xb6,0xec,0xca,0xb2,0x6e,0xbb,0xb2,0xac,0xdb,0x9e,0xaa,0xca,0xb6,0xa9,
    0xba,0xb2,0x6e,0xaa,0xae,0x6d,0xbb,0xb2,0x6c,0xeb,0xae,0x2c,0xdb,0xba,0xe4,0x79,0xaa,0xea,0x99,0xa6,
    0xeb,0x7a,0xa6,0xe9,0xba,0xaa,0xeb,0xda,0xb2,0xea,0xba,0xb2,0xed,0x99,0xa6,0xeb,0x8a,0xaa,0x2b,0xdb,
    0xa6,0xeb,0xca,0xb2,0xeb,0xca,0xb6,0xad,0xca,0xb2,0xae,0x6b,0xa6,0xe9,0xba,0xa2,0xab,0xda,0xae,0xa9,
    0xba,0xb2,0xed,0xca,0xae,0x6d,0xbb,0xb2,0xac,0xfb,0xa6,0xeb,0xea,0xb6,0xea,0xca,0xba,0xae,0xca,0xb2,
    0xee,0xdb,0xb6,0xae,0xfb,0xb2,0xad,0x0b,0xbb,0xe8,0xba,0xb6,0xae,0xca,0xae,0xae,0xab,0xb2,0xac,0xeb,
    0xb2,0x2d,0xeb,0xb6,0x6c,0xdb,0x42,0xc9,0xf3,0x54,0xd5,0x33,0x4d,0xd7,0xf5,0x4c,0xd3,0x75,0x55,0xd7,
    0xb5,0x6d,0xd5,0x75,0x6d,0x5b,0x33,0x4d,0xd7,0x35,0x5d,0x57,0x96,0x45,0xd5,0x75,0x65,0xd5,0x95,0x75,
    0x5d,0x75,0x65,0x5b,0xf7,0x4c,0xd3,0x75,0x4d,0x57,0x95,0x65,0xd3,0x55,0x65,0x59,0x95,0x65,0xdd,0x76,
    0x65,0x57,0x97,0x45,0xd7,0xb5,0x6d,0x55,0x96,0x7d,0x5d,0x75,0x65,0x5f,0x97,0x6d,0xdd,0xf7,0x65,0x59,
    0xd7,0x7d,0xd3,0x75,0x75,0x5b,0x95,0x65,0xdb,0x57,0x65,0x59,0xf7,0x65,0x5d,0xf7,0x85,0x59,0xb7,0x7d,
    0xdd,0x53,0x55,0x5b,0x37,0x5d,0x57,0xd7,0x4d,0xd7,0xd5,0x7d,0x5b,0xd7,0x7d,0x61,0xb6,0x6d,0xdf,0x17,
    0x5d,0x57,0xd7,0x55,0xd9,0xd6,0x85,0x55,0x96,0x75,0xdf,0xd6,0x7d,0x65,0x98,0x75,0x9d,0x30,0xba,0xae,
    0xae,0xab,0xb6,0xec,0xeb,0xaa,0x2c,0xeb,0xbe,0xae,0xeb,0xc6,0x30,0xeb,0xba,0x30,0xac,0xba,0x6d,0xfc,
    0xae,0xad,0x0b,0xc3,0xab,0xeb,0xc6,0xb1,0xeb,0xbe,0xae,0xdc,0xbe,0x8f,0x6a,0xdb,0xbe,0xf0,0xea,0xb6,
    0x31,0xbc,0xba,0x6e,0x1c,0xbb,0xb0,0x1b,0xbf,0xed,0xfb,0xc6,0xb1,0xa9,0xaa,0x6d,0x9b,0xae,0xab,0xeb,
    0xa6,0x2b,0xeb,0xba,0x6c,0xeb,0xbe,0x6f,0xeb,0xba,0x71,0x8c,0xae,0xab,0xeb,0xaa,0x2c,0xfb,0xba,0xea,
    0xca,0xbe,0x6f,0xeb,0xba,0xf0,0xeb,0xbe,0x2f,0x0c,0xa3,0xeb,0xea,0xba,0x2a,0xcb,0xba,0xb0,0xda,0xb2,
    0xaf,0xcb,0xba,0x2e,0x0c,0xbb,0xae,0x1b,0xc3,0x6a,0xdb,0xc2,0xee,0xda,0xba,0x70,0xcc,0xb2,0x2e,0x0c,
    0xb7,0xef,0x2b,0xc7,0xaf,0x0b,0x43,0xd5,0xb6,0x85,0xe1,0xd5,0x75,0xa3,0xab,0xdb,0xc6,0x6f,0x0b,0xc3,
    0xd2,0x37,0x76,0xbe,0x00,0x00,0x80,0x01,0x07,0x00,0x80,0x00,0x13,0xca,0x40,0xa1,0x21,0x2b,0x02,0x80,
    0x38,0x01,0x00,0x06,0x21,0x08,0x15,0x63,0x10,0x2a,0xc6,0x20,0x84,0x10,0x52,0x0a,0x21,0xa4,0x54,0x31,
    0x06,0x21,0x63,0x0e,0x4a,0xc6,0x1c,0x94,0x10,0x4a,0x49,0x21,0x94,0xd2,0x2a,0xc6,0x20,0x64,0x8e,0x49,
    0xc8,0x1c,0x93,0x10,0x4a,0x68,0xa9,0x94,0xd0,0x4a,0x28,0xa5,0xa5,0x50,0x4a,0x4b,0xa1,0x94,0xd6,0x52,
    0x6a,0x2d,0xa6,0xd4,0x5a,0x0c,0xa1,0xb4,0x14,0x4a,0x69,0xad,0x94,0xd2,0x5a,0x6a,0x29,0xb6,0xd4,0x52,
    0x6c,0x15,0x63,0x10,0x32,0xe7,0xa4,0x64,0x8e,0x49,0x28,0xa5,0xb4,0x56,0x4a,0x69,0x29,0x73,0x4c,0x4a,
    0xc6,0xa0,0xa4,0x0e,0x42,0x2a,0xa5,0xa4,0xd2,0x4a,0x49,0xad,0x65,0xce,0x49,0xc9,0xa0,0xa3,0xd2,0x39,
    0x48,0xa9,0xa4,0xd2,0x52,0x49,0xa9,0xb5,0x50,0x4a,0x6b,0xa1,0x94,0xd6,0x4a,0x4a,0xb1,0xa5,0xd2,0x4a,
    0x6d,0xad,0xc5,0x1a,0x4a,0x69,0x2d,0xa4,0xd2,0x5a,0x49,0xa9,0xb5,0xd4,0x52,0x6d,0xad,0xb5,0x5a,0x23,
    0xc6,0x20,0x64,0x8c,0x41,0xc9,0x9c,0x93,0x52,0x4a,0x49,0xa9,0x94,0xd2,0x5a,0xe6,0x9c,0x94,0x0e,0x3a,
    0x2a,0x99,0x83,0x92,0x4a,0x29,0xa9,0x95,0x92,0x52,0xac,0x98,0x93,0xd2,0x41,0x28,0x25,0x83,0x8c,0x4a,
    0x49,0xa5,0xb5,0x92,0x4a,0x2b,0xa1,0x94,0xd6,0x4a,0x4a,0xb1,0x85,0x52,0x5a,0x6b,0xad,0xd5,0x98,0x52,
    0x4b,0x35,0x94,0x92,0x5a,0x49,0xa9,0xc5,0x50,0x4a,0x6b,0xad,0xb5,0x1a,0x53,0x2b,0x35,0x85,0x50,0x52,
    0x0b,0xa5,0xb4,0x16,0x4a,0x69,0xad,0xb5,0x56,0x6b,0x6a,0x2d,0xb6,0x50,0x42,0x6b,0xa1,0xa4,0x16,0x4b,
    0x2a,0x31,0xb5,0x16,0x63,0x6d,0xad,0xc5,0x18,0x4a,0x69,0xad,0xa4,0x12,0x5b,0x29,0xa9,0xc5,0x16,0x5b,
    0x8d,0xad,0xb5,0x58,0x53,0x4b,0x35,0x96,0x92,0x62,0x6c,0xad,0xd5,0xd8,0x4a,0x2d,0x39,0xd6,0x5a,0x6b,
    0x4a,0x2d,0xd6,0xd2,0x52,0x8c,0xad,0xb5,0x98,0x5b,0x4c,0xb9,0xc5,0x58,0x6b,0x0d,0x25,0xb4,0x16,0x4a,
    0x69,0xad,0x94,0xd2,0x5a,0x4a,0xad,0xc5,0xd6,0x5a,0xad,0xa1,0x94,0xd6,0x4a,0x2a,0xb1,0x95,0x92,0x5a,
    0x6c,0xad,0xd5,0xd8,0x5a,0x8c,0x35,0x94,0xd2,0x62,0x29,0x29,0xb5,0x90,0x4a,0x6c,0xad,0xb5,0x58,0x5b,
    0x6c,0x35,0xa6,0x96,0x62,0x6c,0xb1,0xd5,0x58,0x52,0x8b,0x31,0xc6,0x58,0x73,0x4b,0xb5,0xd5,0x94,0x5a,
    0x8b,0xad,0xb5,0x58,0x4b,0x2b,0x35,0xc6,0x18,0x6b,0x6e,0x35,0xe5,0x52,0x00,0x00,0xc0,0x80,0x03,0x00,
    0x40,0x80,0x09,0x65,0xa0,0xd0,0x90,0x95,0x00,0x40,0x14,0x00,0x00,0x60,0x0c,0x63,0x8c,0x41,0x68,0x14,
    0x72,0xcc,0x39,0x29,0x8d,0x52,0xce,0x39,0x27,0x25,0x73,0x0e,0x42,0x08,0x29,0x65,0xce,0x41,0x08,0x21,
    0xa5,0xce,0x39,0x08,0xa5,0xb4,0xd4,0x39,0x07,0xa1,0x94,0x94,0x42,0x29,0x29,0xa5,0x14,0x5b,0x28,0x25,
    0xa5,0xd6,0x5a,0x2c,0x00,0x00,0xa0,0xc0,0x01,0x00,0x20,0xc0,0x06,0x4d,0x89,0xc5,0x01,0x0a,0x0d,0x59,
    0x09,0x00,0x44,0x01,0x00,0x20,0xc6,0x28,0xc5,0x18,0x84,0xc6,0x20,0xa5,0x18,0x83,0xd0,0x18,0xa3,0x14,
    0x63,0x10,0x2a,0xa5,0x18,0x73,0x0e,0x42,0xa5,0x14,0x63,0xce,0x41,0xc8,0x18,0x73,0xce,0x41,0x29,0x19,
    0x63,0xce,0x41,0x27,0x25,0x84,0x10,0x42,0x29,0xa5,0x84,0x10,0x42,0x28,0xa5,0x94,0x02,0x00,0x00,0x0a,
    0x1c,0x00,0x00,0x02,0x6c,0xd0,0x94,0x58,0x1c,0xa0,0xd0,0x90,0x15,0x01,0x40,0x14,0x00,0x00,0x60,0x0c,
    0x62,0x0c,0x31,0x86,0x20,0x74,0x52,0x3a,0x29,0x11,0x84,0x4c,0x4a,0x27,0xa5,0x91,0x12,0x5a,0x0b,0x29,
    0x65,0x96,0x4a,0x8a,0x25,0xc6,0xcc,0x5a,0x89,0xad,0xc4,0xd8,0x48,0x09,0xad,0x85,0xd6,0x32,0x6b,0x25,
    0xc6,0xd2,0x62,0x46,0xad,0xc4,0x58,0x62,0x2a,0x00,0x00,0xec,0xc0,0x01,0x00,0xec,0xc0,0x42,0x28,0x34,
    0x64,0x25,0x00,0x90,0x07,0x00,0x40,0x18,0xa3,0x14,0x63,0xce,0x39,0x67,0x10,0x62,0xcc,0x39,0x08,0x21,
    0x34,0x08,0x31,0xe6,0x1c,0x84,0x10,0x2a,0xc6,0x9c,0x73,0x0e,0x42,0x08,0x15,0x63,0xce,0x39,0x07,0x21,
    0x84,0xce,0x39,0xe7,0x20,0x84,0x10,0x42,0xe7,0x9c,0x73,0x10,0x42,0x08,0xa1,0x83,0x10,0x42,0x08,0xa5,
    0x94,0xd2,0x41,0x08,0x21,0x84,0x52,0x4a,0xe9,0x20,0x84,0x10,0x42,0x29,0xa5,0x74,0x10,0x42,0x08,0xa1,
    0x94,0x52,0x0a,0x00,0x00,0x2a,0x70,0x00,0x00,0x08,0xb0,0x51,0x64,0x73,0x82,0x91,0xa0,0x42,0x43,0x56,
    0x02,0x00,0x79,0x00,0x00,0x80,0x31,0x4a,0x39,0x27,0x25,0xa5,0x46,0x29,0xc6,0x20,0xa4,0x14,0x5b,0xa3,
    0x14,0x63,0x10,0x52,0x6a,0xad,0x62,0x0c,0x42,0x4a,0xad,0xc5,0x58,0x31,0x06,0x21,0xa5,0xd6,0x62,0xec,
    0x20,0xa4,0xd4,0x5a,0x8c,0xb5,0x76,0x10,0x52,0x6a,0x2d,0xc6,0x5a,0x43,0x4a,0xad,0xc5,0x58,0x6b,0xce,
    0x21,0xa5,0xd6,0x62,0xac,0x35,0xd7,0xd4,0x5a,0x8c,0xb5,0xe6,0xdc,0x7b,0x6a,0x2d,0xc6,0x5a,0x73,0xce,
    0xb9,0x00,0x00,0xdc,0x05,0x07,0x00,0xb0,0x03,0x1b,0x45,0x36,0x27,0x18,0x09,0x2a,0x34,0x64,0x25,0x00,
    0x90,0x07,0x00,0x40,0x20,0xa4,0x14,0x63,0x8c,0x39,0x87,0x94,0x62,0x8c,0x31,0xe7,0x9c,0x43,0x4a,0x31,
    0xc6,0x98,0x73,0xce,0x29,0xc6,0x18,0x73,0xce,0x39,0xe7,0x14,0x63,0x8c,0x39,0xe7,0x9c,0x73,0x8c,0x31,
    0xe7,0x9c,0x73,0xce,0x39,0xc6,0x98,0x73,0xce,0x39,0xe7,0x9c,0x73,0xce,0x39,0xe7,0xa0,0x83,0x90,0x39,
    0xe7,0x9c,0x73,0xd0,0x41,0xe8,0x9c,0x73,0xce,0x39,0x08,0x21,0x74,0xce,0x39,0xe7,0x1c,0x84,0x10,0x0a,
    0x00,0x00,0x2a,0x70,0x00,0x00,0x08,0xb0,0x51,0x64,0x73,0x82,0x91,0xa0,0x42,0x43,0x56,0x02,0x00,0xe1,
    0x00,0x00,0x80,0x31,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0xa8,0xa3,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x52,0x02,0x21,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0x95,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,
    0x4a,0x29,0xa5,0x00,0x20,0xdf,0x0a,0x07,0x00,0xff,0x07,0x1b,0x67,0x58,0x49,0x3a,0x2b,0x1c,0x0d,0x2e,
    0x34,0x64,0x25,0x00,0x10,0x0e,0x00,0x00,0x18,0xc3,0x18,0x84,0x8c,0x39,0x27,0x25,0xa5,0x86,0x31,0x08,
    0xa5,0x74,0x4e,0x4a,0x49,0x25,0x35,0x8c,0x41,0x28,0xa5,0x73,0x12,0x52,0x4a,0x29,0x83,0xd0,0x5a,0x6a,
    0xa5,0xa4,0xd2,0x52,0x4a,0x19,0x84,0x94,0x62,0x0b,0x21,0x95,0x94,0x5a,0x0a,0xa5,0xb4,0x56,0x6b,0x29,
    0xa9,0xb5,0x94,0x52,0x28,0x29,0xc5,0x1a,0x4b,0x4a,0xa9,0xa5,0xd6,0x32,0xe7,0x24,0xa4,0x92,0x5a,0x4b,
    0xad,0xb6,0x98,0x39,0x07,0xa5,0xa4,0xd6,0x5a,0x6a,0xad,0xc5,0x10,0x42,0x4a,0xb1,0xb5,0xd6,0x52,0x6b,
    0xb1,0x75,0x52,0x52,0x49,0xad,0xb5,0xd6,0x5a,0x6d,0x2d,0xa4,0x94,0x5a,0x6b,0x2d,0xc6,0xd6,0x62,0x6c,
    0x25,0xa5,0x96,0x5a,0x6b,0xa9,0xc5,0xd6,0x5a,0x4c,0xa9,0xb5,0x16,0x5b,0x4b,0x2d,0xc6,0xd6,0x62,0x4b,
    0xad,0xc5,0xd8,0x62,0x8b,0x31,0xc6,0x1a,0x0b,0x00,0xe0,0x6e,0x70,0x00,0x80,0x48,0xb0,0x71,0x86,0x95,
    0xa4,0xb3,0xc2,0xd1,0xe0,0x42,0x43,0x56,0x02,0x00,0x21,0x01,0x00,0x04,0x32,0x4a,0x39,0xe7,0x9c,0x83,
    0x10,0x42,0x08,0x21,0x52,0x8a,0x31,0xe7,0xa0,0x83,0x10,0x42,0x08,0x21,0x44,0x4a,0x31,0xe6,0x9c,0x83,
    0x10,0x42,0x08,0x21,0x84,0x8c,0x31,0xe7,0x20,0x84,0x10,0x42,0x08,0xa1,0x94,0x90,0x31,0xe6,0x1c,0x84,
    0x10,0x42,0x08,0x21,0x84,0x52,0x3a,0xe7,0x20,0x84,0x50,0x4a,0x09,0xa5,0x94,0x52,0x4a,0xe7,0x1c,0x84,
    0x10,0x42,0x08,0xa5,0x94,0x52,0x4a,0x09,0x21,0x84,0x10,0x42,0x28,0xa5,0x94,0x52,0x4a,0x29,0x21,0x84,
    0x10,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0x25,0x84,0x10,0x42,0x28,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x84,
    0x10,0x42,0x28,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,0x10,0x42,0x28,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x12,0x42,0x08,0xa1,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x42,0x08,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x52,0x4a,0x28,0x21,0x84,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x09,0x25,0x94,0x52,0x4a,0x29,0xa5,0x94,
    0x52,0x4a,0x29,0x21,0x94,0x52,0x4a,0x29,0xa5,0x94,0x52,0x4a,0x29,0xa5,0x00,0x00,0x80,0x03,0x07,0x00,
    0x80,0x00,0x23,0xe8,0x24,0xa3,0xca,0x22,0x6c,0x34,0xe1,0xc2,0x03,0x10,0x00,0x00,0x00,0x02,0x00,0x02,
    0x4c,0x00,0x81,0x01,0x82,0x82,0x51,0x08,0x02,0x84,0x11,0x08,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0xf8,
    0x00,0x00,0x48,0x0a,0x80,0x88,0x88,0x68,0xe6,0x0c,0x0e,0x10,0x12,0x14,0x16,0x18,0x1a,0x1c,0x1e,0x20,
    0x22,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x4f,0x67,0x67,0x53,0x00,
    0x04,0xd0,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x39,0x69,0xfd,0x51,0x02,0x00,0x00,0x00,0x25,0x8f,0xe6,
    0x4e,0x09,0x12,0x10,0x10,0x11,0x11,0x10,0x11,0x11,0x14,0x9e,0x98,0x99,0x75,0xe5,0x0d,0xec,0x2b,0x00,
    0x00,0x00,0xa2,0xa2,0xd2,0x8c,0x8d,0xbf,0x00,0xa6,0x99,0x99,0xb3,0x93,0x57,0x00,0xb0,0x05,0x00,0x00,
    0x00,0x00,0x88,0x70,0x09,0xa2,0x99,0x99,0x75,0xe5,0x15,0x00,0x28,0x00,0x00,0x00,0x00,0x80,0xb0,0x52,
    0x00,0xa6,0x99,0x99,0xb3,0x93,0x57,0x00,0xd0,0x05,0x00,0x00,0x00,0x00,0xc8,0xe6,0x9d,0x02,0xa6,0x99,
    0x99,0xb3,0x93,0x57,0x00,0x70,0x05,0x00,0x00,0x00,0x00,0xe8,0x87,0x99,0x00,0xa6,0x99,0x99,0xb3,0x93,
    0x57,0x00,0x70,0x01,0x00,0x00,0x00,0x00,0xfd,0x7c,0x06,0xa6,0x99,0x99,0xb3,0x93,0x57,0x00,0x70,0x25,
    0x00,0x00,0x00,0x00,0xe8,0x87,0x99,0x04,0xa2,0x99,0x99,0x6d,0xe5,0x15,0x00,0x74,0x09,0x00,0x00,0x00,
    0x00,0x0a,0x79,0x87,0x04,0x96,0xcf,0x50,0x59,0xc1,0x2b,0x00,0xe8,0x31,0x00,0x00,0x20,0x08,0x27,0xf6,
    0x3b,0x32,0x72,0x7e,0x24,
};

static int glb_err(const Buf *b, const char *want) {
    MaOwnMesh m;
    char err[300] = "";
    int rc = ma_glb_load(b->b, b->n, &m, err, sizeof err);
    if (!rc) ma_own_mesh_free(&m);
    if (rc == 0 || !strstr(err, want)) {
        printf("  want \"%s\", got rc %d \"%s\"\n", want, rc, err);
        return 0;
    }
    return 1;
}

static void test_glb(void) {
    Buf b = {0};
    char err[300] = "";
    MaOwnMesh m;
    /* static: a quad moved 1 m along x, plus a collision node */
    const GlbMesh st[2] = {{"crate", 4, 6, quad_p, quad_i, NULL, NULL, 1.0f}, {"collision", 3, 3, coll_p, tri_i, NULL, NULL, 0}};
    make_glb(&b, st, 2, NULL, 0, NULL, 0, NULL, 0);
    int rc = ma_glb_load(b.b, b.n, &m, err, sizeof err);
    CHECK(rc == 0 && m.nv == 4 && m.ni == 6 && !m.skinned && m.ncol == 1, "a static .glb: 4 vertices, 6 indices, one collision triangle");
    if (rc) printf("  %s\n", err);
    else {
        /* glTF (x, y, z) -> game (x, -z, y); the node's translation applied */
        CHECK(m.pos[2][0] == 1.5f && m.pos[2][1] == 0 && m.pos[2][2] == 1.0f, "Y-up -> Z-up, node translation applied");
        CHECK(m.nrm[0][2] == 1.0f && m.uv[1][0] == 0.25f && m.uv[1][1] == 0.5f, "normals turned to +z, UVs kept");
        CHECK(m.col[0][2][1] == 1.0f && m.col[0][1][0] == 1.0f, "collision triangle in game axes");
        CHECK(m.lo[0] == 1.0f && m.hi[2] == 1.0f, "bounds");
        ma_own_mesh_free(&m);
    }
    /* skinned: joints by node name, the 3 strongest, renormalised; node transform ignored */
    const char *jn[2] = {"bip_sw_head", "bip_sw_neck"};
    const GlbMesh sk = {"part", 3, 3, tri_p, tri_i, tri_j, tri_w, 0};
    make_glb(&b, &sk, 1, jn, 2, NULL, 0, NULL, 0);
    rc = ma_glb_load(b.b, b.n, &m, err, sizeof err);
    CHECK(rc == 0 && m.skinned && m.slot[0][0] == 4 && m.slot[0][1] == 3 && m.w[0][0] == 0.75f && m.w[0][1] == 0.25f && m.slot[1][0] == 4 &&
              m.slot[1][1] == -1 && m.slot[2][0] == 3,
          "skinned: bip_sw_head = slot 4, bip_sw_neck = slot 3, weights kept");
    if (!rc) ma_own_mesh_free(&m);
    else printf("  %s\n", err);
    /* errors */
    const char *bad_j[2] = {"bip_sw_hed", "bip_sw_neck"};
    make_glb(&b, &sk, 1, bad_j, 2, NULL, 0, NULL, 0);
    CHECK(glb_err(&b, "unknown joint bip_sw_hed (closest: bip_sw_head)"), "unknown joint: the closest name");
    const char *root_j[2] = {"bip_sw", "bip_sw_neck"};
    make_glb(&b, &sk, 1, root_j, 2, NULL, 0, NULL, 0);
    CHECK(glb_err(&b, "joint bip_sw carries no vertices"), "a joint without palette slot");
    const float zero_w[3][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
    const GlbMesh nw = {"part", 3, 3, tri_p, tri_i, tri_j, zero_w, 0};
    make_glb(&b, &nw, 1, jn, 2, NULL, 0, NULL, 0);
    CHECK(glb_err(&b, "vertex 0 has no weight"), "a weightless vertex");
    const float big_p[3][3] = {{0, 0, 0}, {50, 0, 0}, {0, 180, 0}};
    const GlbMesh cm = {"cm", 3, 3, big_p, tri_i, NULL, NULL, 0};
    make_glb(&b, &cm, 1, NULL, 0, NULL, 0, NULL, 0);
    CHECK(glb_err(&b, "exported in centimetres"), "180 m tall: centimetres");
    make_glb(&b, st, 1, NULL, 0, NULL, 1, NULL, 0);
    CHECK(glb_err(&b, "no UVs"), "no UVs");
    make_glb(&b, st, 1, NULL, 0, "crate.bin", 0, NULL, 0);
    CHECK(glb_err(&b, "another file"), "an external buffer is refused");
    make_glb(&b, &st[1], 1, NULL, 0, NULL, 0, NULL, 0);
    CHECK(glb_err(&b, "no mesh with triangles"), "only a collision mesh");
    Buf junk = {0};
    put(&junk, "not a glb at all", 16);
    CHECK(glb_err(&junk, "not a glTF binary"), "not a .glb");
    /* too many vertices: 65538 (a triangle list of 21846 triangles) */
    const int NV = 65538;
    float (*bp)[3] = calloc((size_t)NV, sizeof *bp);
    uint16_t *bi = calloc((size_t)NV, sizeof *bi);
    for (int v = 0; v < NV; v++) bp[v][0] = (float)(v % 7) * 0.01f, bp[v][1] = (float)(v % 3) * 0.01f, bi[v] = (uint16_t)(v % 65535);
    const GlbMesh huge = {"huge", NV, NV, (const float (*)[3])bp, bi, NULL, NULL, 0};
    make_glb(&b, &huge, 1, NULL, 0, NULL, 0, NULL, 0);
    CHECK(glb_err(&b, "too many (at most 65535"), "too many vertices");
    free(bp), free(bi);
    /* joint names */
    CHECK(ma_joint_index("bip_sw_head") == 6 && ma_joint_index("head") == -1 && !strcmp(ma_joint_closest("bip_l_hand"), "bip_sw_l_hand") &&
              !strcmp(ma_joints[2], "bip_sw_pelvis"),
          "joint names and the closest one");
    /* own paths */
    CHECK(!ma_own_path_check("own/crate.glb", err, sizeof err) && !ma_own_path_check("own/sub/x.png", err, sizeof err) &&
              ma_own_path_check("own/../mod.ini", err, sizeof err) && ma_own_path_check("/etc/passwd", err, sizeof err) &&
              ma_own_path_check("own/a/b/c.png", err, sizeof err) && ma_own_path_check("crate.glb", err, sizeof err) && strstr(err, "outside the mod's own folder"),
          "own paths stay inside own/");
    free(b.b), free(junk.b);
}

/* A 4x4 DXT1 .dds with its full chain (4x4, 2x2, 1x1): one block per level, colour0 = c0 (RGB565), all texels index 0. */
static void make_dds_bc1(Buf *b, uint16_t c0) {
    b->n = 0;
    uint8_t h[128] = {0};
    memcpy(h, "DDS ", 4);
    const uint32_t f[] = {124, 0x1007 | 0x20000, 4, 4, 0, 0, 3};
    for (int k = 0; k < 7; k++)
        for (int q = 0; q < 4; q++) h[4 + 4 * k + q] = (uint8_t)(f[k] >> (8 * q));
    h[4 + 72] = 32, h[4 + 76] = 4, memcpy(h + 4 + 80, "DXT1", 4);
    h[4 + 104] = 0x08, h[4 + 105] = 0x10, h[4 + 106] = 0x40;
    put(b, h, 128);
    for (int l = 0; l < 3; l++) {
        const uint8_t blk[8] = {(uint8_t)c0, (uint8_t)(c0 >> 8), 0, 0, 0, 0, 0, 0};
        put(b, blk, 8);
    }
}

static void test_own_media(const char *tmp) {
    char path[800], err[300];
    int w, h;
    uint8_t *rgba;
    /* PNG */
    uint8_t px[2 * 3 * 4];
    for (int i = 0; i < 6; i++) px[4 * i] = (uint8_t)(40 * i), px[4 * i + 1] = 7, px[4 * i + 2] = 9, px[4 * i + 3] = 255;
    snprintf(path, sizeof path, "%s/own_t.png", tmp);
    ma_png_write(path, 2, 3, px);
    size_t n;
    uint8_t *f = slurp(path, &n);
    CHECK(f && !ma_image_load(f, n, &w, &h, &rgba, err, sizeof err) && w == 2 && h == 3 && !memcmp(rgba, px, sizeof px), "PNG: texels read back");
    if (f && !memcmp(f, "\x89PNG", 4)) free(rgba);
    free(f);
    uint8_t *wide = calloc(2049 * 4, 1);
    ma_png_write(path, 2049, 1, wide);
    free(wide);
    f = slurp(path, &n);
    CHECK(f && ma_image_load(f, n, &w, &h, &rgba, err, sizeof err) && strstr(err, "bad texture size 2049x1"), "PNG: too wide");
    free(f);
    /* DDS BC1: pure red (RGB565 0xF800) */
    Buf d = {0};
    make_dds_bc1(&d, 0xF800);
    CHECK(!ma_image_load(d.b, d.n, &w, &h, &rgba, err, sizeof err) && w == 4 && h == 4 && rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255,
          "DDS BC1 decoded");
    if (w == 4) free(rgba);
    d.n -= 8;
    CHECK(ma_image_load(d.b, d.n, &w, &h, &rgba, err, sizeof err) && strstr(err, "the DDS cannot be used"), "DDS: a partial chain is refused");
    free(d.b);
    CHECK(ma_image_load((const uint8_t *)"GIF89a", 6, &w, &h, &rgba, err, sizeof err) && strstr(err, "not a PNG or DDS"), "other images refused");
    /* WAV: written, read back */
    int16_t pcm[200];
    for (int i = 0; i < 200; i++) pcm[i] = (int16_t)(i * 100 - 10000);
    snprintf(path, sizeof path, "%s/own_s.wav", tmp);
    CHECK(!ma_wav_write(path, pcm, 2, 22050, 100), "WAV written");
    f = slurp(path, &n);
    int16_t *out = NULL;
    int ch, rate, frames;
    CHECK(f && n == 44 + 400 && !ma_sound_decode(f, n, &out, &ch, &rate, &frames, err, sizeof err) && ch == 2 && rate == 22050 && frames == 100 &&
              !memcmp(out, pcm, sizeof pcm),
          "WAV: 16-bit stereo read back");
    free(out), free(f);
    /* OGG: a 440 Hz tone */
    out = NULL;
    int rc = ma_sound_decode(tone_ogg, sizeof tone_ogg, &out, &ch, &rate, &frames, err, sizeof err);
    int peak = 0, zc = 0;
    for (int i = 1; !rc && i < frames; i++) {
        peak = abs(out[i]) > peak ? abs(out[i]) : peak;
        zc += (out[i - 1] < 0) != (out[i] < 0);
    }
    CHECK(!rc && ch == 1 && rate == 8000 && frames >= 1900 && frames <= 2100 && peak > 3000 && zc >= 200 && zc <= 240,
          "OGG decoded: mono 8 kHz, 0.25 s, about 220 zero crossings (440 Hz)");
    if (rc) printf("  %s\n", err);
    else printf("  ogg: %d ch, %d Hz, %d frames, peak %d, %d zero crossings\n", ch, rate, frames, peak, zc);
    free(out);
    CHECK(ma_sound_decode((const uint8_t *)"OggSjunkjunkjunk", 16, &out, &ch, &rate, &frames, err, sizeof err) && strstr(err, "cannot be decoded"),
          "a broken OGG is refused");
    CHECK(ma_sound_decode((const uint8_t *)"fLaC0000", 8, &out, &ch, &rate, &frames, err, sizeof err) && strstr(err, "not a WAV or OGG"), "FLAC refused");
}

static uint8_t *file_bytes(const char *dir, const char *name, size_t *n) {
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return slurp(p, n);
}

static int own_build_err(const char *disc, const char *mod, const char *body, const char *want) {
    char out[1000], err[512] = "";
    snprintf(out, sizeof out, "%s/out-err", mod);
    mkdir(out, 0755);
    char text[3000];
    snprintf(text, sizeof text, "format = 1\n%s", body);
    MaRecipe r;
    if (ma_recipe_parse(text, strlen(text), "assets.ini", &r, err, sizeof err)) {
        printf("  parse: %s\n", err);
        return 0;
    }
    snprintf(r.dir, sizeof r.dir, "%s", mod);
    int files = 0;
    uint64_t bytes = 0;
    int n = ma_build(&r, disc, out, &files, &bytes, err, sizeof err);
    ma_recipe_free(&r);
    if (n >= 0 || !strstr(err, want)) {
        printf("  want \"%s\", got %d \"%s\"\n", want, n, err);
        return 0;
    }
    return 1;
}

static void test_own_build(const char *tmp, const char *disc) {
    char mod[700], own[800], out[800], path[1400], err[512] = "";
    snprintf(mod, sizeof mod, "%s/ma_own_mod", tmp);
    snprintf(own, sizeof own, "%s/own", mod);
    snprintf(out, sizeof out, "%s/out", mod);
    mkdir(mod, 0755), mkdir(own, 0755), mkdir(out, 0755);
    Buf b = {0};
    const GlbMesh st[2] = {{"crate", 4, 6, quad_p, quad_i, NULL, NULL, 0}, {"collision", 3, 3, coll_p, tri_i, NULL, NULL, 0}};
    uint8_t px[8 * 8 * 4];
    for (int i = 0; i < 64; i++) px[4 * i] = 200, px[4 * i + 1] = 120, px[4 * i + 2] = 40, px[4 * i + 3] = 255;
    snprintf(path, sizeof path, "%s/wood.png", own);
    ma_png_write(path, 8, 8, px);
    size_t pn;
    uint8_t *png = slurp(path, &pn);
    make_glb(&b, st, 2, NULL, 0, NULL, 0, png, pn);
    snprintf(path, sizeof path, "%s/crate.glb", own);
    write_file(path, b.b, b.n);
    const char *jn[2] = {"bip_sw_head", "bip_sw_neck"};
    const GlbMesh sk = {"part", 3, 3, tri_p, tri_i, tri_j, tri_w, 0};
    const char *heads[2] = {"bip_sw_head", "bip_sw_head"};   /* char_a's draw moves only with the head */
    make_glb(&b, &sk, 1, heads, 2, NULL, 0, NULL, 0);
    (void)jn;
    snprintf(path, sizeof path, "%s/part.glb", own);
    write_file(path, b.b, b.n);
    const GlbMesh rg = {"pin", 3, 3, tri_p, tri_i, NULL, NULL, 0};
    make_glb(&b, &rg, 1, NULL, 0, NULL, 0, NULL, 0);
    snprintf(path, sizeof path, "%s/pin.glb", own);
    write_file(path, b.b, b.n);
    const char *pelvis[2] = {"bip_sw_pelvis", "bip_sw_neck"};
    make_glb(&b, &sk, 1, pelvis, 2, NULL, 0, NULL, 0);
    snprintf(path, sizeof path, "%s/pelvis.glb", own);
    write_file(path, b.b, b.n);
    snprintf(path, sizeof path, "%s/tone.ogg", own);
    write_file(path, tone_ogg, sizeof tone_ogg);
    int16_t pcm[100] = {0};
    snprintf(path, sizeof path, "%s/click.wav", own);
    ma_wav_write(path, pcm, 1, 11025, 100);
    const char *text =
        "format = 1\n"
        "[source a]\nchar = char_a\n"
        "[source crate-glb]\nfile = own/crate.glb\n"
        "[source part-glb]\nfile = own/part.glb\n"
        "[source pin-glb]\nfile = own/pin.glb\n"
        "[mesh crate]\nfrom = crate-glb\ntake = all\nhost = dyn_s_newspaper_c\ncolour = ff000000\ntexture = own/crate.glb\ngroup = crate\nplaced = yes\n"
        "[mesh hat]\nfrom = part-glb\ntake = all\nrigid = joint bip_sw_head, of a\nhost = dyn_dest_ltb\ntexture = own/wood.png\n"
        "[mesh part]\nfrom = part-glb\ntake = all\ntarget = a skin 0\ntexture = a.skin0\ngroup = part\n"
        "[mesh more]\nfrom = part-glb\ntake = all\ntarget = a\nappend = yes\ngroup = more\n"
        "[mesh pinned]\nfrom = pin-glb\ntake = all\ntarget = a\njoint = bip_sw_head\ngroup = pinned\n"
        "[collision crate-col]\nfrom = crate-glb\nlevel = level95\nat = 10 20 -1\nheading = 90\nmaterial = 7\n"
        "[sound knock]\nsynth = sweep 140 60, seconds 0.3, decay 0.08, noise 0.3, seed 3\nsynth = tone 880, seconds 0.1, volume 0.2\n"
        "[sound tone]\nfile = own/tone.ogg\n"
        "[sound click]\nfile = own/click.wav\n";
    MaRecipe r;
    CHECK(ma_recipe_parse(text, strlen(text), "assets.ini", &r, err, sizeof err) == 0, "own recipe parses");
    snprintf(r.dir, sizeof r.dir, "%s", mod);
    int files = 0;
    uint64_t bytes = 0;
    int n = ma_build(&r, disc, out, &files, &bytes, err, sizeof err);
    CHECK(n == 9, "own recipe: 9 outputs");
    if (n != 9) printf("  %s\n", err);
    CHECK(!r.warn[0], "no warning");
    ma_recipe_free(&r);
    /* the crate: host dyn_s_newspaper_c's draw hash (from the synthetic prop chunk), object layout */
    Disc d;
    disc_open(&d, disc);
    disc_scan(&d);
    uint64_t host = 0;
    for (uint32_t i = 0; i < d.nchunks; i++)
        if (d.chunks[i].hash == PROP_HASH) {
            uint8_t *p = disc_chunk_read(&d, &d.chunks[i]);
            DiscProp pr;
            char why[100];
            if (p && !disc_prop_parse(p, d.chunks[i].size, &pr, why, sizeof why)) {
                uint16_t idx[PROP_NI];
                memcpy(idx, pr.idx, sizeof idx);
                host = meshpack_hash(idx, PROP_NI, 0, PROP_NV - 1, pr.vb, pr.stride, NULL, 0);
            }
            free(p);
        }
    uint64_t head = 0;   /* char_a's draw hash and skin */
    for (uint32_t i = 0; i < d.nchunks; i++)
        if (d.chunks[i].hash == disc_model_hash("char_a") && d.chunks[i].cls == DISC_CLASS_MESH) {
            uint8_t *p = disc_chunk_read(&d, &d.chunks[i]);
            DiscChar dc;
            char why[100];
            if (p && !disc_char_parse(p, d.chunks[i].size, &dc, why, sizeof why)) {
                uint16_t idx[9];
                memcpy(idx, dc.idx, sizeof idx);
                head = meshpack_hash(idx, 9, 0, 8, dc.s0, DISC_STRIDE0, dc.s1, DISC_STRIDE1);
            }
            free(p);
        }
    disc_close(&d);
    char name[200], sub[900];
    snprintf(sub, sizeof sub, "%s/crate", out);
    snprintf(name, sizeof name, "%016llx.wmesh", (unsigned long long)host);
    size_t len;
    uint8_t *wm = file_bytes(sub, name, &len);
    MeshPackMesh mm;
    CHECK(wm && !meshpack_parse_any(wm, len, host, &mm) && mm.nv == 4 && mm.ni == 6 && mm.stride0 == 36 && mm.stride1 == 0,
          "crate: the host's draw replaced by 4 vertices in the 36-byte object layout");
    if (wm && mm.nv == 4) {
        float p2[3], n0[3], uv1[2];
        memcpy(p2, mm.s0 + 2 * 36, 12), memcpy(n0, mm.s0 + 12, 12), memcpy(uv1, mm.s0 + 36 + 28, 8);
        CHECK(p2[0] == 0.5f && p2[1] == 0 && p2[2] == 1.0f && n0[2] == 1.0f && uv1[0] == 0.25f, "crate: Z-up positions, normals, UVs");
        meshpack_free(&mm);
    }
    free(wm);
    snprintf(name, sizeof name, "%016llx.png", (unsigned long long)host);
    uint8_t *tp = file_bytes(sub, name, &len);
    int w = 0, h = 0;
    uint8_t *rgba = NULL;
    CHECK(tp && !ma_image_load(tp, len, &w, &h, &rgba, err, sizeof err) && w == 8 && h == 8 && rgba[0] == 200 && rgba[2] == 40,
          "crate: the texture inside the .glb");
    free(tp), free(rgba);
    CHECK(file_bytes(sub, "placed", &len) != NULL, "crate: a placed group");
    /* the hat: the part rigid in char_a's head frame (slot 4 inverse bind: z - 1.8) on the Destroyers hat */
    snprintf(name, sizeof name, "%016llx.wmesh", 0x6830D7EB9625CFADull);
    wm = file_bytes(out, name, &len);
    CHECK(wm && !meshpack_parse_any(wm, len, 0x6830D7EB9625CFADull, &mm) && mm.nv == 3, "hat: on the spare hat host");
    if (wm && mm.nv == 3) {
        float p0[3];
        memcpy(p0, mm.s0, 12);
        CHECK(fabsf(p0[2] - 0.0f) < 1e-5f && p0[1] == 0.0f, "hat: in the head joint's frame (z 1.8 -> 0)");
        meshpack_free(&mm);
    }
    free(wm);
    snprintf(name, sizeof name, "%016llx.png", 0x6830D7EB9625CFADull);
    CHECK((tp = file_bytes(out, name, &len)) != NULL, "hat: own/wood.png");
    free(tp);
    /* the part: char_a skin 0's draw replaced by the skinned part, character layout */
    snprintf(sub, sizeof sub, "%s/part", out);
    snprintf(name, sizeof name, "%016llx-", (unsigned long long)head);
    DIR *dd = opendir(sub);
    struct dirent *e;
    int found = 0;
    while (dd && (e = readdir(dd)))
        if (strstr(e->d_name, ".wmesh") && !strncmp(e->d_name, name, 17)) {
            wm = file_bytes(sub, e->d_name, &len);
            found = wm && !meshpack_parse_any(wm, len, head, &mm) && mm.nv == 3 && mm.stride0 == 32 && mm.stride1 == 18;
            if (found) {
                int16_t sl[3];
                float wt[3];
                memcpy(sl, mm.s1, 6), memcpy(wt, mm.s1 + 6, 12);
                found = sl[0] == 12 && sl[1] == -3 && sl[2] == -3 && wt[0] == 1.0f && wt[1] == 0;
                meshpack_free(&mm);
            }
            free(wm);
        }
    if (dd) closedir(dd);
    CHECK(found, "part: <draw>-<skin>.wmesh, slots x3 (two weights on the head merged: 12, weight 1)");
    snprintf(sub, sizeof sub, "%s/more", out);
    snprintf(name, sizeof name, "%016llx.wmesh", (unsigned long long)head);
    wm = file_bytes(sub, name, &len);
    CHECK(wm && !meshpack_parse_any(wm, len, head, &mm) && mm.nv == 12 && mm.ni == 12, "append = yes: char_a's 9 vertices + the part's 3");
    if (wm && mm.nv == 12) meshpack_free(&mm);
    free(wm);
    snprintf(sub, sizeof sub, "%s/pinned", out);
    wm = file_bytes(sub, name, &len);
    found = wm && !meshpack_parse_any(wm, len, head, &mm) && mm.nv == 3;
    if (found) {
        int16_t sl[3];
        float wt[3];
        memcpy(sl, mm.s1 + 18, 6), memcpy(wt, mm.s1 + 18 + 6, 12);
        found = sl[0] == 12 && sl[1] == -3 && wt[0] == 1.0f && wt[1] == 0;
        meshpack_free(&mm);
    }
    free(wm);
    CHECK(found, "joint = bip_sw_head: a static mesh rigid on one joint");
    /* collision: the collision node's triangle turned 90 degrees and moved to (10, 20, -1) */
    char *col = (char *)file_bytes(out, "crate-col.col", &len);
    CHECK(col && strstr(col, "level = level95\nmaterial = 7\nflags = 0x23\n") && strstr(col, "tri 10.0000 20.0000 -1.0000 10.0000 21.0000 -1.0000 10.0000 20.0000 -1.0000") == NULL,
          "collision: header");
    /* (0,0,0) (1,0,0) (0,0,-1) glTF = game (0,0,0) (1,0,0) (0,1,0); turned 90: (0,0) (0,1) (-1,0) */
    CHECK(col && strstr(col, "tri 10.0000 20.0000 -1.0000 10.0000 21.0000 -1.0000 9.0000 20.0000 -1.0000"), "collision: the triangle turned and moved");
    free(col);
    /* sounds */
    uint8_t *wv = file_bytes(out, "knock.wav", &len);
    int16_t *pc = NULL;
    int ch, rate, frames;
    CHECK(wv && !ma_sound_decode(wv, len, &pc, &ch, &rate, &frames, err, sizeof err) && ch == 1 && rate == 22050 && frames == 6615,
          "synth: 0.3 s at 22050 Hz, mono");
    int peak = 0;
    for (int i = 0; pc && i < frames; i++) peak = abs(pc[i]) > peak ? abs(pc[i]) : peak;
    CHECK(peak > 8000 && peak < 32767 && pc && abs(pc[frames - 1]) < 200, "synth: loud, faded out at the end");
    free(wv), free(pc);
    wv = file_bytes(out, "tone.wav", &len);
    CHECK(wv && !ma_sound_decode(wv, len, &pc, &ch, &rate, &frames, err, sizeof err) && rate == 8000 && frames > 1900, "own/tone.ogg -> tone.wav");
    free(wv), free(pc);
    CHECK((wv = file_bytes(out, "click.wav", &len)) != NULL && len == 244, "own/click.wav -> click.wav");
    free(wv);
    /* errors, each with assets.ini:LINE: */
    CHECK(own_build_err(disc, mod, "[source a]\nchar = char_a\n[source p]\nfile = own/pelvis.glb\n[mesh m]\nfrom = p\ntake = all\ntarget = a skin 0\ntexture = a.skin0\n",
                        "assets.ini:8: joint bip_sw_pelvis does not move in char_a's draw (its joints: bip_sw_head)"),
          "a joint the target's draw does not animate");
    CHECK(own_build_err(disc, mod, "[source a]\nchar = char_a\n[source p]\nfile = own/pin.glb\n[mesh m]\nfrom = p\ntake = all\ntarget = a\n",
                        "assets.ini:8: own/pin.glb has no skin"),
          "a static mesh as a part needs joint =");
    CHECK(own_build_err(disc, mod, "[source a]\nchar = char_a\n[source p]\nfile = own/part.glb\n[mesh m]\nfrom = p\ntake = all\ntarget = a\njoint = bip_sw_head\n",
                        "joint = is for a mesh without a skin"),
          "joint = on a skinned mesh");
    CHECK(own_build_err(disc, mod, "[source p]\nfile = own/../mod.ini\n", "assets.ini:3: own/../mod.ini: a path outside the mod's own folder"), "path outside own/");
    CHECK(own_build_err(disc, mod, "[source p]\nfile = own/nothere.glb\n[mesh m]\nfrom = p\ntake = all\nhost = dyn_dest_ltb\ntexture = own/wood.png\n",
                        "assets.ini:3: own/nothere.glb: not in the mod's folder"),
          "a missing file");
    CHECK(own_build_err(disc, mod, "[source p]\nfile = own/crate.glb\n[mesh m]\nfrom = p\ntake = islands(above 1)\nhost = dyn_dest_ltb\ntexture = own/wood.png\n",
                        "your own mesh is taken whole"),
          "take = all only");
    CHECK(own_build_err(disc, mod, "[source p]\nfile = own/crate.glb\n[mesh m]\nfrom = p\ntake = all\nhost = dyn_dest_ltb\ntexture = own/nope.png\n",
                        "own/nope.png: not in the mod's folder"),
          "a missing texture");
    CHECK(own_build_err(disc, mod, "[sound s]\nfile = own/tone.ogg\nsynth = tone 440\n", "not both"), "file and synth");
    CHECK(own_build_err(disc, mod, "[sound s]\nsynth = tone 5\n", "synth = sweep F0 F1"), "synth range");
    CHECK(own_build_err(disc, mod, "[sound s]\nfile = own/wood.png\n", "file = own/NAME.wav or own/NAME.ogg"), "a sound's extension");
    CHECK(own_build_err(disc, mod, "[source p]\nfile = own/crate.glb\n[collision c]\nfrom = p\nlevel = hideout\nat = 0 0 0\n", "level = levelNN"), "collision level");
    CHECK(own_build_err(disc, mod, "[source a]\nchar = char_a\n[collision c]\nfrom = a\nlevel = level95\nat = 0 0 0\n", "from = your own mesh"), "collision of the game's mesh");
    /* the cache key follows own/: a changed own file builds again */
    const char *small = "format = 1\n[sound s]\nfile = own/click.wav\n";
    snprintf(path, sizeof path, "%s/assets.ini", mod);
    write_file(path, small, strlen(small));
    char hdd[800];
    snprintf(hdd, sizeof hdd, "%s/hdd", mod);
    mkdir(hdd, 0755);
    MaResult res;
    CHECK(modassets_prepare(mod, "own", "1", disc, hdd, 0, &res, err, sizeof err) == 1 && res.built && res.outputs == 1, "own mod built");
    CHECK(modassets_current(mod, "own", "1", disc, hdd) == 1, "own mod current");
    snprintf(path, sizeof path, "%s/click.wav", own);
    ma_wav_write(path, pcm, 1, 11025, 99);
    CHECK(modassets_current(mod, "own", "1", disc, hdd) == 0, "an own file changed: not current");
    CHECK(modassets_prepare(mod, "own", "1", disc, hdd, 0, &res, err, sizeof err) == 1 && res.built, "an own file changed: built again");
    CHECK(modassets_prepare(mod, "own", "1", disc, hdd, 0, &res, err, sizeof err) == 1 && !res.built && res.outputs == 1, "then a cache hit");
    free(b.b), free(png);
}

int main(int argc, char **argv) {
    const char *tmp = argc > 1 ? argv[1] : "/tmp";
    char disc[600];
    snprintf(disc, sizeof disc, "%s/ma_disc", tmp);
    make_disc(disc);
    test_pyrand();
    test_parser();
    test_key();
    test_png(tmp);
    test_build(tmp, disc);
    test_trophy(tmp, disc);
    test_fights(tmp, disc);
    test_cache(tmp, disc);
    test_layers(tmp);
    test_glb();
    test_own_media(tmp);
    test_own_build(tmp, disc);
    if (failures) {
        printf("modassets_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("modassets_test: all passed\n");
    return 0;
}
