/* Game-independent --neon-glow test: option parsing; mask file names (made and parsed, the colour
 * replacement names refused); the mask index over temporary packs (first pack wins, the .dds wins
 * within a pack, other files ignored, a mask-only pack, forget, clear); loading a grey PNG and a
 * BC4 .emit.dds into an L8 mip chain (sizes, values, malformed files refused); the BC4 decoder
 * against known blocks; the L8 mip filter; and the flicker (deterministic, within its range,
 * different per sign, gentle frame to frame, none at amplitude 0). */
#include "../src/neon_glow.h"
#include "../src/texpack.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_args(void) {
    NeonGlowConfig c = NEON_GLOW_DEFAULTS;
    CHECK(!c.on && c.strength == NEON_GLOW_STRENGTH_DEFAULT && c.bloom == NEON_GLOW_BLOOM_DEFAULT &&
              c.flicker == NEON_GLOW_FLICKER_DEFAULT, "args: off by default, default strength, bloom and flicker");
    CHECK(neon_glow_arg(&c, "--neon-glow", NULL) == 1 && c.on, "args: --neon-glow turns it on");
    CHECK(neon_glow_arg(&c, "--no-neon-glow", NULL) == 1 && !c.on, "args: --no-neon-glow turns it off");
    CHECK(neon_glow_arg(&c, "--neon-glow-strength", "2.5") == 2 && c.strength == 2.5f, "args: --neon-glow-strength 2.5");
    CHECK(neon_glow_arg(&c, "--neon-glow-strength", "0") == -1 && c.strength == 2.5f, "args: strength 0 refused, unchanged");
    CHECK(neon_glow_arg(&c, "--neon-glow-strength", "3.5") == -1, "args: strength above 3 refused");
    CHECK(neon_glow_arg(&c, "--neon-glow-strength", NULL) == -1, "args: missing strength refused");
    CHECK(neon_glow_arg(&c, "--neon-glow-bloom", "0") == 2 && c.bloom == 0.0f, "args: --neon-glow-bloom 0");
    CHECK(neon_glow_arg(&c, "--neon-glow-bloom", "1.5") == -1, "args: bloom above 1 refused");
    CHECK(neon_glow_arg(&c, "--neon-glow-flicker", "0.1") == 2 && c.flicker == 0.1f, "args: --neon-glow-flicker 0.1");
    CHECK(neon_glow_arg(&c, "--neon-glow-flicker", "0.5") == -1, "args: flicker above 0.3 refused");
    CHECK(neon_glow_arg(&c, "--neon-glow-flicker", "x") == -1, "args: a non-number refused");
    CHECK(neon_glow_arg(&c, "--bloom", NULL) == 0, "args: other options are not consumed");
}

static void test_names(void) {
    char b[64];
    CHECK(neon_glow_mask_name(0x0123456789ABCDEFull, 0, b, sizeof b) == 0 && !strcmp(b, "0123456789abcdef.emit.png"), "name: png %s", b);
    CHECK(neon_glow_mask_name(0xFull, 1, b, sizeof b) == 0 && !strcmp(b, "000000000000000f.emit.dds"), "name: dds %s", b);
    CHECK(neon_glow_mask_name(1, 0, b, 10) == -1, "name: a short buffer is refused");
    uint64_t h = 0;
    int dds = -1;
    CHECK(!neon_glow_parse_name("0123456789abcdef.emit.png", &h, &dds) && h == 0x0123456789ABCDEFull && dds == 0, "parse: png");
    CHECK(!neon_glow_parse_name("0123456789ABCDEF.EMIT.DDS", &h, &dds) && dds == 1, "parse: upper case dds");
    CHECK(neon_glow_parse_name("0123456789abcdef.png", &h, &dds) == -1, "parse: a colour replacement is not a mask");
    CHECK(neon_glow_parse_name("0123456789abcdef.emit.jpg", &h, &dds) == -1, "parse: other extensions refused");
    CHECK(neon_glow_parse_name("0123456789abcdeg.emit.png", &h, &dds) == -1, "parse: non-hex refused");
    CHECK(neon_glow_parse_name("0123456789abcdef.emit.png.bak", &h, &dds) == -1, "parse: trailing text refused");
}

static void touch(const char *dir, const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) fclose(f);
}

static int ends_with(const char *s, const char *e) {
    size_t a = s ? strlen(s) : 0, b = strlen(e);
    return s && a >= b && !strcmp(s + a - b, e);
}

static void test_index(const char *tmp) {
    char a[400], b[400];
    snprintf(a, sizeof a, "%s/a", tmp);
    snprintf(b, sizeof b, "%s/b", tmp);
    mkdir(a, 0755);
    mkdir(b, 0755);
    touch(a, "00000000000000aa.emit.png");
    touch(a, "00000000000000aa.emit.dds");   /* same pack: the dds wins */
    touch(a, "00000000000000bb.emit.png");
    touch(a, "00000000000000bb.png");        /* a colour replacement: ignored */
    touch(a, "readme.txt");
    touch(b, "00000000000000bb.emit.dds");   /* later pack: the first pack's png stays */
    touch(b, "00000000000000cc.emit.png");   /* only here */
    neon_glow_index_clear();
    CHECK(neon_glow_index_add(a) == 2 && neon_glow_index_count() == 2, "index: two masks in pack a");
    CHECK(neon_glow_index_add(b) == 1 && neon_glow_index_count() == 3, "index: pack b adds one new hash");
    char *p = neon_glow_mask_path(0xAA);
    CHECK(ends_with(p, "/a/00000000000000aa.emit.dds"), "index: the dds wins within a pack (%s)", p ? p : "-");
    free(p);
    p = neon_glow_mask_path(0xBB);
    CHECK(ends_with(p, "/a/00000000000000bb.emit.png"), "index: the first pack wins (%s)", p ? p : "-");
    free(p);
    p = neon_glow_mask_path(0xCC);
    CHECK(ends_with(p, "/b/00000000000000cc.emit.png"), "index: a later pack's own mask (%s)", p ? p : "-");
    free(p);
    CHECK(!neon_glow_mask_path(0xDD) && !neon_glow_has(0xDD) && neon_glow_has(0xCC), "index: an unknown hash has none");
    neon_glow_index_forget(0xCC);
    CHECK(!neon_glow_has(0xCC) && neon_glow_index_count() == 2, "index: forget drops a hash");
    char bad[400];
    snprintf(bad, sizeof bad, "%s/missing", tmp);
    CHECK(neon_glow_index_add(bad) == -1 && neon_glow_index_count() == 2, "index: an unreadable folder changes nothing");
    neon_glow_index_clear();
    CHECK(!neon_glow_index_count() && !neon_glow_has(0xAA), "index: clear empties it");
    /* a mask-only pack: indexed by itself, and texpack sees nothing in it */
    CHECK(neon_glow_index_add(b) == 2, "index: a mask-only pack");
    CHECK(texpack_open(b) == 0, "index: texpack ignores the mask files");
    texpack_close();
    neon_glow_index_clear();
}

static void test_bc4(void) {
    /* 8-value block: r0 200 > r1 100; texel i gets index i % 8 */
    uint8_t blk[8] = {200, 100};
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) bits |= (uint64_t)(i % 8) << (3 * i);
    for (int i = 0; i < 6; i++) blk[2 + i] = (uint8_t)(bits >> (8 * i));
    uint8_t out[16];
    neon_glow_bc4_decode(blk, 4, 4, out);
    const int want8[8] = {200, 100, (6 * 200 + 1 * 100) / 7, (5 * 200 + 2 * 100) / 7, (4 * 200 + 3 * 100) / 7,
                          (3 * 200 + 4 * 100) / 7, (2 * 200 + 5 * 100) / 7, (1 * 200 + 6 * 100) / 7};
    int ok = 1;
    for (int i = 0; i < 16; i++) ok &= out[i] == want8[i % 8];
    CHECK(ok, "bc4: 8-value palette");
    /* 6-value block: r0 10 <= r1 60; indices 6 and 7 are 0 and 255 */
    uint8_t b6[8] = {10, 60};
    bits = 0;
    for (int i = 0; i < 16; i++) bits |= (uint64_t)(i % 8) << (3 * i);
    for (int i = 0; i < 6; i++) b6[2 + i] = (uint8_t)(bits >> (8 * i));
    neon_glow_bc4_decode(b6, 4, 4, out);
    const int want6[8] = {10, 60, (4 * 10 + 1 * 60) / 5, (3 * 10 + 2 * 60) / 5, (2 * 10 + 3 * 60) / 5, (1 * 10 + 4 * 60) / 5, 0, 255};
    ok = 1;
    for (int i = 0; i < 16; i++) ok &= out[i] == want6[i % 8];
    CHECK(ok, "bc4: 6-value palette with 0 and 255");
    /* a 6x5 image: 2x2 blocks, the partial ones cropped */
    uint8_t four[32];
    for (int b = 0; b < 4; b++) {
        memset(four + 8 * b + 2, 0, 6);
        four[8 * b] = four[8 * b + 1] = (uint8_t)(50 * (b + 1));
    }
    uint8_t img[30];
    neon_glow_bc4_decode(four, 6, 5, img);
    CHECK(img[0] == 50 && img[5] == 100 && img[4 * 6] == 150 && img[4 * 6 + 5] == 200, "bc4: blocks placed and cropped");
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* A DX10 BC4 .dds of w x h with `levels` levels of flat blocks (level l value 10 * (l + 1)). */
static size_t make_dds(uint8_t *buf, uint32_t w, uint32_t h, uint32_t levels, uint32_t dxgi) {
    memset(buf, 0, 148);
    memcpy(buf, "DDS ", 4);
    put32(buf + 4, 124);
    put32(buf + 8, 0xA1007);
    put32(buf + 12, h);
    put32(buf + 16, w);
    put32(buf + 28, levels);
    put32(buf + 76, 32);
    put32(buf + 80, 4);
    memcpy(buf + 84, "DX10", 4);
    put32(buf + 128, dxgi);
    put32(buf + 132, 3);
    put32(buf + 140, 1);
    size_t off = 148;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, n = ((lw + 3) / 4) * ((lh + 3) / 4);
        for (uint32_t b = 0; b < n; b++, off += 8) {
            memset(buf + off, 0, 8);
            buf[off] = buf[off + 1] = (uint8_t)(10 * (l + 1));
        }
    }
    return off;
}

static int write_file(const char *path, const uint8_t *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(d, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

static void test_load(const char *tmp) {
    static uint8_t buf[4096];
    char p[400];
    uint32_t w, h, levels, ow, oh;
    size_t off;
    const char *why = NULL;
    size_t n = make_dds(buf, 16, 8, 5, 80);
    CHECK(!neon_glow_dds_parse(buf, n, &w, &h, &off) && w == 16 && h == 8 && off == 148, "dds: DX10 BC4_UNORM parsed");
    CHECK(neon_glow_dds_parse(buf, 100, &w, &h, &off) != NULL, "dds: truncated header refused");
    CHECK(neon_glow_dds_parse(buf, 148 + 10, &w, &h, &off) != NULL, "dds: truncated level 0 refused");
    uint8_t other[4096];
    size_t on = make_dds(other, 16, 8, 5, 71);
    CHECK(neon_glow_dds_parse(other, on, &w, &h, &off) != NULL, "dds: BC1 refused (not a mask)");
    /* legacy FourCC ATI1 */
    memcpy(other, buf, n);
    memcpy(other + 84, "ATI1", 4);
    memmove(other + 128, other + 148, n - 148);
    CHECK(!neon_glow_dds_parse(other, n - 20, &w, &h, &off) && off == 128, "dds: legacy ATI1 FourCC parsed");
    snprintf(p, sizeof p, "%s/00000000000000aa.emit.dds", tmp);
    write_file(p, buf, n);
    uint8_t *l8 = neon_glow_load(p, &ow, &oh, &levels, &why);
    CHECK(l8 && ow == 16 && oh == 8 && levels == 5, "load: dds -> %ux%u, %u levels", ow, oh, levels);
    CHECK(l8 && l8[0] == 10 && l8[16 * 8 - 1] == 10 && l8[16 * 8] == 10, "load: level 0 decoded, mips rebuilt from it");
    free(l8);
    /* a grey PNG through texpack's writer (B8G8R8A8 in, the red channel read back) */
    uint32_t px[6 * 4];
    for (int i = 0; i < 24; i++) px[i] = 0xFF000000u | (uint32_t)(i * 10) << 16 | 0x0505u;
    snprintf(p, sizeof p, "%s/00000000000000bb.emit.png", tmp);
    texpack_png_write(p, 6, 4, px);
    l8 = neon_glow_load(p, &ow, &oh, &levels, &why);
    CHECK(l8 && ow == 6 && oh == 4 && levels == 3 && l8[0] == 0 && l8[23] == 230, "load: png -> %ux%u, %u levels, red channel", ow, oh, levels);
    free(l8);
    snprintf(p, sizeof p, "%s/nothing.emit.png", tmp);
    why = NULL;
    CHECK(!neon_glow_load(p, &ow, &oh, &levels, &why) && why, "load: a missing file fails with a reason (%s)", why ? why : "-");
}

static void test_mips(void) {
    uint8_t img[4 * 4] = {0};
    img[0] = img[1] = img[4] = img[5] = 255;
    uint32_t levels = 0;
    uint8_t *c = neon_glow_l8_mips(img, 4, 4, &levels);
    CHECK(c && levels == 3 && c[16] == 255 && c[17] == 0 && c[20] == 64, "mips: 4x4 -> 2x2 -> 1x1, rounded box");
    free(c);
    uint8_t odd[3 * 1] = {30, 60, 90};
    c = neon_glow_l8_mips(odd, 3, 1, &levels);
    CHECK(c && levels == 2 && c[3] == 45, "mips: 3x1 -> 1x1 (odd edge dropped as texpack_mips does: %d)", c ? c[3] : -1);
    free(c);
}

static void test_flicker(void) {
    const uint64_t a = 0x1234567890ABCDEFull, b = 0x0FEDCBA987654321ull;
    float lo = 2, hi = -1, maxstep = 0, diff = 0;
    int same = 1;
    for (uint32_t f = 0; f < 60 * 120; f++) {
        float x = neon_glow_flicker(a, f, 0.05f);
        same &= x == neon_glow_flicker(a, f, 0.05f);
        if (x < lo) lo = x;
        if (x > hi) hi = x;
        if (f) {
            float s = fabsf(x - neon_glow_flicker(a, f - 1, 0.05f));
            if (s > maxstep) maxstep = s;
        }
        diff += fabsf(x - neon_glow_flicker(b, f, 0.05f));
    }
    CHECK(same, "flicker: deterministic");
    CHECK(lo >= 0.95f - 1e-6f && hi <= 1.0f + 1e-6f, "flicker: within 1 - amplitude .. 1 (%.3f .. %.3f)", lo, hi);
    CHECK(hi - lo > 0.02f, "flicker: it does move (%.3f)", hi - lo);
    CHECK(maxstep <= 0.05f + 1e-6f, "flicker: gentle frame to frame (max step %.3f)", maxstep);
    CHECK(diff / (60 * 120) > 0.002f, "flicker: two signs differ (mean %.4f)", diff / (60 * 120));
    CHECK(neon_glow_flicker(a, 1234, 0.0f) == 1.0f, "flicker: amplitude 0 is steady");
}

int main(void) {
    char tmp[] = "/tmp/neon_glow_testXXXXXX";
    if (!mkdtemp(tmp)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    test_args();
    test_names();
    test_index(tmp);
    test_bc4();
    test_load(tmp);
    test_mips();
    test_flicker();
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd)) printf("note: could not remove %s\n", tmp);
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
