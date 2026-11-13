/* Game-independent --normal-maps test: option parsing (defaults, ranges, other options untouched);
 * companion file names (made from a hash and from a base texture's file name, parsed back, base
 * colour textures and other files refused); the companion index over temporary packs (relief and
 * specular maps per hash, the first pack wins, the .dds wins within a pack only while .dds files
 * are offered, a later pack's .png backs a .dds-only hash, counts, forget, clear). */
#include "../src/normal_maps.h"
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
    test_names();
    test_index(dir);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) printf("note: could not remove %s\n", dir);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
