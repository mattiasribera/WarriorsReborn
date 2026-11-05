/* Game-independent --sky-hd test: option parsing; the sky-stage classifier on synthetic frames
 * shaped like the title's (colour clear, dome, cap, skyline, depth-only clear, far glows) and on
 * frames without a sky; the dome palette (ramp rows, trimmed means that ignore stars and painted
 * buildings, silhouette colour, star detection) on synthetic textures; the sky and skyline
 * parameters (camera basis, moon opposite the shadow direction, rain). */
#include "../src/sky.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static SkyOp clear_color(void) { return (SkyOp){.kind = SKY_OP_CLEAR_COLOR}; }
static SkyOp clear_depth(void) { return (SkyOp){.kind = SKY_OP_CLEAR_DEPTH}; }
static SkyOp other(void) { return (SkyOp){.kind = SKY_OP_OTHER}; }
static SkyOp dome(void) {
    return (SkyOp){.kind = SKY_OP_DRAW, .fvf = 0x152, .nv = 37, .near = 0.0505f, .radius = 0.63f, .view_origin = 1,
                   .tex_fmt = SKY_TEX_DXT1, .tex_w = 256, .tex_h = 512};
}
static SkyOp cap(void) {
    return (SkyOp){.kind = SKY_OP_DRAW, .fvf = 0x152, .nv = 27, .near = 0.0505f, .radius = 0.6f, .view_origin = 1, .blend = 1,
                   .tex_fmt = 0x0F, .tex_w = 512, .tex_h = 256};
}
static SkyOp skyline(void) {
    return (SkyOp){.kind = SKY_OP_DRAW, .fvf = 0x252, .nv = 3293, .near = 38.99f, .radius = 700, .zwrite = 1, .tex_fmt = 0x0F, .tex_w = 32,
                   .tex_h = 16};
}
static SkyOp glow(void) {
    return (SkyOp){.kind = SKY_OP_DRAW, .fvf = 0x152, .nv = 380, .near = 0.1f, .radius = 400, .blend = 1, .emissive = 0.99f, .tex_fmt = 0x0F,
                   .tex_w = 64, .tex_h = 64};
}
static SkyOp world(void) {
    return (SkyOp){.kind = SKY_OP_DRAW, .fvf = 0, .nv = 500, .near = 0.1f, .radius = 50, .zwrite = 1, .tex_fmt = SKY_TEX_DXT1, .tex_w = 256,
                   .tex_h = 256};
}

static void test_args(void) {
    SkyConfig c = SKY_DEFAULTS;
    CHECK(!c.on, "sky off by default");
    CHECK(sky_arg(&c, "--sky-hd", NULL) == 1 && c.on, "--sky-hd turns it on");
    CHECK(sky_arg(&c, "--no-sky-hd", "x") == 1 && !c.on, "--no-sky-hd turns it off");
    CHECK(sky_arg(&c, "--rain-hd", NULL) == 0 && !c.on, "other options are not ours");
}

static void test_classify(void) {
    SkyStage st;
    /* The title's frame: Cf3 D152/37 D152/27 D252/n C3 D152/m D152/m world... */
    SkyOp f[] = {clear_color(), dome(), cap(), skyline(), clear_depth(), glow(), glow(), world(), world()};
    int r = sky_classify(f, sizeof f / sizeof f[0], &st);
    CHECK(r == 1 && st.dome == 1 && st.cap == 2 && st.skyline == 3, "full stage: dome %d cap %d skyline %d", st.dome, st.cap, st.skyline);
    CHECK(st.glow_first == 5 && st.glow_count == 2, "far glows after the depth clear: %d x %d", st.glow_first, st.glow_count);

    /* Offscreen operations inside the stage and before the glows are skipped. */
    SkyOp g[] = {other(), clear_color(), other(), dome(), skyline(), other(), clear_depth(), other(), glow(), world()};
    r = sky_classify(g, sizeof g / sizeof g[0], &st);
    CHECK(r == 1 && st.dome == 3 && st.cap == -1 && st.skyline == 4 && st.glow_first == 8 && st.glow_count == 1,
          "no cap, offscreen ops between: dome %d cap %d skyline %d glows %d x %d", st.dome, st.cap, st.skyline, st.glow_first, st.glow_count);

    /* level54-style skyline: FVF 0x152, 16x16 DXT1 atlas. */
    SkyOp k = skyline();
    k.fvf = 0x152;
    k.tex_fmt = SKY_TEX_DXT1;
    k.tex_w = k.tex_h = 16;
    SkyOp h[] = {clear_color(), dome(), k, clear_depth(), world()};
    r = sky_classify(h, 5, &st);
    CHECK(r == 1 && st.skyline == 2 && st.glow_first == -1 && st.glow_count == 0, "16x16 atlas skyline, no glows: skyline %d glows %d",
          st.skyline, st.glow_count);

    /* An interior's 16x16 dome is not a sky. */
    SkyOp small = dome();
    small.tex_w = small.tex_h = 16;
    SkyOp i1[] = {clear_color(), small, clear_depth(), world()};
    r = sky_classify(i1, 4, &st);
    CHECK(r == 0 && st.dome == -1 && st.skyline == -1 && st.glow_first == -1, "16x16 dome: no sky");

    /* The first draw of the stage must be the dome: a world draw first means no sky. */
    SkyOp i2[] = {clear_color(), world(), dome(), clear_depth()};
    CHECK(sky_classify(i2, 4, &st) == 0, "world draw first: no sky");

    /* A dome drawn with depth writes, off-centre or with a far near plane is not the dome. */
    SkyOp d1 = dome();
    d1.zwrite = 1;
    SkyOp d2 = dome();
    d2.view_origin = 0;
    SkyOp d3 = dome();
    d3.near = 39;
    SkyOp d4 = dome();
    d4.radius = 5;
    SkyOp d5 = dome();
    d5.fvf = 0;
    SkyOp d6 = dome();
    d6.tex_h = 256;
    const SkyOp *bad[] = {&d1, &d2, &d3, &d4, &d5, &d6};
    for (int b = 0; b < 6; b++) {
        SkyOp fr[] = {clear_color(), *bad[b], clear_depth()};
        CHECK(sky_classify(fr, 3, &st) == 0, "not a dome (case %d)", b);
    }

    /* No colour clear: no stage. Menus (no depth clear): the stage runs to the end. */
    SkyOp n1[] = {dome(), cap(), clear_depth()};
    CHECK(sky_classify(n1, 3, &st) == 0, "no colour clear: no sky");
    SkyOp n2[] = {clear_color(), dome(), cap()};
    r = sky_classify(n2, 3, &st);
    CHECK(r == 1 && st.cap == 2 && st.glow_first == -1, "no depth clear: dome and cap, no glows");

    /* A second colour clear restarts the stage (the frame's last colour clear before the depth clear). */
    SkyOp n3[] = {clear_color(), world(), clear_color(), dome(), clear_depth()};
    r = sky_classify(n3, 5, &st);
    CHECK(r == 1 && st.dome == 3, "stage after the last colour clear: dome %d", st.dome);

    /* The cap must be blended; an unblended second draw is not the cap (and not the skyline). */
    SkyOp c1 = cap();
    c1.blend = 0;
    SkyOp n4[] = {clear_color(), dome(), c1, clear_depth()};
    r = sky_classify(n4, 4, &st);
    CHECK(r == 1 && st.cap == -1 && st.skyline == -1, "unblended second draw: no cap");

    /* Glows stop at the first draw that is not one. */
    SkyOp g1 = glow();
    g1.emissive = 0.5f;
    SkyOp n5[] = {clear_color(), dome(), clear_depth(), glow(), g1, glow()};
    r = sky_classify(n5, 6, &st);
    CHECK(r == 1 && st.glow_first == 3 && st.glow_count == 1, "glows: run of %d", st.glow_count);
    CHECK(sky_classify(NULL, 0, &st) == 0 && st.dome == -1, "empty frame");
}

static uint32_t argb(int r, int g, int b) { return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
static int ch(uint32_t c, int k) { return (int)(c >> (16 - 8 * k) & 255); }

/* A dome texture with a vertical gradient: row y (0 = top) is (10 + 200 y / (h - 1), 50, 200 - 150 y / (h - 1)). */
static void gradient(uint32_t *t, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) t[y * w + x] = argb(10 + (int)(200.0f * y / (h - 1)), 50, 200 - (int)(150.0f * y / (h - 1)));
}

static void test_palette(void) {
    enum { W = 64, H = 128 };
    static uint32_t t[W * H];
    SkyPalette p;
    CHECK(sky_palette(t, 4, 4, &p) == -1, "too small: rejected");
    CHECK(fabsf(sky_dome_row(0) - SKY_HORIZON_ROW) < 1e-4f && fabsf(sky_dome_row(32) - 0.375f) < 1e-3f && fabsf(sky_dome_row(52) - 0.06f) < 1e-3f &&
              sky_dome_row(90) == 0.0f,
          "dome rows: 0 deg %.3f, 32 deg %.3f, 52 deg %.3f, 90 deg %.3f", sky_dome_row(0), sky_dome_row(32), sky_dome_row(52), sky_dome_row(90));
    CHECK(sky_dome_row(10) > sky_dome_row(20) && sky_dome_row(60) > sky_dome_row(80), "dome rows rise toward the top");

    /* The ramp follows the gradient at the rows sky_dome_row gives. */
    gradient(t, W, H);
    CHECK(sky_palette(t, W, H, &p) == 0, "gradient: palette");
    int ok = 1;
    for (int k = 0; k < SKY_RAMP; k++) {
        float row = sky_dome_row(90.0f * k / (SKY_RAMP - 1));
        int want = 10 + (int)(200.0f * row);
        if (abs(ch(p.ramp[k], 0) - want) > 6 || ch(p.ramp[k], 1) != 50) ok = 0;
        printf("     ramp[%d] %06x (row %.3f, red %d)\n", k, p.ramp[k] & 0xFFFFFF, row, want);
    }
    CHECK(ok, "ramp follows the texture's vertical gradient");
    CHECK(ch(p.ramp[0], 0) > ch(p.ramp[SKY_RAMP - 1], 0) && ch(p.ramp[0], 2) < ch(p.ramp[SKY_RAMP - 1], 2), "horizon red, zenith blue");
    CHECK(p.stars == 0.0f, "no stars in a smooth gradient (%.3f)", p.stars);

    /* Painted buildings (dark, a third of the row) and stars (single bright texels) do not move
     * the ramp; the silhouette colour is the buildings'; the stars are counted. */
    uint32_t sky[W * H];
    for (uint32_t i = 0; i < W * H; i++) sky[i] = argb(30, 40, 90);
    for (uint32_t y = (uint32_t)(H * 0.8f); y < H; y++)
        for (uint32_t x = 0; x < W; x += 3) sky[y * W + x] = argb(5, 5, 10);
    for (uint32_t y = 0; y < (uint32_t)(H * 0.3f); y += 4) sky[y * W + (y * 7) % W] = argb(250, 250, 250);
    CHECK(sky_palette(sky, W, H, &p) == 0, "buildings and stars: palette");
    ok = 1;
    for (int k = 0; k < SKY_RAMP; k++)
        if (abs(ch(p.ramp[k], 0) - 30) > 2 || abs(ch(p.ramp[k], 2) - 90) > 2) ok = 0;
    CHECK(ok, "ramp ignores buildings and stars (horizon %06x, zenith %06x)", p.ramp[0] & 0xFFFFFF, p.ramp[SKY_RAMP - 1] & 0xFFFFFF);
    CHECK(abs(ch(p.silhouette, 0) - 5) <= 2 && abs(ch(p.silhouette, 2) - 10) <= 2, "silhouette = the buildings (%06x)", p.silhouette & 0xFFFFFF);
    const float share = (float)((uint32_t)(H * 0.3f + 3) / 4) / (W * (uint32_t)(H * SKY_STAR_ROWS));
    const float want = share / SKY_STAR_FULL > 1 ? 1 : share / SKY_STAR_FULL;
    CHECK(fabsf(p.stars - want) < 0.05f && p.stars > 0, "stars counted: %.3f (want %.3f)", p.stars, want);

    /* A painted moon (a large bright disc) is not stars; a dark zenith gets the night minimum. */
    for (uint32_t i = 0; i < W * H; i++) sky[i] = argb(8, 8, 16);
    for (uint32_t y = 2; y < 14; y++)
        for (uint32_t x = 2; x < 14; x++) sky[y * W + x] = argb(240, 240, 230);
    CHECK(sky_palette(sky, W, H, &p) == 0 && fabsf(p.stars - SKY_STAR_DARK_MIN) < 1e-5f, "moon disc not counted, dark zenith: stars %.3f",
          p.stars);
}

/* A camera looking along +y (world, z up), D3D row-vector view: view x = world x, view y = world
 * z, view z = world y. */
static void camera(float view[16], float proj[16]) {
    memset(view, 0, 64);
    memset(proj, 0, 64);
    view[0] = 1;    /* world x -> view x */
    view[9] = 1;    /* world z -> view y */
    view[6] = 1;    /* world y -> view z */
    view[15] = 1;
    proj[0] = 1.397f;
    proj[5] = 1.995f;
    proj[10] = 1.0f;
    proj[11] = 1;
    proj[14] = -0.05f;
}

static void test_build(void) {
    SkyPalette p;
    for (int k = 0; k < SKY_RAMP; k++) p.ramp[k] = argb(200, 100, 50);
    p.silhouette = argb(20, 20, 20);
    p.stars = 0.8f;
    float view[16], proj[16];
    camera(view, proj);
    RSky s;
    const float sun[3] = {0.452f, 0.452f, -0.769f};
    CHECK(sky_build(&p, view, proj, argb(128, 255, 64), 0, 0, sun, 12.5f, &s) == 1, "sky built");
    CHECK(fabsf(s.fwd[1] - 1) < 1e-5f && fabsf(s.fwd[0]) < 1e-5f && fabsf(s.fwd[2]) < 1e-5f, "forward = world +y (%.3f %.3f %.3f)", s.fwd[0],
          s.fwd[1], s.fwd[2]);
    CHECK(fabsf(s.right[0] - 1 / 1.397f) < 1e-4f && fabsf(s.up[2] - 1 / 1.995f) < 1e-4f, "right and up scaled by the projection (%.4f, %.4f)",
          s.right[0], s.up[2]);
    CHECK(ch(s.ramp[0], 0) == 100 && ch(s.ramp[0], 1) == 100 && ch(s.ramp[0], 2) == 13, "palette scaled by the dome's shade (%06x)",
          s.ramp[0] & 0xFFFFFF);
    CHECK(fabsf(s.moon[0] + 0.452f) < 1e-3f && fabsf(s.moon[2] - 0.769f) < 1e-3f && s.moon_size == SKY_MOON_RADIUS && s.moon_light > 0.99f,
          "moon opposite the shadow direction (%.3f %.3f %.3f)", s.moon[0], s.moon[1], s.moon[2]);
    CHECK(fabsf(s.cover - SKY_COVER_CLEAR) < 1e-5f && fabsf(s.stars - 0.8f * SKY_STAR_CHANCE) < 1e-6f && s.seconds == 12.5f, "clear sky: cover %.2f stars %.2f",
          s.cover, s.stars);
    CHECK(sky_build(&p, view, proj, argb(255, 255, 255), 1, 0, sun, 0, &s) == 1 && fabsf(s.cover - SKY_COVER_CAP) < 1e-5f, "cap: more clouds");
    CHECK(sky_build(&p, view, proj, argb(255, 255, 255), 1, 1, sun, 0, &s) == 1 && fabsf(s.cover - SKY_COVER_RAIN) < 1e-5f && s.stars == 0 &&
              s.moon_light == 0,
          "rain: overcast, no stars, no moon");
    const float up_sun[3] = {0, 0, 1};
    CHECK(sky_build(&p, view, proj, argb(255, 255, 255), 0, 0, up_sun, 0, &s) == 1 && s.moon_size == 0, "shadows pointing up: no moon");
    CHECK(sky_build(&p, view, proj, argb(255, 255, 255), 0, 0, NULL, 0, &s) == 1 && s.moon_size == 0, "no sun constant: no moon");
    p.stars = 0;
    CHECK(sky_build(&p, view, proj, argb(255, 255, 255), 0, 0, sun, 0, &s) == 1 && s.moon_size == 0, "starless sky (dawn, overcast): no moon");
    float bad[16];
    memcpy(bad, proj, sizeof bad);
    bad[11] = 0;
    bad[15] = 1;
    CHECK(sky_build(&p, view, bad, 0, 0, 0, sun, 0, &s) == 0, "orthographic projection: no sky");

    RSkyline k;
    p.ramp[0] = argb(100, 80, 60);
    sky_skyline_build(&p, argb(128, 128, 128), 0, 3, &k);
    CHECK(ch(k.haze, 0) == 50 && ch(k.haze, 1) == 40 && ch(k.haze, 2) == 30 && k.windows == 1 && k.haze_max == SKY_HAZE_MAX &&
              k.haze_dist == SKY_HAZE_DIST && k.seconds == 3,
          "skyline: haze = shaded horizon (%06x)", k.haze & 0xFFFFFF);
    sky_skyline_build(&p, argb(128, 128, 128), 1, 3, &k);
    CHECK(k.windows < 1 && k.windows > 0, "skyline: rain dims the windows (%.2f)", k.windows);
}

static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
/* Stars a 1920 x 1080 screen shows: the distinct star cells (sky.frag: floor(d x SKY_STAR_CELLS),
 * d the pixel's view direction) above SKY_STAR_MIN_ELEVATION times the chance of a cell holding a
 * star (RSky.stars). The camera of camera() pitched up by pitch radians. */
static float stars_on_screen(const RSky *s, float pitch) {
    enum { W = 1920, H = 1080, STEP = 2 };
    static uint64_t keys[(W / STEP) * (H / STEP)];
    uint32_t n = 0;
    const float c = cosf(pitch), sn = sinf(pitch);
    for (int y = 0; y < H; y += STEP)
        for (int x = 0; x < W; x += STEP) {
            const float nx = (x + 0.5f) / W * 2 - 1, ny = 1 - (y + 0.5f) / H * 2;
            float d[3];
            for (int i = 0; i < 3; i++) d[i] = s->fwd[i] + nx * s->right[i] + ny * s->up[i];
            /* pitch about world x: y forward, z up */
            const float dy = d[1] * c - d[2] * sn, dz = d[1] * sn + d[2] * c;
            d[1] = dy;
            d[2] = dz;
            const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int i = 0; i < 3; i++) d[i] /= l;
            if (asinf(d[2]) <= SKY_STAR_MIN_ELEVATION) continue;
            uint64_t k = 0;
            for (int i = 0; i < 3; i++) k = k << 21 | (uint64_t)((int64_t)floorf(d[i] * SKY_STAR_CELLS) + (1 << 20));
            keys[n++] = k;
        }
    qsort(keys, n, sizeof *keys, cmp_u64);
    uint32_t cells = 0;
    for (uint32_t i = 0; i < n; i++) cells += !i || keys[i] != keys[i - 1];
    return (float)cells * s->stars;
}

/* A hazy 1979 city sky: a few dozen stars on a screen, not hundreds. */
static void test_star_density(void) {
    SkyPalette p;
    for (int k = 0; k < SKY_RAMP; k++) p.ramp[k] = argb(10, 10, 20);
    p.silhouette = argb(5, 5, 5);
    float view[16], proj[16];
    camera(view, proj);
    RSky s;
    p.stars = SKY_STAR_DARK_MIN;   /* the usual night dome */
    CHECK(sky_build(&p, view, proj, argb(255, 255, 255), 0, 0, NULL, 0, &s) == 1, "night sky built");
    const float typical = stars_on_screen(&s, 0.6f);
    CHECK(typical >= 8 && typical <= 50, "night dome: %.0f stars on a 1080p screen looking up (want 8-50)", typical);
    p.stars = 1;
    sky_build(&p, view, proj, argb(255, 255, 255), 0, 0, NULL, 0, &s);
    const float full = stars_on_screen(&s, 0.6f);
    CHECK(full <= 90, "starriest dome: %.0f stars on a 1080p screen (want at most 90)", full);
    CHECK(s.stars > 0 && s.stars < 0.01f, "star chance per cell %.4f is small", s.stars);
}

int main(void) {
    test_args();
    test_star_density();
    test_classify();
    test_palette();
    test_build();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
