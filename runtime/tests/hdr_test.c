/* Game-independent HDR10 / saturation test (src/hdr.c): option parsing, the PQ encoding of known
 * values, BT.709 -> BT.2020, saturation (1 = identity, > 1 more chroma, < 1 less), the highlight
 * expansion (identity below the knee, continuous, the peak at white), the HDR pixel (HUD at paper
 * white, light sources expanded towards the peak, never above it) and the surface format choice (HDR10
 * when offered under auto / on, the SDR choice as before otherwise, with the reason). */
#include "../src/hdr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_args(void) {
    HdrConfig c = HDR_DEFAULTS;
    CHECK(c.mode == HDR_OFF && c.peak == 1000 && c.paper == 200 && c.saturation == 1, "defaults: off, 1000 nits peak, 200 paper white, saturation 1");
    CHECK(hdr_arg(&c, "--hdr", "auto") == 2 && c.mode == HDR_AUTO, "--hdr auto");
    CHECK(hdr_arg(&c, "--hdr", "on") == 2 && c.mode == HDR_ON, "--hdr on");
    CHECK(hdr_arg(&c, "--hdr", "maybe") == -1 && c.mode == HDR_ON, "--hdr maybe rejected, unchanged");
    CHECK(hdr_arg(&c, "--hdr", NULL) == -1, "--hdr without a value rejected");
    CHECK(hdr_arg(&c, "--hdr", "off") == 2 && c.mode == HDR_OFF, "--hdr off");
    CHECK(hdr_arg(&c, "--hdr-peak", "600") == 2 && c.peak == 600, "--hdr-peak 600");
    CHECK(hdr_arg(&c, "--hdr-peak", "300") == -1 && c.peak == 600, "--hdr-peak 300 rejected (below 400)");
    CHECK(hdr_arg(&c, "--hdr-paper-white", "250") == 2 && c.paper == 250, "--hdr-paper-white 250");
    CHECK(hdr_arg(&c, "--hdr-paper-white", "600") == -1 && c.paper == 250, "--hdr-paper-white 600 rejected");
    CHECK(hdr_arg(&c, "--saturation", "1.3") == 2 && c.saturation == 1.3f, "--saturation 1.3");
    CHECK(hdr_arg(&c, "--saturation", "2.5") == -1 && hdr_arg(&c, "--saturation", "0.4") == -1 && c.saturation == 1.3f,
          "--saturation outside 0.5-2 rejected");
    CHECK(hdr_arg(&c, "--saturation", "x") == -1 && hdr_arg(&c, "--filmic", NULL) == 0, "a bad value rejected, another option not ours");
    CHECK(!strcmp(hdr_mode_name(HDR_AUTO), "auto") && !strcmp(hdr_mode_name(HDR_ON), "on") && !strcmp(hdr_mode_name(HDR_OFF), "off"), "mode names");
}

static void test_pq(void) {
    /* Reference values of SMPTE ST 2084 (e.g. ITU-R BT.2100 tables): 100 nits 0.5081, 1000 0.7518. */
    const struct { float nits, pq; } k[] = {{0, 0.0000007f}, {100, 0.508078f}, {203, 0.580690f}, {1000, 0.751827f}, {10000, 1.0f}};
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        const float v = hdr_pq_encode(k[i].nits);
        CHECK(fabsf(v - k[i].pq) < 2e-4f, "pq: %.0f nits -> %.6f (%.6f)", k[i].nits, v, k[i].pq);
    }
    int mono = 1;
    float worst = 0;
    for (int n = 1; n <= 10000; n += 7) {
        mono &= hdr_pq_encode((float)n) >= hdr_pq_encode((float)(n - 1)) && hdr_pq_encode((float)n + 10) > hdr_pq_encode((float)n);
        const float back = hdr_pq_decode(hdr_pq_encode((float)n));
        if (fabsf(back - n) / n > worst) worst = fabsf(back - n) / n;
    }
    CHECK(mono && worst < 2e-3f, "pq: increasing, decode(encode(x)) within 0.2%% (%.5f)", worst);
    CHECK(hdr_pq_encode(-5) == hdr_pq_encode(0) && hdr_pq_encode(20000) == 1.0f, "pq: clamped to 0-10000 nits");
}

static void test_gamut(void) {
    float w[3] = {1, 1, 1}, r[3] = {1, 0, 0}, o[3];
    hdr_709_to_2020(w, o);
    CHECK(fabsf(o[0] - 1) < 1e-4f && fabsf(o[1] - 1) < 1e-4f && fabsf(o[2] - 1) < 1e-4f, "gamut: white stays white (%.4f %.4f %.4f)", o[0], o[1], o[2]);
    hdr_709_to_2020(r, o);
    CHECK(fabsf(o[0] - 0.6274f) < 1e-3f && o[1] > 0.06f && o[2] > 0.01f, "gamut: BT.709 red inside BT.2020 (%.4f %.4f %.4f)", o[0], o[1], o[2]);
}

static float chroma(const float c[3]) {
    const float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    return fabsf(c[0] - y) + fabsf(c[1] - y) + fabsf(c[2] - y);
}

static void test_saturation(void) {
    const float c[3] = {0.5f, 0.35f, 0.25f};
    float o[3];
    hdr_saturate(c, 1.0f, o);
    CHECK(!memcmp(c, o, sizeof c), "saturation 1: exactly the input");
    hdr_saturate(c, 1.5f, o);
    CHECK(chroma(o) > chroma(c) * 1.49f && fabsf(hdr_luma(o) - hdr_luma(c)) < 1e-5f, "saturation 1.5: more chroma (%.3f > %.3f), same luma",
          chroma(o), chroma(c));
    hdr_saturate(c, 0.5f, o);
    CHECK(chroma(o) < chroma(c) * 0.51f, "saturation 0.5: less chroma (%.3f)", chroma(o));
    const float g[3] = {0.4f, 0.4f, 0.4f};
    hdr_saturate(g, 2.0f, o);
    CHECK(fabsf(o[0] - 0.4f) < 1e-6f && fabsf(o[2] - 0.4f) < 1e-6f, "saturation: grey stays grey");
    const float b[3] = {0.0f, 0.0f, 0.8f};
    hdr_saturate(b, 2.0f, o);
    CHECK(o[0] >= 0 && o[1] >= 0, "saturation: channels never negative");
    /* The SDR grade: 1 leaves display values as they are (within float rounding), 1.5 adds chroma. */
    const float d[3] = {0.8f, 0.5f, 0.2f};
    float s1[3], s2[3];
    hdr_sdr_grade(1.0f, d, s1);
    hdr_sdr_grade(1.5f, d, s2);
    CHECK(fabsf(s1[0] - 0.8f) < 1e-5f && fabsf(s1[1] - 0.5f) < 1e-5f && fabsf(s1[2] - 0.2f) < 1e-5f && chroma(s2) > chroma(d),
          "sdr grade: 1 keeps the colour, 1.5 adds chroma (%.3f > %.3f)", chroma(s2), chroma(d));
}

static void test_expand(void) {
    CHECK(hdr_expand(0.3f, 5, 1) == 0.3f && hdr_expand(HDR_KNEE, 5, 1) == HDR_KNEE, "expand: identity up to the knee");
    CHECK(fabsf(hdr_expand(1, 5, 1) - 5) < 1e-5f && fabsf(hdr_expand(1, 5, 0.5f) - 3) < 1e-5f, "expand: white to the ratio (weight 1), half way (0.5)");
    CHECK(hdr_expand(0.9f, 1, 1) == 0.9f && hdr_expand(0.9f, 5, 0) == 0.9f, "expand: no headroom or no weight: identity");
    int mono = 1;
    for (int i = 1; i <= 1000; i++) mono &= hdr_expand(i / 1000.0f, 5, 1) >= hdr_expand((i - 1) / 1000.0f, 5, 1);
    const float e = 1e-3f, slope = (hdr_expand(HDR_KNEE + e, 5, 1) - hdr_expand(HDR_KNEE, 5, 1)) / e;
    CHECK(mono && fabsf(slope - 1) < 0.01f, "expand: increasing, slope 1 at the knee (%.4f)", slope);
}

static void test_pixel(void) {
    const HdrLevels l = {200, 1000, 1};
    float pq[3];
    const float white[3] = {1, 1, 1}, grey[3] = {0.5f, 0.5f, 0.5f};
    float top = hdr_pixel(&l, white, 0, 0, pq);
    CHECK(fabsf(top - 200) < 0.01f && fabsf(pq[0] - hdr_pq_encode(200)) < 1e-5f && fabsf(pq[2] - pq[0]) < 1e-5f, "pixel: HUD white at paper white (%.1f nits)", top);
    top = hdr_pixel(&l, white, 1, 1, pq);
    CHECK(fabsf(top - 1000) < 0.5f && fabsf(pq[1] - hdr_pq_encode(1000)) < 1e-4f, "pixel: a light source's white at the peak (%.1f nits)", top);
    const float scene_white = hdr_pixel(&l, white, 1, 0, pq);
    CHECK(scene_white > 200 && scene_white < 1000, "pixel: an unlit scene white partly expanded (%.1f nits)", scene_white);
    const float g0 = hdr_pixel(&l, grey, 0, 0, pq), g1 = hdr_pixel(&l, grey, 1, 1, pq);
    CHECK(fabsf(g0 - g1) < 1e-3f && fabsf(g0 - 200 * powf(0.5f, 2.2f)) < 0.01f, "pixel: mid-tones as in SDR in the scene and the HUD (%.2f nits)", g0);
    const float red[3] = {1, 0.1f, 0.1f};
    top = hdr_pixel(&l, red, 1, 1, pq);
    CHECK(top <= 1000.01f, "pixel: a saturated light never above the peak (%.1f nits)", top);
    const HdrLevels sat = {200, 1000, 1.6f};
    float a[3], b[3];
    const float col[3] = {0.7f, 0.4f, 0.3f};
    hdr_pixel(&l, col, 0, 0, a);
    hdr_pixel(&sat, col, 0, 0, b);
    CHECK(b[0] - b[2] > a[0] - a[2], "pixel: saturation applies in HDR too (red - blue %.4f > %.4f)", b[0] - b[2], a[0] - a[2]);
    const HdrLevels flat = {300, 400, 1};
    top = hdr_pixel(&flat, white, 1, 1, pq);
    CHECK(fabsf(top - 400) < 0.5f, "pixel: the peak follows --hdr-peak (%.1f nits)", top);
}

static void test_pick(void) {
    char why[160];
    int hdr = 0;
    const HdrSurfaceFormat sdr[] = {{HDR_VK_FORMAT_B8G8R8A8_SRGB, 0, 1}, {HDR_VK_FORMAT_B8G8R8A8_UNORM, 0, 1}};
    CHECK(hdr_pick_format(sdr, 2, HDR_OFF, 1, &hdr, why, sizeof why) == 1 && !hdr && !why[0], "pick: off, UNORM before sRGB");
    CHECK(hdr_pick_format(sdr, 2, HDR_AUTO, 1, &hdr, why, sizeof why) == 1 && !hdr && strstr(why, "no HDR10"), "pick: auto on an SDR display: SDR (%s)", why);
    const HdrSurfaceFormat both[] = {{HDR_VK_FORMAT_B8G8R8A8_UNORM, 0, 1}, {HDR_VK_FORMAT_A2B10G10R10_UNORM, 0, 1},
                                     {HDR_VK_FORMAT_A2B10G10R10_UNORM, HDR_VK_COLOR_SPACE_HDR10_ST2084, 1}};
    CHECK(hdr_pick_format(both, 3, HDR_AUTO, 1, &hdr, why, sizeof why) == 2 && hdr && !why[0], "pick: auto with HDR10 offered: A2B10G10R10 ST 2084");
    CHECK(hdr_pick_format(both, 3, HDR_ON, 1, &hdr, why, sizeof why) == 2 && hdr, "pick: on with HDR10 offered");
    CHECK(hdr_pick_format(both, 3, HDR_OFF, 1, &hdr, why, sizeof why) == 0 && !hdr, "pick: off ignores HDR10");
    CHECK(hdr_pick_format(both, 3, HDR_ON, 0, &hdr, why, sizeof why) == 0 && !hdr && strstr(why, "swapchain_colorspace"),
          "pick: without the colour space extension: SDR (%s)", why);
    const HdrSurfaceFormat fp16[] = {{HDR_VK_FORMAT_B8G8R8A8_UNORM, 0, 1}, {HDR_VK_FORMAT_R16G16B16A16_SFLOAT, HDR_VK_COLOR_SPACE_HDR10_ST2084, 1}};
    CHECK(hdr_pick_format(fp16, 2, HDR_AUTO, 1, &hdr, why, sizeof why) == 1 && hdr, "pick: FP16 ST 2084 taken");
    const HdrSurfaceFormat noblit[] = {{HDR_VK_FORMAT_B8G8R8A8_UNORM, 0, 1}, {HDR_VK_FORMAT_A2B10G10R10_UNORM, HDR_VK_COLOR_SPACE_HDR10_ST2084, 0}};
    CHECK(hdr_pick_format(noblit, 2, HDR_ON, 1, &hdr, why, sizeof why) == 0 && !hdr && strstr(why, "blit"), "pick: HDR10 that cannot be blitted to: SDR (%s)", why);
    const HdrSurfaceFormat odd[] = {{HDR_VK_FORMAT_B8G8R8A8_UNORM, 1000104001u, 1}, {HDR_VK_FORMAT_B8G8R8A8_UNORM, 0, 1}};
    CHECK(hdr_pick_format(odd, 2, HDR_OFF, 1, &hdr, why, sizeof why) == 1, "pick: SDR prefers the sRGB non-linear colour space of a format");
    const HdrSurfaceFormat only_other[] = {{HDR_VK_FORMAT_B8G8R8A8_UNORM, 1000104001u, 1}};
    CHECK(hdr_pick_format(only_other, 1, HDR_OFF, 1, &hdr, why, sizeof why) == 0, "pick: another colour space when it is the only one (as before)");
    const HdrSurfaceFormat none[] = {{HDR_VK_FORMAT_B8G8R8A8_UNORM, 0, 0}};
    CHECK(hdr_pick_format(none, 1, HDR_OFF, 1, &hdr, why, sizeof why) == -1, "pick: nothing to blit to: -1");
}

int main(void) {
    test_args();
    test_pq();
    test_gamut();
    test_saturation();
    test_expand();
    test_pixel();
    test_pick();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
