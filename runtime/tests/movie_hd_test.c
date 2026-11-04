/* Game-independent --movie-hd test: the _hd file-name rule and Bink header check (movie_hd.c),
 * the 16:9 fit rectangle, the GPU texel coordinates and the bilinear YUV quad (yuv.c). */
#include "../src/movie_hd.h"
#include "../src/yuv.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else printf("ok %d\n", __LINE__); } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

static void name_rule(void) {
    static const struct { const char *in, *out; } ok[] = {
        {"\\??\\D:\\bik\\logo.bik", "\\??\\D:\\bik\\logo_hd.bik"},
        {"\\??\\D:\\bik\\l1_in_w.bik", "\\??\\D:\\bik\\l1_in_hd.bik"},
        {"\\??\\D:\\bik_pal\\l9_out_w_pal.bik", "\\??\\D:\\bik_pal\\l9_out_hd.bik"},
        {"\\??\\D:\\bik_pal\\plogo_pal.bik", "\\??\\D:\\bik_pal\\plogo_hd.bik"},
        {"\\Device\\CdRom0\\BIK\\Trailer.BIK", "\\Device\\CdRom0\\BIK\\Trailer_hd.bik"},
    };
    char out[256];
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        CHECK(movie_hd_name(ok[i].in, out, sizeof out) && !strcmp(out, ok[i].out), "%s -> %s", ok[i].in, out);
    static const char *const no[] = {
        "\\??\\D:\\bik\\logo_hd.bik",       /* already HD */
        "\\??\\D:\\bik\\logo.wad",          /* not a movie */
        "\\??\\D:\\audio\\logo.bik",        /* not the movie folder */
        "\\??\\D:\\xbik\\logo.bik",
        "\\??\\D:\\bik\\_w.bik",            /* no name left */
        "logo.bik",
    };
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) CHECK(!movie_hd_name(no[i], out, sizeof out), "%s redirected", no[i]);
    char small[23];   /* "\\??\\D:\\bik\\logo_hd.bik" is 22 characters */
    CHECK(!movie_hd_name("\\??\\D:\\bik\\logo.bik", small, 22), "no room: redirected");
    CHECK(movie_hd_name("\\??\\D:\\bik\\logo.bik", small, 23) && !strcmp(small, "\\??\\D:\\bik\\logo_hd.bik"), "exact room");
}

static void header(uint32_t w, uint32_t h, uint8_t *b) {
    memset(b, 0, MOVIE_HD_HEADER_BYTES);
    memcpy(b, "BIKi", 4);
    memcpy(b + 20, &w, 4);
    memcpy(b + 24, &h, 4);
}
static void header_check(void) {
    uint8_t b[MOVIE_HD_HEADER_BYTES];
    header(1280, 720, b);
    CHECK(movie_hd_header_ok(b, sizeof b), "1280x720 rejected");
    CHECK(!movie_hd_header_ok(b, sizeof b - 1), "short header accepted");
    header(1920, 1080, b);
    CHECK(movie_hd_header_ok(b, sizeof b), "1920x1080 rejected");
    header(640, 480, b);
    CHECK(!movie_hd_header_ok(b, sizeof b), "640x480 accepted (trailer_hd)");
    header(640, 360, b);
    CHECK(!movie_hd_header_ok(b, sizeof b), "640x360 accepted");
    header(1280, 720, b);
    b[0] = 'X';
    CHECK(!movie_hd_header_ok(b, sizeof b), "bad signature accepted");
}

static void fit(void) {
    float in[4] = {0, 0, 640, 480}, r[4];
    yuv_fit_rect(in, 16.f / 9.f, 1.f, r);   /* 4:3 display: letterbox 640x360 */
    CHECK(NEAR(r[0], 0) && NEAR(r[1], 60) && NEAR(r[2], 640) && NEAR(r[3], 420), "4:3 -> %g,%g-%g,%g", r[0], r[1], r[2], r[3]);
    yuv_fit_rect(in, 16.f / 9.f, 4.f / 3.f, r);   /* --widescreen: anamorphic pixels, exact */
    CHECK(NEAR(r[0], 0) && NEAR(r[1], 0) && NEAR(r[2], 640) && NEAR(r[3], 480), "16:9 -> %g,%g-%g,%g", r[0], r[1], r[2], r[3]);
    float tall[4] = {100, 0, 200, 300};
    yuv_fit_rect(tall, 1.f, 1.f, r);
    CHECK(NEAR(r[0], 100) && NEAR(r[1], 100) && NEAR(r[2], 200) && NEAR(r[3], 200), "tall -> %g,%g-%g,%g", r[0], r[1], r[2], r[3]);
    float wide[4] = {0, 10, 400, 110};
    yuv_fit_rect(wide, 2.f, 1.f, wide);   /* in place, pillarbox */
    CHECK(NEAR(wide[0], 100) && NEAR(wide[1], 10) && NEAR(wide[2], 300) && NEAR(wide[3], 110), "wide -> %g,%g-%g,%g", wide[0],
          wide[1], wide[2], wide[3]);
}

static void gpu_coord(void) {
    /* The title's edges -0.5 and size - 0.5: the first and last texel centres in the renderer's
     * convention (i + 0.5), i.e. 0.5 and size - 0.5 before the 1 / size scale. */
    CHECK(NEAR(yuv_gpu_coord(-0.5f, -0.5f, 1279.5f), 0.5f), "lo");
    CHECK(NEAR(yuv_gpu_coord(1279.5f, -0.5f, 1279.5f), 1279.5f), "hi");
    CHECK(NEAR(yuv_gpu_coord(639.5f, -0.5f, 1279.5f), 640.f), "mid %g", yuv_gpu_coord(639.5f, -0.5f, 1279.5f));
    CHECK(NEAR(yuv_gpu_coord(359.5f, 359.5f, -0.5f), 359.5f), "flipped hi");
    CHECK(NEAR(yuv_gpu_coord(-0.5f, 359.5f, -0.5f), 0.5f), "flipped lo");
}

/* Bilinear quad: a 2:1 downscale averages texel pairs (a box filter), and the half-size chroma
 * plane is interpolated between its texels instead of repeated. Identity coefficients (plane i
 * -> channel i, no offsets) make the output the filtered plane values. */
static void bilinear(void) {
    YuvCoeffs id = {{0, 0, 0}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    uint8_t y[4] = {0, 100, 200, 250}, c[2] = {0, 200};
    YuvPlane pl[3] = {{y, 4, 1, 4, -0.5f, -0.5f, 3.5f, 0.5f}, {c, 2, 1, 2, -0.5f, -0.5f, 1.5f, 0.5f}, {c, 2, 1, 2, -0.5f, -0.5f, 1.5f, 0.5f}};
    uint32_t out[4] = {0};
    yuv_draw_quad_bilinear(out, sizeof out, 2, 1, 0, 0, 2, 1, pl, &id);
    CHECK(out[0] >> 16 == 50 && out[1] >> 16 == 225, "2:1 luma %u %u", out[0] >> 16, out[1] >> 16);
    CHECK((out[0] >> 8 & 0xFF) == 0 && (out[1] >> 8 & 0xFF) == 200, "2:1 chroma %u %u", out[0] >> 8 & 0xFF, out[1] >> 8 & 0xFF);
    /* 4 output pixels from the 2-texel chroma plane: centres at -0.25, 0.25, 0.75, 1.25 texels ->
     * clamped 0, then 1/4, 3/4 of the way, clamped 200. The nearest quad repeats 0, 0, 200, 200. */
    yuv_draw_quad_bilinear(out, sizeof out, 4, 1, 0, 0, 4, 1, pl, &id);
    uint32_t want[4] = {0, 50, 150, 200};
    for (int i = 0; i < 4; i++) CHECK((out[i] >> 8 & 0xFF) == want[i], "chroma %d: %u want %u", i, out[i] >> 8 & 0xFF, want[i]);
    yuv_draw_quad(out, sizeof out, 4, 1, 0, 0, 4, 1, pl, &id);
    CHECK((out[1] >> 8 & 0xFF) == 0 && (out[2] >> 8 & 0xFF) == 200, "nearest chroma %u %u", out[1] >> 8 & 0xFF, out[2] >> 8 & 0xFF);
    /* BT.601 white and black through the filtered path match the 8-bit conversion. */
    YuvCoeffs bt;
    yuv_coeffs_bt601(&bt);
    CHECK(yuv_to_xrgb_f(&bt, 235, 128, 128) == yuv_to_xrgb(&bt, 235, 128, 128), "white");
    CHECK(yuv_to_xrgb_f(&bt, 16, 128, 128) == 0, "black %06x", yuv_to_xrgb_f(&bt, 16, 128, 128));
    /* Clipping: a quad half outside the target writes only the visible pixels. */
    uint32_t clip[2] = {7, 7};
    yuv_draw_quad_bilinear(clip, sizeof clip, 1, 1, -1, 0, 1, 1, pl, &id);
    CHECK(clip[1] == 7 && clip[0] >> 16 == 225, "clip %08x %08x", clip[0], clip[1]);
}

int main(void) {
    name_rule();
    header_check();
    fit();
    gpu_coord();
    bilinear();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}
