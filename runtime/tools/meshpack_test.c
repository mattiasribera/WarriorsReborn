/* Game-independent test of the --mesh-pack content hash, .wmesh parsing, directory loading and the
 * draw memo (src/meshpack.c), and the mods' mesh overrides of any vertex layout (src/meshover.c). The hash constants are shared with tests/test_mesh_pack.py, which
 * checks that tools/mesh_pack computes the same values.
 *
 * meshpack_test DIR HASH...: load a real pack directory instead (e.g. one written by
 * tools/mesh_pack) and check that every HASH (16 hex digits) is in it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/meshpack.h"
#include "../src/meshover.h"

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

/* Loads dir like --mesh-pack does and looks up each hash; prints one line per mesh. */
static int check_dir(const char *dir, int n, char **hashes) {
    int loaded = meshpack_open(dir);
    printf("%s: %d meshes loaded\n", dir, loaded);
    for (int i = 0; i < n; i++) {
        uint64_t h = strtoull(hashes[i], NULL, 16);
        const MeshPackMesh *m = meshpack_find(h);
        CHECK(m != NULL, "%s: no valid %016llx.wmesh", dir, (unsigned long long)h);
        if (m) printf("%016llx: %u vertices, %u triangles\n", (unsigned long long)h, m->nv, m->ni / 3);
    }
    meshpack_close();
    if (!fails) printf("ALL OK\n");
    return fails != 0;
}

/* A WMP1 file of any layout: nv vertices of stride0 (+ stride1) bytes, ni indices i % nv. */
static size_t make_any(uint8_t *buf, uint32_t nv, uint32_t ni, uint32_t st0, uint32_t st1) {
    uint32_t hdr[4] = {nv, ni, st0, st1};
    memcpy(buf, "WMP1", 4);
    memcpy(buf + 4, hdr, 16);
    size_t o = 20;
    for (uint32_t i = 0; i < nv * (st0 + st1); i++) buf[o++] = (uint8_t)(i * 7);
    for (uint32_t i = 0; i < ni; i++, o += 2) {
        uint16_t v = (uint16_t)(i % nv);
        memcpy(buf + o, &v, 2);
    }
    return o;
}

static void write_file(const char *dir, const char *name, const void *data, size_t len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

static void remove_file(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    unlink(path);
}

/* src/meshover.c: layouts, file names, texture-limited lookups, the primitive in the hash. */
static void test_meshover(void) {
    static uint8_t buf[1 << 14];
    MeshPackMesh m;
    size_t len = make_any(buf, 3, 3, 36, 0);   /* one stream of 36 bytes (an object's FVF 0x152) */
    CHECK(meshpack_parse(buf, len, 1, &m), "the mesh pack keeps the character layout only");
    CHECK(!meshpack_parse_any(buf, len, 1, &m) && m.stride0 == 36 && m.stride1 == 0 && !m.s1 && m.nv == 3 && m.idx[2] == 2,
          "any layout parses (one stream)");
    meshpack_free(&m);
    len = make_any(buf, 3, 3, 33, 1);   /* odd stream sizes: the index block is realigned */
    CHECK(!meshpack_parse_any(buf, len, 1, &m) && m.stride1 == 1 && m.idx[1] == 1 && m.s1[0] == (uint8_t)(99 * 7),
          "odd stream bytes parse");
    meshpack_free(&m);
    len = make_any(buf, 3, 3, 300, 0);
    CHECK(meshpack_parse_any(buf, len, 1, &m), "stride over 256 is rejected");

    uint64_t mesh, tex;
    int has, hide;
    CHECK(!meshover_parse_name("00000000000000aa.wmesh", &mesh, &tex, &has, &hide) && mesh == 0xaa && !has && !hide, "plain name");
    CHECK(!meshover_parse_name("00000000000000aa-0123456789abcdef.wmesh", &mesh, &tex, &has, &hide) && has && tex == 0x0123456789abcdefull &&
              !hide,
          "texture-limited name");
    CHECK(!meshover_parse_name("00000000000000aa.hide", &mesh, &tex, &has, &hide) && mesh == 0xaa && !has && hide, "hide name");
    CHECK(!meshover_parse_name("00000000000000aa-0123456789abcdef.hide", &mesh, &tex, &has, &hide) && has && hide, "texture-limited hide");
    CHECK(meshover_parse_name("00000000000000aa-0123.wmesh", &mesh, &tex, &has, &hide), "short texture hash");
    CHECK(meshover_parse_name("00000000000000AA.wmesh", &mesh, &tex, &has, &hide), "upper case");
    CHECK(meshover_parse_name("00000000000000aa.png", &mesh, &tex, &has, &hide), "a texture is not a mesh");
    CHECK(meshover_parse_name("00000000000000aa.hidden", &mesh, &tex, &has, &hide), "only .hide hides");

    const uint16_t a[3] = {0, 1, 2};
    uint8_t s0[128];
    for (int i = 0; i < 128; i++) s0[i] = (uint8_t)i;
    uint64_t list = meshover_draw_hash(5, a, 3, 0, 2, s0, 32, NULL, 0), strip = meshover_draw_hash(6, a, 3, 0, 2, s0, 32, NULL, 0);
    CHECK(list == meshpack_hash(a, 3, 0, 2, s0, 32, NULL, 0) && strip != list, "a strip hashes apart from a list");

    char dir[] = "/tmp/meshover_testXXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    len = make_any(buf, 3, 3, 36, 0);
    write_file(dir, "00000000000000aa.wmesh", buf, len);
    write_file(dir, "00000000000000aa.png", "png", 3);
    len = make_any(buf, 4, 6, 32, 18);
    write_file(dir, "00000000000000aa-00000000000000cc.wmesh", buf, len);
    write_file(dir, "00000000000000dd.wmesh", "junk", 4);
    CHECK(meshover_count() == 0 && meshover_add_dir(dir, 3, NULL) == 2 && meshover_count() == 2 && meshover_count_mod(3) == 2 &&
              meshover_count_mod(0) == 0,
          "two valid overrides loaded for mod 3");
    int needs = 0;
    CHECK(meshover_has_mesh(0xaa, &needs) && needs && !meshover_has_mesh(0xdd, &needs), "has_mesh");
    const MeshOverride *o = meshover_find(0xaa, 0xcc, 1);
    CHECK(o && o->has_tex && o->mesh.nv == 4 && !o->png, "the texture-limited override wins for its texture");
    o = meshover_find(0xaa, 0xee, 1);
    CHECK(o && !o->has_tex && o->mesh.stride0 == 36 && o->png && strstr(o->png, "00000000000000aa.png"),
          "another texture: the plain override, with its png");
    o = meshover_find(0xaa, 0, 0);
    CHECK(o && !o->has_tex, "unknown texture: the plain override");
    CHECK(meshover_get(meshover_index(o)) == o && !meshover_get(2), "index round trip");
    CHECK(meshover_add_dir("/nonexistent/meshover", 4, "m") == -1, "missing folder");
    CHECK(meshover_first_use(o) && !meshover_first_use(o) && !meshover_first_use(NULL), "first use once");
    uint64_t h;
    meshover_memo_set(77, 7);
    CHECK(meshover_memo_get(77, &h) && h == 7 && !meshover_memo_get(78, &h), "memo");
    meshover_close();
    CHECK(meshover_count() == 0 && !meshover_find(0xaa, 0, 0), "closed");

    /* Groups: a subfolder is the group "<id>/<subfolder>", off until meshover_group_set; .hide. */
    char sub[600];
    snprintf(sub, sizeof sub, "%s/knocked", dir);
    CHECK(mkdir(sub, 0700) == 0, "group folder");
    write_file(dir, "00000000000000bb.hide", "", 0);
    len = make_any(buf, 3, 3, 36, 0);
    write_file(sub, "00000000000000bb.wmesh", buf, len);
    write_file(sub, "00000000000000bb.png", "png", 3);
    write_file(sub, "00000000000000aa-00000000000000cc.hide", "", 0);
    CHECK(meshover_add_dir(dir, 5, NULL) == 3 && meshover_group_set("m/knocked", 1) == -1, "id NULL: no groups loaded");
    meshover_close();
    CHECK(meshover_add_dir(dir, 5, "m") == 5 && meshover_count_mod(5) == 5, "the folder's 3 and the group's 2");
    o = meshover_find(0xbb, 0, 0);
    CHECK(o && o->hide && !o->group && !o->mesh.nv, "group off: the folder's .hide");
    o = meshover_find(0xaa, 0xcc, 1);
    CHECK(o && !o->hide && !o->group && o->mesh.nv == 4, "group off: its texture-limited hide is not used");
    CHECK(meshover_group_on("m/knocked") == 0 && meshover_group_set("m/knocked", 1) == 2 && meshover_group_on("m/knocked") == 1 &&
              meshover_group_on("m/x") == -1 && meshover_group_set("m/x", 1) == -1,
          "group on: 2 overrides; unknown groups");
    o = meshover_find(0xbb, 0, 0);
    CHECK(o && !o->hide && o->group && o->mesh.stride0 == 36 && o->png && strstr(o->png, "knocked/00000000000000bb.png"),
          "group on: its mesh wins over the folder's .hide, with its png");
    o = meshover_find(0xaa, 0xcc, 1);
    CHECK(o && o->hide && o->group, "group on: its texture-limited hide wins");
    o = meshover_find(0xaa, 0xee, 1);
    CHECK(o && !o->has_tex && !o->group, "another texture: still the folder's plain override");
    CHECK(meshover_group_set("m/knocked", 0) == 2 && meshover_find(0xbb, 0, 0)->hide && !meshover_find(0xbb, 0, 0)->group, "group off again");
    CHECK(!meshover_instance_mask(0xaa) && !meshover_marker_mask(0xbb), "no .worn: not an instance group");
    meshover_close();

    /* Instance groups: a "<marker>.worn" file in the group folder. */
    write_file(sub, "00000000000000bb.worn", "", 0);
    write_file(dir, "00000000000000ee.worn", "", 0);   /* not in a group: ignored */
    write_file(sub, "00000000000000bb-00000000000000cc.worn", "", 0);   /* a marker has no texture: ignored */
    CHECK(meshover_add_dir(dir, 5, "m") == 5, "a .worn file is not an override");
    CHECK(!meshover_instance_mask(0xaa) && !meshover_marker_mask(0xbb), "instance group off: no masks");
    meshover_group_set("m/knocked", 1);
    uint32_t bit = meshover_marker_mask(0xbb);
    CHECK(bit && !(bit & (bit - 1)) && !meshover_marker_mask(0xaa) && !meshover_marker_mask(0xee), "the marker of one group");
    CHECK(meshover_instance_mask(0xaa) == bit && meshover_instance_mask(0xbb) == bit && !meshover_instance_mask(0xdd),
          "instance mask: the meshes the group overrides");
    o = meshover_find_inst(0xbb, 0, 0, bit);
    CHECK(o && o->hide && !o->group, "marker on a head (group off for this draw): the folder's .hide");
    o = meshover_find_inst(0xbb, 0, 0, 0);
    CHECK(o && !o->hide && o->group && o->mesh.stride0 == 36, "marker off a head: the group's mesh");
    o = meshover_find_inst(0xaa, 0xcc, 1, bit);
    CHECK(o && !o->hide && !o->group && o->mesh.nv == 4, "character wearing the marker: the folder's own override");
    o = meshover_find_inst(0xaa, 0xcc, 1, 0);
    CHECK(o && o->hide && o->group, "character without the marker: the group's override");
    CHECK(meshover_find(0xaa, 0xcc, 1) == o, "meshover_find: every enabled group on");

    /* Points (ModMeshPoint): per group, by key; set again moves, NULL removes; cleared at once. */
    const int gi = __builtin_ctz(bit);
    const float p1[3] = {1, 2, 3}, p2[3] = {4, 5, 6};
    CHECK(!meshover_points_any() && meshover_point_count(gi) == 0, "no points at load");
    CHECK(meshover_point_set("m/knocked", "17", p1) == 1 && meshover_point_set("m/knocked", "18", p2) == 2 && meshover_points_any(),
          "two keys: two points");
    CHECK(meshover_point_set("m/knocked", "17", p2) == 2 && meshover_point(gi, 0)[0] == 4 && meshover_point(gi, 1)[2] == 6,
          "the same key moves its point");
    CHECK(meshover_point_set("m/knocked", "17", NULL) == 1 && meshover_point(gi, 0)[1] == 5 && !meshover_point(gi, 1),
          "NULL removes the key's point");
    CHECK(meshover_point_set("m/knocked", "99", NULL) == 1, "removing an unknown key changes nothing");
    CHECK(meshover_point_set("m/x", "1", p1) == -1 && meshover_point_set(NULL, "1", p1) == -1 && meshover_point_set("m/knocked", NULL, p1) == -1,
          "unknown group, no group or no key: -1");
    int full = 0;
    for (int i = 0; i < MESHOVER_POINTS + 1; i++) {
        char key[16];
        snprintf(key, sizeof key, "k%d", i);
        full = meshover_point_set("m/knocked", key, p1);
    }
    CHECK(full == -2 && meshover_point_count(gi) == MESHOVER_POINTS, "a group holds at most %d points", MESHOVER_POINTS);
    meshover_points_clear();
    CHECK(!meshover_points_any() && meshover_point_count(gi) == 0 && !meshover_point(gi, 0), "cleared");
    CHECK(!meshover_placed_mask(0xbb), "no placed file: not a placed group");
    meshover_close();

    /* Placed groups: a file "placed" in the group folder (not outside one). */
    write_file(sub, "placed", "", 0);
    write_file(dir, "placed", "", 0);
    CHECK(meshover_add_dir(dir, 5, "m") == 5, "a placed file is not an override");
    CHECK(!meshover_placed_mask(0xbb), "placed group off: no mask");
    meshover_group_set("m/knocked", 1);
    bit = meshover_placed_mask(0xbb);
    CHECK(bit && bit == meshover_placed_mask(0xaa) && !meshover_placed_mask(0xdd), "placed mask: the meshes the group overrides");
    remove_file(sub, "placed");
    remove_file(dir, "placed");
    meshover_close();
    remove_file(sub, "00000000000000bb.worn");
    remove_file(sub, "00000000000000bb-00000000000000cc.worn");
    remove_file(dir, "00000000000000ee.worn");
    CHECK(meshover_group_on("m/knocked") == -1, "close drops the groups");
    remove_file(sub, "00000000000000bb.wmesh");
    remove_file(sub, "00000000000000bb.png");
    remove_file(sub, "00000000000000aa-00000000000000cc.hide");
    rmdir(sub);
    remove_file(dir, "00000000000000bb.hide");
    remove_file(dir, "00000000000000aa.wmesh");
    remove_file(dir, "00000000000000aa.png");
    remove_file(dir, "00000000000000aa-00000000000000cc.wmesh");
    remove_file(dir, "00000000000000dd.wmesh");
    rmdir(dir);
}

int main(int argc, char **argv) {
    if (argc > 1) return check_dir(argv[1], argc - 2, argv + 2);
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
    test_meshover();
    if (!fails) printf("ALL OK\n");
    return fails != 0;
}
