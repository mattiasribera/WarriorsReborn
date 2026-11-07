/* Game-independent --sharp-faces test: option parsing; the rule that picks character skins (and
 * leaves pack / built-in replacements alone); the 2x filter on synthetic images (size, flat and
 * smooth images kept, sharp edges without ringing, DXT1 block seams smoothed, painted edges kept,
 * binary alpha kept binary, mean colour preserved) and its cost on a 256x256 skin. */
#include "../src/sharp_faces.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static uint32_t grey(uint32_t v) { return 0xFF000000u | v << 16 | v << 8 | v; }
static int ch(uint32_t c, int s) { return (int)(c >> s & 0xFF); }

static void test_args(void) {
    SharpFacesConfig c = SHARP_FACES_DEFAULTS;
    CHECK(!c.on, "args: off by default");
    CHECK(sharp_faces_arg(&c, "--sharp-faces", NULL) == 1 && c.on, "args: --sharp-faces turns it on");
    CHECK(sharp_faces_arg(&c, "--no-sharp-faces", NULL) == 1 && !c.on, "args: --no-sharp-faces turns it off");
    CHECK(sharp_faces_arg(&c, "--sky-hd", NULL) == 0, "args: other options are not consumed");
    CHECK(!c.over_pack, "args: over-pack off by default");
    CHECK(sharp_faces_arg(&c, "--sharp-faces-over-pack", NULL) == 1 && c.over_pack && !c.on,
          "args: --sharp-faces-over-pack turns over-pack on (not sharp faces itself)");
    CHECK(sharp_faces_arg(&c, "--no-sharp-faces-over-pack", NULL) == 1 && !c.over_pack,
          "args: --no-sharp-faces-over-pack turns it off again");
}

static void test_applies(void) {
    SharpFacesConfig on = {1, 0}, off = {0, 0};
    const uint32_t D = SHARP_FACES_DXT1;
    const int G = SHARP_FACES_SRC_GUEST, B = SHARP_FACES_SRC_BUILTIN, P = SHARP_FACES_SRC_PACK;
    CHECK(sharp_faces_applies(&on, 1, 0, G, D, 256, 256), "rule: a 256x256 DXT1 character skin");
    CHECK(sharp_faces_applies(&on, 1, 0, G, D, 128, 128), "rule: a 128x128 skin");
    CHECK(sharp_faces_applies(&on, 1, 0, G, D, 128, 256), "rule: non-square power-of-two skins");
    CHECK(!sharp_faces_applies(&off, 1, 0, G, D, 256, 256), "rule: nothing while off");
    CHECK(!sharp_faces_applies(&on, 0, 0, G, D, 256, 256), "rule: not world draws");
    CHECK(!sharp_faces_applies(&on, 1, 1, G, D, 256, 256), "rule: stage 0 only");
    CHECK(!sharp_faces_applies(&on, 1, 0, P, D, 256, 256), "rule: a pack replacement wins by default");
    CHECK(!sharp_faces_applies(&on, 1, 0, B, D, 256, 256), "rule: a built-in replacement wins");
    CHECK(sharp_faces_applies(&on, 1, 0, G, D, 512, 512), "rule: 512 skins (the main cast) too");
    CHECK(!sharp_faces_applies(&on, 1, 0, G, D, 1024, 1024), "rule: larger textures are left as they are");
    CHECK(!sharp_faces_applies(&on, 1, 0, G, D, 32, 64), "rule: tiny textures are left as they are");
    CHECK(!sharp_faces_applies(&on, 1, 0, G, D, 192, 256), "rule: non-power-of-two sides are left out");
    CHECK(!sharp_faces_applies(&on, 1, 0, G, 0x0F, 256, 256), "rule: DXT1 only (not DXT5 or others)");
}

/* --sharp-faces-over-pack: character skins beat the pack; everything else is unchanged. */
static void test_over_pack(void) {
    SharpFacesConfig both = {1, 1}, only_over = {0, 1};
    const uint32_t D = SHARP_FACES_DXT1;
    const int G = SHARP_FACES_SRC_GUEST, B = SHARP_FACES_SRC_BUILTIN, P = SHARP_FACES_SRC_PACK;
    CHECK(sharp_faces_applies(&both, 1, 0, P, D, 256, 256), "over-pack: a pack's character skin is upscaled");
    CHECK(sharp_faces_applies(&both, 1, 0, P, D, 512, 512), "over-pack: 512 pack skins too");
    CHECK(sharp_faces_applies(&both, 1, 0, G, D, 256, 256), "over-pack: guest skins still upscaled");
    CHECK(!sharp_faces_applies(&both, 0, 0, P, D, 256, 256), "over-pack: world draws keep the pack texture");
    CHECK(!sharp_faces_applies(&both, 1, 1, P, D, 256, 256), "over-pack: other stages keep the pack texture");
    CHECK(!sharp_faces_applies(&both, 1, 0, P, 0x0F, 256, 256), "over-pack: non-DXT1 pack textures untouched");
    CHECK(!sharp_faces_applies(&both, 1, 0, P, D, 1024, 1024), "over-pack: out-of-range sizes untouched");
    CHECK(!sharp_faces_applies(&both, 1, 0, B, D, 256, 256), "over-pack: built-in replacements still win");
    CHECK(!sharp_faces_applies(&only_over, 1, 0, P, D, 256, 256), "over-pack: nothing without --sharp-faces");
    CHECK(!sharp_faces_applies(&both, 1, 0, 7, D, 256, 256), "over-pack: unknown sources are left alone");
}

static void test_flat_and_size(void) {
    enum { W = 16, H = 8 };
    uint32_t src[W * H];
    for (int i = 0; i < W * H; i++) src[i] = 0xFF4080C0u;
    uint32_t *o = sharp_faces_upscale(src, W, H);
    int same = o != NULL;
    for (int i = 0; o && i < 4 * W * H; i++) same &= o[i] == 0xFF4080C0u;
    CHECK(same, "filter: a flat image stays flat at 2W x 2H");
    free(o);
}

static void test_gradient(void) {
    enum { W = 32, H = 32 };
    uint32_t src[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) src[y * W + x] = grey((uint32_t)(x * 6 + 20));
    uint32_t *o = sharp_faces_upscale(src, W, H);
    int worst = 0, mono = 1;
    for (int y = 0; o && y < H; y++)
        for (int x = 0; x < W; x++) {
            /* the 2x2 box of the output reproduces the source texel */
            int s = 0;
            for (int k = 0; k < 4; k++) s += ch(o[(2 * y + k / 2) * 2 * W + 2 * x + k % 2], 8);
            int d = abs((s + 2) / 4 - ch(src[y * W + x], 8));
            if (d > worst) worst = d;
        }
    for (int x = 1; o && x < 2 * W; x++) mono &= ch(o[5 * 2 * W + x], 0) >= ch(o[5 * 2 * W + x - 1], 0);
    CHECK(o && worst <= 2, "filter: a smooth ramp is reproduced (box error %d)", worst);
    CHECK(o && mono, "filter: a ramp stays monotonic");
    free(o);
}

static void test_edge(void) {
    enum { W = 16, H = 16 };
    uint32_t src[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) src[y * W + x] = grey(x < 7 ? 30 : 220);
    uint32_t *o = sharp_faces_upscale(src, W, H);
    int lo = 255, hi = 0, steep = 0;
    for (int y = 0; o && y < 2 * H; y++)
        for (int x = 0; x < 2 * W; x++) {
            int v = ch(o[y * 2 * W + x], 16);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    /* edge between source x 6 and 7 = output x 13 | 14: it stays within about two output texels */
    if (o) steep = ch(o[2 * W * 4 + 11], 16) <= 40 && ch(o[2 * W * 4 + 16], 16) >= 210;
    CHECK(o && lo >= 30 && hi <= 220, "filter: no ringing past a hard edge (range %d-%d)", lo, hi);
    CHECK(steep, "filter: a hard (painted) edge stays sharp");
    free(o);
}

static void test_deblock(void) {
    enum { W = 16, H = 16 };
    uint32_t src[W * H];
    /* flat 4x4 blocks that differ by a small step: DXT1 quantisation seams on a smooth cheek */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) src[y * W + x] = grey((uint32_t)(100 + (x / 4) * 6));
    uint32_t *o = sharp_faces_upscale(src, W, H);
    /* across the seam at source x 4 (output x 7 | 8) the largest output step is far below 6 */
    int step = 0;
    for (int x = 1; o && x < 2 * W; x++) {
        int d = abs(ch(o[3 * 2 * W + x], 8) - ch(o[3 * 2 * W + x - 1], 8));
        if (d > step) step = d;
    }
    CHECK(o && step <= 3, "filter: DXT1 block seams are smoothed (largest step %d of 6)", step);
    free(o);
}

static void test_alpha(void) {
    enum { W = 8, H = 8 };
    uint32_t src[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) src[y * W + x] = (x < 4 ? 0x00000000u : 0xFF000000u) | 0x806040u;
    uint32_t *o = sharp_faces_upscale(src, W, H);
    int binary = o != NULL;
    for (int i = 0; o && i < 4 * W * H; i++) binary &= (o[i] >> 24) == 0 || (o[i] >> 24) == 0xFF;
    CHECK(binary, "filter: punch-through alpha stays binary");
    free(o);
}

static void test_cost(void) {
    enum { W = 256, H = 256 };
    uint32_t *src = malloc(W * H * 4);
    uint32_t seed = 12345;
    for (int i = 0; i < W * H; i++) {
        seed = seed * 1103515245u + 12345u;
        src[i] = 0xFF000000u | (seed >> 8 & 0xFFFFFFu);
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t *o = sharp_faces_upscale(src, W, H);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    CHECK(o != NULL, "cost: 256x256 -> 512x512 in %.2f ms on this machine", ms);
    free(o);
    free(src);
}

int main(void) {
    test_args();
    test_applies();
    test_over_pack();
    test_flat_and_size();
    test_gradient();
    test_edge();
    test_deblock();
    test_alpha();
    test_cost();
    printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
    return failures != 0;
}
