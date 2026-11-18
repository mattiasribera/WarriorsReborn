/* Texture layers (src/texlayer.h): file names, loading a folder, layers on and off, combinations
 * per texture, the blend and its stacking. */
#include "../src/texlayer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void touch(const char *dir, const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) fclose(f);
}

/* Fake reader: the layer's colour is chosen by the label in its path. */
static uint32_t *fake_load(const char *path, uint32_t *w, uint32_t *h) {
    if (strstr(path, "missing")) return NULL;
    uint32_t *px = malloc(4 * sizeof *px);
    uint32_t c = strstr(path, "red") ? 0xffff0000u      /* opaque red */
                 : strstr(path, "half") ? 0x800000ffu    /* blue at alpha 128 */
                                         : 0x0000ff00u;   /* green, alpha 0 */
    for (int i = 0; i < 4; i++) px[i] = c;
    px[3] = 0;   /* the last texel transparent in every layer */
    *w = *h = 2;
    return px;
}

int main(void) {
    uint64_t tex;
    char label[41];
    CHECK(!texlayer_parse_name("0123456789abcdef@cl-2.png", &tex, label) && tex == 0x0123456789abcdefull && !strcmp(label, "cl-2"),
          "parse <tex>@<label>.png");
    CHECK(texlayer_parse_name("0123456789ABCDEF@cl.png", &tex, label), "upper-case hex refused");
    CHECK(texlayer_parse_name("0123456789abcdef-cl.png", &tex, label), "no @ refused");
    CHECK(texlayer_parse_name("0123456789abcdef@.png", &tex, label), "empty label refused");
    CHECK(texlayer_parse_name("0123456789abcdef@Cl.png", &tex, label), "label with upper case refused");
    CHECK(texlayer_parse_name("0123456789abcdef@cl.wmesh", &tex, label), "other extension refused");
    CHECK(texlayer_parse_name("0123456789abcdef.png", &tex, label), "a mesh override's texture is not a layer");

    char dir[] = "/tmp/texlayer_testXXXXXX";
    if (!mkdtemp(dir)) return 1;
    touch(dir, "00000000000000aa@1red.png");
    touch(dir, "00000000000000aa@2half.png");
    touch(dir, "00000000000000bb@1red.png");
    touch(dir, "00000000000000cc@missing.png");
    touch(dir, "00000000000000aa.wmesh");   /* a mesh override: not a layer */
    touch(dir, "notes.txt");
    CHECK(texlayer_add_dir(dir, "m") == 4 && texlayer_count() == 4, "4 layer files loaded");
    CHECK(texlayer_add_dir(dir, "m") == 0, "loading the same folder again adds nothing");
    CHECK(texlayer_add_dir("/nonexistent/texlayer", "m") == -1, "a missing folder is -1");
    CHECK(texlayer_on("m/1red") == 0 && texlayer_on("m/nope") == -1 && texlayer_active() == 0, "layers start off");
    CHECK(texlayer_select(0xaa) == 0, "nothing on: no combination");
    CHECK(texlayer_set("m/nope", 1) == -1, "unknown layer is -1");
    CHECK(texlayer_set("m/1red", 1) == 2 && texlayer_active() == 1, "m/red covers 2 textures");
    CHECK(texlayer_set("m/1red", 1) == 2 && texlayer_active() == 1, "turning a layer on twice counts once");
    uint16_t a = texlayer_select(0xaa), b = texlayer_select(0xbb);
    CHECK(a && b && a != b && texlayer_key(a) != texlayer_key(b), "one combination per texture");
    CHECK(texlayer_select(0xaa) == a, "the same combination again has the same id");
    CHECK(texlayer_select(0xdd) == 0, "a texture without layers draws as it is");
    texlayer_set("m/2half", 1);
    uint16_t ab = texlayer_select(0xaa);
    CHECK(ab && ab != a && texlayer_key(ab) != texlayer_key(a), "a second layer on gives a new combination");
    CHECK(texlayer_select(0xbb) == b, "the other texture keeps its combination");

    /* blend: base grey 0x40 with alpha 0x7f */
    uint32_t px[4];
    for (int i = 0; i < 4; i++) px[i] = 0x7f404040u;
    CHECK(texlayer_compose(a, px, 2, 2, fake_load) == 0 && px[0] == 0x7fff0000u && px[3] == 0x7f404040u,
          "opaque layer replaces the colour, keeps the base alpha; alpha 0 leaves the base (%08x %08x)", px[0], px[3]);
    for (int i = 0; i < 4; i++) px[i] = 0x7f404040u;
    texlayer_compose(ab, px, 2, 2, fake_load);
    CHECK(px[0] == 0x7f7f0080u, "layers stack in file-name order: 1red, then 2half (blue at 128) (%08x)", px[0]);
    uint32_t big[16];
    for (int i = 0; i < 16; i++) big[i] = 0xff000000u;
    texlayer_compose(a, big, 4, 4, fake_load);
    CHECK(big[0] == 0xffff0000u && big[5] == 0xffff0000u && big[15] == 0xff000000u && big[10] == 0xff000000u,
          "a smaller layer is sampled at the nearest texel");
    texlayer_set("m/missing", 1);
    uint16_t c = texlayer_select(0xcc);
    for (int i = 0; i < 4; i++) px[i] = 0x7f404040u;
    CHECK(c && texlayer_compose(c, px, 2, 2, fake_load) == 1 && px[0] == 0x7f404040u, "an unreadable layer is reported and skipped");
    texlayer_set("m/1red", 0);
    texlayer_set("m/2half", 0);
    texlayer_set("m/missing", 0);
    CHECK(texlayer_active() == 0 && texlayer_select(0xaa) == 0, "all off: no combination");

    char p[600];
    const char *names[] = {"00000000000000aa@1red.png", "00000000000000aa@2half.png", "00000000000000bb@1red.png",
                           "00000000000000cc@missing.png", "00000000000000aa.wmesh", "notes.txt"};
    for (int i = 0; i < 6; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        unlink(p);
    }
    rmdir(dir);
    texlayer_close();
    CHECK(texlayer_count() == 0, "closed");
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
