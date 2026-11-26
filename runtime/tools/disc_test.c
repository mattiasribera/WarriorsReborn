/* Game-independent test of the disc reader (src/disc.c): name hashes, XBoxWad.idx lookups on a
 * synthetic disc folder (two WADs), the container walk (packs, bare items, malformed packs), the
 * header-only scan of a whole disc, the skin binding table and synthetic character and texture
 * chunks. No game data. Usage: disc_test [tmpdir] */
#include "../src/disc.h"
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

typedef struct {
    uint8_t b[1 << 16];
    size_t n;
} Buf;

static void put32(Buf *b, uint32_t v) {
    for (int i = 0; i < 4; i++) b->b[b->n++] = (uint8_t)(v >> (8 * i));
}
static void put(Buf *b, const void *p, size_t n) {
    memcpy(b->b + b->n, p, n);
    b->n += n;
}
static void pad(Buf *b, size_t to, uint8_t v) {
    while (b->n < to) b->b[b->n++] = v;
}
static void chunk(Buf *b, uint32_t cls, uint32_t hash, const char *payload) {
    put32(b, cls), put32(b, (uint32_t)strlen(payload)), put32(b, 0), put32(b, hash);
    put(b, payload, strlen(payload));
}
/* An item of two chunks "abc" (class 1) and "defgh" (class 2). */
static void item(Buf *b, uint32_t hash) {
    put32(b, 2), put32(b, 8), put32(b, 0), put32(b, hash);
    chunk(b, 1, hash + 1, "abc");
    chunk(b, 2, hash + 2, "defgh");
}

static void write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(p, 1, n, f);
    fclose(f);
}

static void test_hashes(void) {
    CHECK(disc_crc32(0, "123456789", 9) == 0xCBF43926u, "zlib CRC-32 check value");
    CHECK(disc_crc32(disc_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u, "CRC-32 continues");
    CHECK(disc_name_hash("paks\\global.pak") == 0x4cfc9c5cu, "global.pak name hash");
    CHECK(disc_name_hash("PAKS/Global.pak") == 0x4cfc9c5cu, "name hash: case and slash folded");
    CHECK(disc_model_hash("abc") == disc_crc32(0, "abc", 3), "model hash = CRC-32 of the name");
}

static void test_walk(void) {
    static Buf b;
    DiscChunk c[16];
    b.n = 0;   /* a pack of two items */
    put32(&b, 2), put32(&b, 0), put32(&b, 0), put32(&b, DISC_PACK_MAGIC);
    item(&b, 0x100);
    item(&b, 0x200);
    int n = disc_walk_buffer(b.b, b.n, 0, c, 16);
    CHECK(n == 4, "pack: 4 chunks");
    CHECK(c[0].cls == 1 && c[0].hash == 0x101 && c[0].item == 0x100 && c[0].off == 48 && c[0].size == 3 && !c[0].bare, "pack chunk 0");
    CHECK(c[3].cls == 2 && c[3].hash == 0x202 && c[3].item == 0x200 && !memcmp(b.b + c[3].off, "defgh", 5), "pack chunk 3");
    b.n = 0;   /* a pack that claims a third item: not a pack, then a bare item at 0 (the header as an item) */
    put32(&b, 3), put32(&b, 0), put32(&b, 0), put32(&b, DISC_PACK_MAGIC);
    item(&b, 0x100);
    item(&b, 0x200);
    n = disc_walk_buffer(b.b, b.n, 0, c, 16);
    CHECK(n == 0, "malformed pack: its header is no bare item (total 0)");
    b.n = 0;   /* a bare item */
    item(&b, 0x300);
    pad(&b, 40, 0);
    n = disc_walk_buffer(b.b, b.n, 0, c, 16);
    CHECK(n == 2 && c[0].bare && c[1].hash == 0x302 && c[1].item == 0x300, "bare item");
    CHECK(disc_walk_buffer(b.b, 31, 0, c, 16) == 0, "bare: under 32 bytes is nothing");
    b.n = 0;   /* an empty pack is a pack (no bare fallback) */
    put32(&b, 0), put32(&b, 0), put32(&b, 0), put32(&b, DISC_PACK_MAGIC);
    item(&b, 0x400);
    CHECK(disc_walk_buffer(b.b, b.n, 0, c, 16) == 0, "pack of 0 items: no chunks");
    b.n = 0;   /* a chunk with a non-zero third word ends the item */
    put32(&b, 1), put32(&b, 3), put32(&b, 0), put32(&b, 0x500);
    put32(&b, 1), put32(&b, 3), put32(&b, 7), put32(&b, 0x501);
    put(&b, "abc", 3);
    pad(&b, 40, 0);
    CHECK(disc_walk_buffer(b.b, b.n, 0, c, 16) == 0, "bad chunk header: no item");
    b.n = 0;   /* a chunk running past the item's total */
    put32(&b, 1), put32(&b, 2), put32(&b, 0), put32(&b, 0x600);
    chunk(&b, 1, 0x601, "abc");
    pad(&b, 40, 0);
    CHECK(disc_walk_buffer(b.b, b.n, 0, c, 16) == 0, "chunk over the item total: no item");
}

/* A synthetic character payload: 3 vertices, one triangle, 32 inverse bind matrices. */
static void character(Buf *b, int bad) {
    b->n = 0;
    const uint32_t nv = 3, d1 = 0x60, d2 = d1 + 64, tail = d2 + 6;
    put32(b, 1), put32(b, 0x76), put32(b, 3), put32(b, tail);
    pad(b, 0x80, 0);
    for (uint32_t i = 0; i < nv * DISC_STRIDE0; i++) b->b[b->n++] = (uint8_t)i;
    pad(b, 0x80 + d1, 0xEE);
    for (uint32_t i = 0; i < nv * DISC_STRIDE1; i++) b->b[b->n++] = (uint8_t)(0x40 + i);
    pad(b, 0x80 + d2, 0xEE);
    const uint16_t idx[3] = {0, 1, (uint16_t)(bad == 1 ? 3 : 2)};
    put(b, idx, 6);
    put32(b, 0x00800001), put32(b, 0), put32(b, 0);
    put32(b, 0x00800001), put32(b, d1), put32(b, 0);
    put32(b, 0x00010001), put32(b, d2), put32(b, 0);
    pad(b, b->n + 5, 0xEE);
    put(b, bad == 2 ? "ctex" : "cter", 4);
    for (int k = 0; k < DISC_PALETTE; k++)
        for (int e = 0; e < 16; e++) {
            float v = (float)(k * 100 + e);
            put(b, &v, 4);
        }
    put32(b, 1), put32(b, 0), put32(b, 12), put32(b, 24);
}

static void test_char(void) {
    static Buf b;
    DiscChar c;
    char err[128];
    character(&b, 0);
    CHECK(disc_is_char(b.b, b.n), "is a character");
    CHECK(disc_char_parse(b.b, b.n, &c, err, sizeof err) == 0, "character parses");
    CHECK(c.nv == 3 && c.ni == 3, "3 vertices, 3 indices");
    CHECK(c.s0 == b.b + 0x80 && c.s0[5] == 5 && c.s1[0] == 0x40 && c.idx[4] == 2, "stream pointers");
    CHECK(c.inverse_bind[4][3] == 403.0f && c.inverse_bind[31][15] == 3115.0f, "inverse binds by slot");
    character(&b, 1);
    CHECK(disc_char_parse(b.b, b.n, &c, err, sizeof err) == -1 && strstr(err, "index out of range"), "index past the vertices");
    character(&b, 2);
    CHECK(disc_char_parse(b.b, b.n, &c, err, sizeof err) == -1 && strstr(err, "cter"), "no matrix block");
    character(&b, 0);
    b.b[0] = 2;
    CHECK(!disc_is_char(b.b, b.n) && disc_char_parse(b.b, b.n, &c, err, sizeof err) == -1, "a prop is not a character");
}

static void test_skins(void) {
    static Buf b;
    DiscSkin r[8], s[8];
    b.n = 0;
    put32(&b, 4), put32(&b, 0), put32(&b, 0), put32(&b, 0);
    const uint32_t recs[4][4] = {{0x11, 1, 0xA0, 0x501}, {0x22, 1, 0xA0, 0x502}, {0x33, 2, 0xB0, 0x503}, {0x44, 1, 0xA0, 0x504}};
    for (int i = 0; i < 4; i++) {
        put(&b, recs[i], 16);
        put32(&b, 0), put32(&b, 9), put32(&b, 9), put32(&b, 0);
    }
    CHECK(disc_skins_parse(b.b, b.n, r, 8) == 4 && r[2].tex == 0x503 && r[2].model == 0xB0, "skin table");
    int n = disc_skins_for(r, 4, 0x22, 0xA0, s, 8);
    CHECK(n == 3 && s[0].tex == 0x502 && s[1].tex == 0x501 && s[2].tex == 0x504, "exact record first, then the model's");
    CHECK(disc_skins_for(r, 4, 0x99, 0xB0, s, 8) == 1 && s[0].tex == 0x503, "by model item only");
    b.b[16 + 20] = 8;   /* the two size words of record 0 differ */
    CHECK(disc_skins_parse(b.b, b.n, r, 8) == -1, "size words must match");
    put32(&b, 0), put32(&b, 0), put32(&b, 0), put32(&b, 0);
    CHECK(disc_skins_parse(b.b, b.n, r, 8) == -1, "16 bytes of slack: not the table");
}

static void test_tex(void) {
    static Buf b;
    b.n = 0;
    put32(&b, 0), put32(&b, 0), put32(&b, 0), put32(&b, 0x10);
    pad(&b, 0x90, 0);
    put32(&b, 0x00040001), put32(&b, 0x40), put32(&b, 0), put32(&b, 0x12345678), put32(&b, 0);
    uint32_t f, s, o;
    CHECK(disc_tex_parse(b.b, b.n, &f, &s, &o) == 0 && f == 0x12345678 && s == 0 && o == 0xC0, "texture resource");
    b.b[0x92] = 0x05;   /* Common type bits: not a texture */
    CHECK(disc_tex_parse(b.b, b.n, &f, &s, &o) == -1, "not a texture resource");
}

static void test_disc(const char *tmp) {
    char dir[512], path[600];
    snprintf(dir, sizeof dir, "%s/disc_test_disc", tmp);
    mkdir(dir, 0755);
    static Buf w0, w1, idx;
    /* wad A: a pack at 0, an empty file at 2048; wad B: a bare skin table item at 0 */
    w0.n = 0;
    put32(&w0, 1), put32(&w0, 0), put32(&w0, 0), put32(&w0, DISC_PACK_MAGIC);
    item(&w0, 0x700);
    const size_t pack_len = w0.n;
    pad(&w0, 4096, 0);
    w1.n = 0;
    put32(&w1, 1), put32(&w1, 48), put32(&w1, 0), put32(&w1, 0x800);
    put32(&w1, DISC_CLASS_SKINS), put32(&w1, 48), put32(&w1, 0), put32(&w1, 0x801);
    put32(&w1, 1), put32(&w1, 0), put32(&w1, 0), put32(&w1, 0);
    put32(&w1, 0x11), put32(&w1, 1), put32(&w1, 0xA0), put32(&w1, 0x501);
    put32(&w1, 0), put32(&w1, 9), put32(&w1, 9), put32(&w1, 0);
    const size_t glr_len = w1.n;
    pad(&w1, 1024, 0);
    snprintf(path, sizeof path, "%s/A.wad", dir), write_file(path, w0.b, w0.n);
    snprintf(path, sizeof path, "%s/B.wad", dir), write_file(path, w1.b, w1.n);
    idx.n = 0;
    put32(&idx, 2), put32(&idx, 4), put32(&idx, 0), put32(&idx, 20), put32(&idx, 20 + 72);
    char name[36] = "A.wad";
    put(&idx, name, 32), put32(&idx, 0);
    memset(name, 0, sizeof name), strcpy(name, "B.wad");
    put(&idx, name, 32), put32(&idx, 0);
    put32(&idx, 0), put32(&idx, (uint32_t)pack_len), put32(&idx, 0), put32(&idx, disc_name_hash("x.pak"));
    put32(&idx, 2), put32(&idx, 0), put32(&idx, 0), put32(&idx, disc_name_hash("empty.bin"));
    put32(&idx, 1u << 24), put32(&idx, (uint32_t)glr_len), put32(&idx, 0), put32(&idx, disc_name_hash("warriors.glr"));
    put32(&idx, 5u << 24), put32(&idx, 16), put32(&idx, 0), put32(&idx, disc_name_hash("badwad.bin"));
    snprintf(path, sizeof path, "%s/XBoxWad.idx", dir), write_file(path, idx.b, idx.n);

    size_t n = 0;
    uint8_t *p = disc_read_hash(dir, disc_name_hash("x.pak"), 1 << 20, &n);
    CHECK(p && n == pack_len && !memcmp(p, w0.b, n), "x.pak read through the index");
    free(p);
    CHECK(!disc_read_hash(dir, disc_name_hash("x.pak"), 16, &n), "over the size limit: NULL");
    p = disc_read_hash(dir, disc_name_hash("empty.bin"), 16, &n);
    CHECK(p && n == 0, "an empty file is an empty buffer");
    free(p);
    p = disc_read_hash(dir, disc_name_hash("warriors.glr"), 1 << 20, &n);
    CHECK(p && n == glr_len && p[16] == DISC_CLASS_SKINS, "from the second WAD (1 KiB units)");
    free(p);
    CHECK(!disc_read_hash(dir, disc_name_hash("badwad.bin"), 1 << 20, &n), "a wad index past the table: NULL");
    CHECK(!disc_read_hash(dir, 0x12345678u, 1 << 20, &n), "no such file");
    CHECK(!disc_read_hash("/nonexistent-disc", disc_name_hash("x.pak"), 1 << 20, &n), "no disc folder");

    Disc d;
    CHECK(disc_open(&d, dir) == 0 && d.nwads == 2 && d.nfiles == 4, "disc_open");
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < idx.n; i++) h = (h ^ idx.b[i]) * 0x100000001b3ull;
    CHECK(d.fingerprint == h, "fingerprint = FNV-1a 64 of XBoxWad.idx");
    CHECK(disc_scan(&d) == 3, "scan: the pack's 2 chunks and the table's");
    CHECK(d.nchunks == 3 && d.chunks[0].file == 0 && d.chunks[1].hash == 0x702 && d.chunks[2].file == 2 && d.chunks[2].bare,
          "scan: chunks in file table order");
    CHECK(d.ntables == 1 && d.tables[0].hash == 0x801 && d.tables[0].file == 2, "scan: the skin table");
    uint8_t *pl = disc_chunk_read(&d, &d.chunks[1]);
    CHECK(pl && !memcmp(pl, "defgh", 5), "chunk payload");
    free(pl);
    DiscSkin r[4];
    pl = disc_chunk_read(&d, &d.tables[0]);
    CHECK(pl && disc_skins_parse(pl, d.tables[0].size, r, 4) == 1 && r[0].tex == 0x501, "table payload parses");
    free(pl);
    disc_close(&d);
    /* the index file: written by the first scan, read by the next (same chunks), refused when stale */
    char ixp[700];
    snprintf(ixp, sizeof ixp, "%s/chunks.idx", tmp);
    remove(ixp);
    int loaded = -1;
    CHECK(!disc_open(&d, dir) && disc_scan_cached(&d, ixp, &loaded) == 3 && loaded == 0, "index file: the first call scans");
    DiscChunk first[3];
    memcpy(first, d.chunks, sizeof first);
    disc_close(&d);
    FILE *ixf = fopen(ixp, "rb");
    CHECK(ixf != NULL, "index file: written");
    if (ixf) fclose(ixf);
    CHECK(!disc_open(&d, dir) && disc_scan_cached(&d, ixp, &loaded) == 3 && loaded == 1 && !memcmp(d.chunks, first, sizeof first) && d.ntables == 1 &&
              d.tables[0].hash == 0x801,
          "index file: the second call reads the same chunks and tables");
    disc_close(&d);
    CHECK(!disc_open(&d, dir), "reopen");
    d.fingerprint ^= 1;   /* another disc */
    CHECK(disc_scan_cached(&d, ixp, &loaded) == 3 && loaded == 0, "index file of another disc: scanned again");
    disc_close(&d);
    write_file(ixp, "DISCIDX1", 8);
    CHECK(!disc_open(&d, dir) && disc_scan_cached(&d, ixp, &loaded) == 3 && loaded == 0, "a truncated index file: scanned again");
    disc_close(&d);
    CHECK(!disc_open(&d, dir) && disc_scan_cached(&d, ixp, &loaded) == 3 && loaded == 1, "and rewritten");
    disc_close(&d);
    snprintf(path, sizeof path, "%s/XBoxWad.idx", dir), write_file(path, idx.b, 12);
    CHECK(disc_open(&d, dir) == -1, "a short index does not open");
}

/* A static prop: FVF 0x152 with the normal packed (stride 28 = 36 - 8), a triangle strip. */
static void test_prop(void) {
    static Buf b;
    const uint32_t nv = 5, ibd = (nv * 28 + 15) & ~15u, tail = ibd + 5 * 2;
    put32(&b, 2), put32(&b, 0x6D), put32(&b, 2), put32(&b, tail);
    pad(&b, 0x54, 0);
    put32(&b, 0x00800001), put32(&b, 0), put32(&b, 0), put32(&b, 0x00010001), put32(&b, ibd), put32(&b, 0), put32(&b, 0);
    put32(&b, 0x152), put32(&b, 6u | 28u << 8), put32(&b, 3);
    pad(&b, 0x80, 0);
    for (uint32_t i = 0; i < nv * 7; i++) put32(&b, i);
    pad(&b, 0x80 + ibd, 0xEE);
    const uint16_t strip[5] = {0, 1, 2, 2, 3};
    put(&b, strip, sizeof strip);
    DiscProp p;
    char err[128];
    CHECK(!disc_prop_parse(b.b, b.n, &p, err, sizeof err), "packed-normal strip prop parses");
    CHECK(p.nv == 5 && p.nidx == 5 && p.stride == 28 && p.prim == 6 && p.normal < 0 && p.normal_packed == 12 && p.colour == 16 && p.uv0 == 20,
          "layout: NORMPACKED3 at 12, the diffuse and uv shifted by 8");
    b.b[0x80 + ibd - 1] = 0;
    CHECK(disc_prop_parse(b.b, b.n, &p, err, sizeof err) && strstr(err, "0xEE"), "the vertex/index gap must be 0xEE");
    b.b[0x80 + ibd - 1] = 0xEE;
    b.b[0x75] = 32;
    CHECK(disc_prop_parse(b.b, b.n, &p, err, sizeof err) && strstr(err, "stride"), "a stride the FVF cannot have");
    b.b[0] = 1;
    CHECK(disc_prop_parse(b.b, b.n, &p, err, sizeof err) && !disc_is_prop(b.b, b.n), "not a prop head");
}

int main(int argc, char **argv) {
    const char *tmp = argc > 1 ? argv[1] : "/tmp";
    test_hashes();
    test_walk();
    test_char();
    test_skins();
    test_tex();
    test_prop();
    test_disc(tmp);
    if (failures) {
        printf("disc_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("disc_test: all passed\n");
    return 0;
}
