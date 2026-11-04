/* Game-independent test of the --mesh-pack content hash, .wmesh parsing, directory loading and the
 * draw memo (src/meshpack.c). The hash constants are shared with tests/test_mesh_pack.py, which
 * checks that tools/mesh_pack computes the same values. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/meshpack.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static size_t make_wmesh(uint8_t *buf, uint32_t nv, uint32_t ni, uint16_t bad_index) {
    uint32_t hdr[4] = {nv, ni, MESHPACK_STRIDE0, MESHPACK_STRIDE1};
    memcpy(buf, "WMP1", 4);
    memcpy(buf + 4, hdr, 16);
    size_t o = 20;
    for (uint32_t i = 0; i < nv * (MESHPACK_STRIDE0 + MESHPACK_STRIDE1); i++) buf[o++] = (uint8_t)i;
    for (uint32_t i = 0; i < ni; i++, o += 2) {
        uint16_t v = (uint16_t)(i % nv);
        if (i == ni - 1 && bad_index) v = bad_index;
        memcpy(buf + o, &v, 2);
    }
    return o;
}

int main(void) {
    uint8_t s0[128], s1[72];
    for (int i = 0; i < 128; i++) s0[i] = (uint8_t)i;
    for (int i = 0; i < 72; i++) s1[i] = (uint8_t)i;
    const uint16_t a[3] = {0, 1, 2}, b[3] = {1, 2, 3};
    uint64_t ha = meshpack_hash(a, 3, 0, 2, s0, MESHPACK_STRIDE0, s1, MESHPACK_STRIDE1);
    CHECK(ha == 0x89f37b56e5bddf58ull, "hash of the 3-vertex draw: %016llx", (unsigned long long)ha);
    uint64_t hb = meshpack_hash(b, 3, 1, 3, s0, MESHPACK_STRIDE0, s1, MESHPACK_STRIDE1);
    /* The range starts at the lowest index: the bytes of vertices 1..3 (s0 + 32, s1 + 18). */
    CHECK(hb == 0x8c72595e17bcc73full, "hash of the offset draw: %016llx", (unsigned long long)hb);

    static uint8_t buf[1 << 16];
    MeshPackMesh m;
    size_t len = make_wmesh(buf, 4, 6, 0);
    CHECK(!meshpack_parse(buf, len, 7, &m) && m.nv == 4 && m.ni == 6 && m.hash == 7 && m.idx[5] == 1 && m.s1[0] == 128,
          "valid mesh parses");
    free((void *)m.s0);
    CHECK(meshpack_parse(buf, len - 1, 7, &m), "truncated file is rejected");
    len = make_wmesh(buf, 4, 6, 4);
    CHECK(meshpack_parse(buf, len, 7, &m), "index >= vertex count is rejected");
    len = make_wmesh(buf, 4, 5, 0);
    CHECK(meshpack_parse(buf, len, 7, &m), "index count not a multiple of 3 is rejected");
    len = make_wmesh(buf, 4, 6, 0);
    buf[0] = 'X';
    CHECK(meshpack_parse(buf, len, 7, &m), "bad magic is rejected");

    char dir[] = "/tmp/meshpack_testXXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    char path[256];
    len = make_wmesh(buf, 3, 3, 0);
    snprintf(path, sizeof path, "%s/00000000000000aa.wmesh", dir);
    FILE *f = fopen(path, "wb");
    fwrite(buf, 1, len, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/00000000000000bb.wmesh", dir);   /* invalid: skipped */
    f = fopen(path, "wb");
    fwrite("junk", 1, 4, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/readme.txt", dir);   /* not a mesh name: ignored */
    f = fopen(path, "wb");
    fclose(f);
    CHECK(meshpack_open(dir) == 1 && meshpack_count() == 1, "one valid mesh loaded");
    CHECK(meshpack_find(0xaa) && meshpack_find(0xaa)->nv == 3 && !meshpack_find(0xbb), "lookup by hash");
    CHECK(meshpack_open("/nonexistent/meshpack") == -1 && meshpack_count() == 0, "missing directory");
    unlink(path);
    snprintf(path, sizeof path, "%s/00000000000000aa.wmesh", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/00000000000000bb.wmesh", dir);
    unlink(path);
    rmdir(dir);

    uint64_t h;
    CHECK(!meshpack_memo_get(5, &h), "empty memo");
    for (uint64_t k = 1; k <= 20; k++) meshpack_memo_set(k << 8 | 1, k * 100);   /* collide in the low bits */
    meshpack_memo_set(5, 55);
    CHECK(meshpack_memo_get(5, &h) && h == 55, "memo hit");
    meshpack_memo_set(5, 56);
    CHECK(meshpack_memo_get(5, &h) && h == 56, "memo update");
    meshpack_close();
    if (!fails) printf("ALL OK\n");
    return fails != 0;
}
