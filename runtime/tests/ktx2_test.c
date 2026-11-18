/* KTX2 (Basis Universal) transcoding for texture packs (src/ktx2.cpp + the vendored transcoder):
 * the synthetic fixtures in tests/ktx2 (made by tests/ktx2_fixtures.py from the patterns below)
 * come back as the expected BC format with the full mip chain and texels close to the pattern:
 * ETC1S -> BC1 (opaque) / BC3 (alpha), UASTC and XUBC7 -> BC7; a cutout keeps its alpha-test
 * shape and, down the mip chain, its coverage; a normal map its height in alpha, a 4-channel map
 * all four channels. Refused: no mips,
 * texture arrays, truncated or foreign data. Also: several threads transcode at once, and a pack's
 * <hash>.ktx2 loads end to end through texpack_load_image. Usage: ktx2_test [fixtures dir]. */
#include "../src/ktx2.h"
#include "../src/texpack.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

enum { N = 16, LEVELS = 5 };
static const char *dir = "tests/ktx2";

/* Keep in step with ktx2_fixtures.py texel(). */
static void pattern(const char *kind, int x, int y, int c[4]) {
    if (!strcmp(kind, "rgb") || !strcmp(kind, "cutout")) {
        int b = 128 + 8 * (x - y);
        c[0] = 16 * x + 8, c[1] = 16 * y + 8, c[2] = b < 0 ? 0 : b > 255 ? 255 : b;
        c[3] = !strcmp(kind, "rgb") || (2 * x - 15) * (2 * x - 15) + (2 * y - 15) * (2 * y - 15) < 144 ? 255 : 0;
    } else if (!strcmp(kind, "normal")) {
        c[0] = 128 + 6 * (x - 8), c[1] = 128 + 6 * (8 - y), c[2] = 240, c[3] = 255 - 10 * abs(x - 8);
    } else {   /* shine */
        c[0] = 16 * x + 8, c[1] = 248 - 16 * y, c[2] = x < 8 ? 0 : 255, c[3] = y < 8 ? 64 : 192;
    }
}

static uint8_t *read_file(const char *name, size_t *n) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = b ? (size_t)len : 0;
    return b;
}

/* Channel c of a B8G8R8A8 texel (0 R, 1 G, 2 B, 3 A). */
static int chan(uint32_t px, int c) { return (int)(px >> (c == 0 ? 16 : c == 1 ? 8 : c == 2 ? 0 : 24)) & 255; }

typedef struct {
    double psnr[4];   /* per channel against the pattern */
    int max_err[4];
} Stats;

static Stats compare(const char *kind, const uint32_t *px) {
    Stats s;
    for (int c = 0; c < 4; c++) {
        double se = 0;
        int mx = 0;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int want[4];
                pattern(kind, x, y, want);
                int d = chan(px[y * N + x], c) - want[c];
                se += (double)d * d;
                if (abs(d) > mx) mx = abs(d);
            }
        s.psnr[c] = se ? 10 * log10(255.0 * 255.0 * N * N / se) : 99;
        s.max_err[c] = mx;
    }
    return s;
}

/* Transcodes a fixture; level 0 decoded on the CPU into *px (malloc'd) when the result is good. */
static const char *load(const char *name, TexpackImage *img, uint32_t **px) {
    size_t n;
    uint8_t *b = read_file(name, &n);
    if (!b) return "fixture missing";
    memset(img, 0, sizeof *img);
    const char *why = ktx2_transcode(b, n, img);
    free(b);
    *px = why ? NULL : texpack_image_bgra(img);
    return why;
}

static void test_fixture(const char *name, const char *kind, uint32_t format, double min_psnr, int alpha) {
    TexpackImage img;
    uint32_t *px;
    const char *why = load(name, &img, &px);
    CHECK(!why && img.format == format && img.w == N && img.h == N && img.levels == LEVELS && px,
          "%s: transcodes to format %u (%s; %ux%u, %u levels)", name, img.format, why ? why : "ok", img.w, img.h, img.levels);
    if (why || !px) return;
    Stats s = compare(kind, px);
    const int nc = alpha ? 4 : 3;
    double worst = 99;
    for (int c = 0; c < nc; c++) worst = s.psnr[c] < worst ? s.psnr[c] : worst;
    CHECK(worst >= min_psnr, "%s: level 0 PSNR R %.1f G %.1f B %.1f A %.1f dB (worst used channel >= %.0f)", name, s.psnr[0], s.psnr[1],
          s.psnr[2], s.psnr[3], min_psnr);
    if (!alpha) CHECK(s.max_err[3] == 0, "%s: opaque stays opaque (alpha error %d)", name, s.max_err[3]);
    /* the last level is one block whose single texel is near the pattern's average colour */
    size_t at = 0;
    for (uint32_t l = 0; l + 1 < img.levels; l++) at += texpack_level_bytes(img.format, N >> l, N >> l);
    TexpackImage last = {img.format, 1, 1, 1, (uint8_t *)img.data + at};
    uint32_t *one = texpack_image_bgra(&last);
    double avg[4] = {0};
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int want[4];
            pattern(kind, x, y, want);
            for (int c = 0; c < 4; c++) avg[c] += want[c] / (double)(N * N);
        }
    int far = 0;
    for (int c = 0; c < nc && one; c++)
        if (fabs(chan(one[0], c) - avg[c]) > 40) far = 1;   /* a sanity bound: the converter's Lanczos 2x2 -> 1x1 step rings (99 for 195/62) */
    CHECK(one && !far, "%s: the 1x1 level is close to the average (%d %d %d %d vs %.0f %.0f %.0f %.0f)", name, one ? chan(one[0], 0) : -1,
          one ? chan(one[0], 1) : -1, one ? chan(one[0], 2) : -1, one ? chan(one[0], 3) : -1, avg[0], avg[1], avg[2], avg[3]);
    free(one);
    if (!strcmp(kind, "cutout")) {
        int wrong = 0;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int want[4];
                pattern(kind, x, y, want);
                wrong += (chan(px[y * N + x], 3) >= 128) != (want[3] >= 128);
            }
        CHECK(!wrong, "%s: every texel keeps its side of the alpha test (%d wrong)", name, wrong);
        /* the converter keeps alpha-test coverage down the chain (texture_compress.py keep_coverage),
         * as far as a level's texel count allows: level 1 (8 x 8) here; 4 x 4 cannot hold 0.44 */
        double cov[2] = {0};
        size_t off = 0;
        for (uint32_t l = 0; l < 2; l++) {
            const uint32_t s = N >> l;
            TexpackImage lv = {img.format, s, s, 1, (uint8_t *)img.data + off};
            uint32_t *q = texpack_image_bgra(&lv);
            for (uint32_t i = 0; q && i < s * s; i++) cov[l] += (chan(q[i], 3) >= 128) / (double)(s * s);
            free(q);
            off += texpack_level_bytes(img.format, s, s);
        }
        CHECK(fabs(cov[1] - cov[0]) < 0.1, "%s: alpha-test coverage kept on level 1 (%.2f, %.2f)", name, cov[0], cov[1]);
    }
    if (!strcmp(kind, "normal")) CHECK(s.max_err[3] <= 8, "%s: the height in alpha survives (max error %d)", name, s.max_err[3]);
    if (!strcmp(kind, "shine")) {
        double b0 = 0, b1 = 0, a0 = 0, a1 = 0;   /* region means of the step channels */
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                *(x < 8 ? &b0 : &b1) += chan(px[y * N + x], 2) / 128.0;
                *(y < 8 ? &a0 : &a1) += chan(px[y * N + x], 3) / 128.0;
            }
        CHECK(s.max_err[0] <= 12 && s.max_err[1] <= 12 && b0 < 16 && b1 > 239 && fabs(a0 - 64) < 8 && fabs(a1 - 192) < 8,
              "%s: all four channels independent (R/G max error %d/%d, B %.0f|%.0f, A %.0f|%.0f)", name, s.max_err[0], s.max_err[1], b0, b1,
              a0, a1);
    }
    free(px);
    free(img.data);
}

static void test_refused(void) {
    TexpackImage img;
    uint32_t *px;
    const char *why = load("rgb_nomips.ktx2", &img, &px);
    CHECK(why && strstr(why, "mip chain"), "a file without mips is refused (%s)", why ? why : "accepted");
    why = load("rgb_array.ktx2", &img, &px);
    CHECK(why && strstr(why, "2D"), "a texture array is refused (%s)", why ? why : "accepted");
    size_t n;
    uint8_t *b = read_file("shine_uastc.ktx2", &n);
    int bad = 0;
    for (size_t cut = 0; b && cut < n; cut += n / 16 + 1) {   /* every truncation fails cleanly */
        memset(&img, 0, sizeof img);
        why = ktx2_transcode(b, cut, &img);
        if (!why) { bad++; free(img.data); }
    }
    CHECK(b && !bad, "truncated files are refused (%d accepted)", bad);
    for (size_t i = 0; b && i < n; i += 7) {   /* flipped bytes: refused or transcoded, never a crash */
        b[i] ^= 0x5a;
        memset(&img, 0, sizeof img);
        if (!ktx2_transcode(b, n, &img)) free(img.data);
        b[i] ^= 0x5a;
    }
    CHECK(b != NULL, "files with flipped bytes do not crash the transcoder");
    free(b);
    static const uint8_t junk[64] = "DDS this is not a KTX2 file at all..";
    memset(&img, 0, sizeof img);
    why = ktx2_transcode(junk, sizeof junk, &img);
    CHECK(why && !img.data, "foreign data is refused (%s)", why ? why : "accepted");
}

/* Several threads transcoding at once give the single-threaded bytes. */
typedef struct {
    const uint8_t *buf;
    size_t n, bytes;
    const uint8_t *want;
    int mismatches;
} Job;
static void *worker(void *arg) {
    Job *j = arg;
    for (int i = 0; i < 25; i++) {
        TexpackImage img;
        memset(&img, 0, sizeof img);
        if (ktx2_transcode(j->buf, j->n, &img) || memcmp(img.data, j->want, j->bytes)) j->mismatches++;
        free(img.data);
    }
    return NULL;
}
static void test_threads(void) {
    const char *names[2] = {"rgb_etc1s.ktx2", "shine_uastc.ktx2"};
    Job job[4];
    pthread_t th[4];
    uint8_t *bufs[2] = {0};
    size_t sizes[2] = {0};
    TexpackImage ref[2];
    int ok = 1;
    for (int k = 0; k < 2; k++) {
        bufs[k] = read_file(names[k], &sizes[k]);
        memset(&ref[k], 0, sizeof ref[k]);
        ok &= bufs[k] && !ktx2_transcode(bufs[k], sizes[k], &ref[k]);
    }
    if (ok) {
        for (int t = 0; t < 4; t++) {
            const int k = t & 1;
            size_t bytes = 0;
            for (uint32_t l = 0; l < ref[k].levels; l++) bytes += texpack_level_bytes(ref[k].format, N >> l, N >> l);
            job[t] = (Job){bufs[k], sizes[k], bytes, ref[k].data, 0};
        }
        for (int t = 0; t < 4; t++) pthread_create(&th[t], NULL, worker, &job[t]);
        for (int t = 0; t < 4; t++) pthread_join(th[t], NULL);
    }
    int mism = 0;
    for (int t = 0; ok && t < 4; t++) mism += job[t].mismatches;
    CHECK(ok && !mism, "4 threads x 25 transcodes match the single-threaded result (%d mismatches)", mism);
    for (int k = 0; k < 2; k++) {
        free(bufs[k]);
        if (ok) free(ref[k].data);
    }
}

/* A pack with <hash>.ktx2 and <hash>.png: the .ktx2 is offered and loads through texpack. */
static void test_pack(void) {
    char tmp[] = "/tmp/ktx2_test.XXXXXX", path[600];
    if (!mkdtemp(tmp)) {
        CHECK(0, "temporary pack directory");
        return;
    }
    size_t n;
    uint8_t *b = read_file("cutout_uastc.ktx2", &n);
    snprintf(path, sizeof path, "%s/00000000000000c1.ktx2", tmp);
    FILE *f = b ? fopen(path, "wb") : NULL;
    if (f) { fwrite(b, 1, n, f); fclose(f); }
    free(b);
    uint32_t px[4] = {0xff000000u, 0xff000000u, 0xff000000u, 0xff000000u};
    snprintf(path, sizeof path, "%s/00000000000000c1.png", tmp);
    texpack_png_write(path, 2, 2, px);
    texpack_set_ktx2(ktx2_transcode);
    texpack_set_dds(1);
    CHECK(texpack_open(tmp) == 1, "pack: one hash");
    char *p = texpack_path(0xc1);
    TexpackImage img;
    const char *why = NULL;
    int r = p ? texpack_load_image(p, &img, &why) : -1;
    CHECK(p && strstr(p, ".ktx2") && !r && img.format == TEXPACK_BC7 && img.levels == LEVELS, "pack: the .ktx2 loads as BC7 (%s)",
          why ? why : p ? p : "no path");
    uint32_t count;
    double total, longest;
    texpack_ktx2_stats(&count, &total, &longest);
    CHECK(count == 1 && total > 0, "pack: one transcode counted (%u, %.3f ms)", count, total);
    if (!r) free(img.data);
    free(p);
    texpack_close();
    texpack_set_dds(0);
    snprintf(path, sizeof path, "%s/00000000000000c1.ktx2", tmp);
    remove(path);
    snprintf(path, sizeof path, "%s/00000000000000c1.png", tmp);
    remove(path);
    rmdir(tmp);
}

int main(int argc, char **argv) {
    if (argc > 1) dir = argv[1];
    ktx2_init();
    /* 16 x 16 synthetic gradients with independent R/G/B ramps are hard for 4x4 block codecs (ETC1S
     * shares one hue per half block): these bounds catch wrong formats, channels or levels, not codec
     * quality, which was measured on real textures. */
    test_fixture("rgb_etc1s.ktx2", "rgb", TEXPACK_BC1, 20, 0);
    test_fixture("rgb_uastc.ktx2", "rgb", TEXPACK_BC7, 27, 0);
    test_fixture("cutout_etc1s.ktx2", "cutout", TEXPACK_BC3, 20, 1);
    test_fixture("cutout_uastc.ktx2", "cutout", TEXPACK_BC7, 27, 1);
    test_fixture("cutout_xubc7.ktx2", "cutout", TEXPACK_BC7, 27, 1);
    test_fixture("normal_uastc.ktx2", "normal", TEXPACK_BC7, 38, 1);
    test_fixture("shine_uastc.ktx2", "shine", TEXPACK_BC7, 32, 1);
    test_fixture("shine_xubc7.ktx2", "shine", TEXPACK_BC7, 32, 1);
    test_refused();
    test_threads();
    test_pack();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
