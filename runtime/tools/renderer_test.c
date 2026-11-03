/* Game-independent test of the Vulkan renderer (src/renderer.h): clear + readback parity with
 * the CPU clear path (XRGB8888 bytes B, G, R, X), the initial color upload and the order of
 * clears, draws, textures, output state and the register-combiner pixel shader. Exits 77 (skip)
 * when no Vulkan device can be initialized. */
#include "../src/renderer.h"
#include "../src/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

enum { W = 64, H = 48 };

/* Number of pixels in px (w x h, tightly packed) that differ from expect (low 24 bits). */
static int mismatches(const uint32_t *px, uint32_t expect, uint32_t *first) {
    int n = 0;
    for (int i = 0; i < W * H; i++)
        if ((px[i] & 0xFFFFFFu) != (expect & 0xFFFFFFu)) { if (!n) *first = px[i]; n++; }
    return n;
}

/* Channel distance between two XRGB pixels (max over R, G, B). */
static int dist(uint32_t a, uint32_t b) {
    int m = 0;
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF);
        d = d < 0 ? -d : d;
        m = d > m ? d : m;
    }
    return m;
}

/* Full-screen quad (clip space, y up) with texture coordinates u0..u1 / v0..v1 (v0 at the top)
 * on every stage (q = 1). */
static void quad(RVertex q[4], float u0, float v0, float u1, float v1, uint32_t color) {
    const float xy[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}}, uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    for (int i = 0; i < 4; i++) {
        q[i] = (RVertex){xy[i][0], xy[i][1], 0.5f, 1, color, 0, {{0}}, 0};
        for (int s = 0; s < RENDERER_STAGES; s++) {
            q[i].tex[s][0] = uv[i][0];
            q[i].tex[s][1] = uv[i][1];
            q[i].tex[s][3] = 1;
        }
    }
}

/* Texture cache budget: the cache is dropped at renderer_begin once the textures exceed it
 * (texture packs raise it, see renderer_set_texture_budget). */
static void test_texture_budget(void) {
    static uint32_t big[256 * 256];
    renderer_begin(NULL, 0);   /* start from a small cache */
    renderer_set_texture_budget(1u << 20);
    uint32_t a = renderer_texture_create(0xB0001, RENDERER_TEX_BGRA8, 256, 256, 1, big);   /* 256 KiB */
    renderer_begin(NULL, 0);
    CHECK(a && renderer_texture_find(0xB0001) == a, "under a 1 MiB budget the texture stays cached");
    for (uint32_t k = 0; k < 4; k++) renderer_texture_create(0xB0002 + k, RENDERER_TEX_BGRA8, 256, 256, 1, big);
    renderer_begin(NULL, 0);
    CHECK(!renderer_texture_find(0xB0001), "past the budget the cache is dropped at the next frame");
    renderer_set_texture_budget(64u << 20);
    for (uint32_t k = 0; k < 5; k++) renderer_texture_create(0xB0010 + k, RENDERER_TEX_BGRA8, 256, 256, 1, big);
    renderer_begin(NULL, 0);
    CHECK(renderer_texture_find(0xB0010) != 0, "a larger budget keeps the same textures");
    renderer_set_texture_budget(0);   /* back to the default */
    uint32_t px[W * H];
    renderer_end(px, W * 4);
}

static void test_textures(void) {
    static uint32_t px[W * H];
    const RViewport full = {0, 0, W, H, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    int r;

    /* 2x2 B8G8R8A8 texture sampled at its texel centres: texel (0,0) top left, (1,0) top right. */
    const uint32_t texels[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    CHECK(renderer_texture_find(0x1234) == 0, "unknown texture key is not cached");
    uint32_t t = renderer_texture_create(0x1234, RENDERER_TEX_BGRA8, 2, 2, 1, texels);
    CHECK(t != 0 && renderer_texture_find(0x1234) == t, "created texture is found by its key (%u)", t);
    renderer_begin(NULL, 0);
    renderer_set_texture(t);
    quad(q, 0.25f, 0.25f, 0.75f, 0.75f, 0xFFFFFFFFu);
    renderer_draw(&full, 0, q, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && dist(px[1 * W + 1], 0xFF0000u) < 24 && dist(px[1 * W + W - 2], 0x00FF00u) < 24 &&
              dist(px[(H - 2) * W + 1], 0x0000FFu) < 24 && dist(px[(H - 2) * W + W - 2], 0xFFFFFFu) < 24,
          "2x2 texture corners: %06x %06x %06x %06x", px[W + 1] & 0xFFFFFFu, px[W + W - 2] & 0xFFFFFFu, px[(H - 2) * W + 1] & 0xFFFFFFu,
          px[(H - 2) * W + W - 2] & 0xFFFFFFu);

    /* Modulation by the vertex color, and wrap addressing (u 1..2 samples like 0..1). */
    const uint32_t grey[16] = {0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u,
                               0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u};
    uint32_t g = renderer_texture_create(0x5678, RENDERER_TEX_BGRA8, 4, 4, 1, grey);
    renderer_begin(NULL, 0);
    renderer_set_texture(g);
    quad(q, 1, 1, 2, 2, 0xFFFF8000u);
    renderer_draw(&full, 0, q, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && dist(px[(H / 2) * W + W / 2], 0x804000u) < 3, "texture x vertex color: %06x (want ~804000)",
          px[(H / 2) * W + W / 2] & 0xFFFFFFu);

    /* Alpha test (D3DCMPFUNC numbering) against a 0..255 reference; alpha = texture x vertex. */
    const uint32_t half_tex[4] = {0x80FF0000u, 0x80FF0000u, 0x80FF0000u, 0x80FF0000u};
    uint32_t a = renderer_texture_create(0x9abc, RENDERER_TEX_BGRA8, 2, 2, 1, half_tex);
    static const struct { uint32_t func, ref, color; int drawn; } at[] = {
        {5, 0x80, 0xFFFFFFFFu, 0}, {7, 0x80, 0xFFFFFFFFu, 1}, {5, 1, 0xFFFFFFFFu, 1}, {5, 1, 0x00FFFFFFu, 0},
        {2, 0x81, 0xFFFFFFFFu, 1}, {1, 0, 0xFFFFFFFFu, 0}, {0, 0xFF, 0x00FFFFFFu, 1}};
    for (unsigned i = 0; i < sizeof at / sizeof *at; i++) {
        renderer_begin(NULL, 0);
        renderer_clear(RENDERER_CLEAR_COLOR, 0x00203040u, 1.0f);
        renderer_set_texture(a);
        renderer_set_alpha_test(at[i].func, at[i].ref);
        quad(q, 0, 0, 1, 1, at[i].color);
        renderer_draw(&full, 0, q, 4, qi, 6);
        r = renderer_end(px, W * 4);
        uint32_t want = at[i].drawn ? 0x00FF0000u & at[i].color : 0x00203040u;
        CHECK(r == 0 && px[(H / 2) * W + W / 2] == want, "alpha test func %u ref %02x, vertex alpha %02x: %08x (want %08x)", at[i].func,
              at[i].ref, at[i].color >> 24, px[(H / 2) * W + W / 2], want);
    }

    /* Texture 0: the vertex color as before (untextured draws). */
    renderer_begin(NULL, 0);
    renderer_set_texture(0);
    quad(q, 0, 0, 1, 1, 0x00123456u);
    renderer_draw(&full, 0, q, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && px[(H / 2) * W + W / 2] == 0x00123456u, "texture 0 draws the vertex color (%08x)", px[(H / 2) * W + W / 2]);

    /* Mip levels: level 1 (1x1) is used when the texture is minified to one pixel per 64 texels. */
    uint32_t mip[4 * 4 + 2 * 2 + 1];
    for (int i = 0; i < 16; i++) mip[i] = 0xFFFF0000u;
    for (int i = 16; i < 20; i++) mip[i] = 0xFF00FF00u;
    mip[20] = 0xFF0000FFu;
    uint32_t m = renderer_texture_create(0xdef0, RENDERER_TEX_BGRA8, 4, 4, 3, mip);
    renderer_begin(NULL, 0);
    renderer_set_texture(m);
    quad(q, 0, 0, 256, 256, 0xFFFFFFFFu);
    renderer_draw(&full, 0, q, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && dist(px[(H / 2) * W + W / 2], 0x0000FFu) < 12, "minified texture samples the last mip level (%06x)",
          px[(H / 2) * W + W / 2] & 0xFFFFFFu);

    /* BC1 (DXT1) blocks uploaded as is when the device supports BC formats. */
    if (renderer_has_bc()) {
        const uint8_t blk[8] = {0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0};   /* c0 = red, all indices 0 */
        uint32_t b = renderer_texture_create(0x1111, RENDERER_TEX_BC1, 4, 4, 1, blk);
        renderer_begin(NULL, 0);
        renderer_set_texture(b);
        quad(q, 0, 0, 1, 1, 0xFFFFFFFFu);
        renderer_draw(&full, 0, q, 4, qi, 6);
        r = renderer_end(px, W * 4);
        CHECK(b && r == 0 && px[(H / 2) * W + W / 2] == 0x00FF0000u, "BC1 block samples red (%08x)", px[(H / 2) * W + W / 2]);
    } else {
        printf("ok   BC formats unsupported on this device: DXT textures are decoded on the CPU\n");
    }
}

/* Output state (renderer_set_state, NV2A method values): blending, color mask, depth write and
 * function, culling, and the pipeline cache. */
static void test_state(void) {
    static uint32_t px[W * H];
    const RViewport full = {0, 0, W, H, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RState rs;
    int r;
    const uint32_t c = (H / 2) * W + W / 2;

    renderer_state_default(&rs);
    CHECK(rs.blend_enable == 0 && rs.blend_src == 1 && rs.blend_dst == 0 && rs.blend_eq == 0x8006 && rs.color_mask == 0x01010101u &&
              rs.depth_write == 1 && rs.depth_func == 0x203 && rs.cull == 0,
          "default state: no blending, ONE/ZERO, ADD, all channels, depth write LEQUAL, no culling");

    /* SRC_ALPHA / ONE_MINUS_SRC_ALPHA: half-transparent green over red. */
    static const struct { uint32_t src, dst, eq, color, want; const char *what; } bl[] = {
        {0x302, 0x303, 0x8006, 0x8000FF00u, 0x7F8000u, "SRC_ALPHA, ONE_MINUS_SRC_ALPHA"},
        {1, 1, 0x8006, 0x40004080u, 0xFF4080u, "ONE, ONE (additive)"},
        {0x302, 1, 0x8006, 0x80004080u, 0xFF2040u, "SRC_ALPHA, ONE"},
        {1, 1, 0x800B, 0xFF400000u, 0xBF0000u, "ONE, ONE, REVERSE_SUBTRACT"},
        {0x306, 0, 0x8006, 0xFF808080u, 0x800000u, "DST_COLOR, ZERO (modulate)"},
        {1, 1, 0x8008, 0xFF8000FFu, 0xFF00FFu, "MAX"},
        {0x8003, 0x8004, 0x8006, 0xFF00FF00u, 0xBF4000u, "CONSTANT_ALPHA (0x40), ONE_MINUS_CONSTANT_ALPHA"},
        {0x304, 0, 0x8006, 0xFF00FF00u, 0x00FF00u, "DST_ALPHA of the alpha-less target is 1"},
    };
    for (unsigned i = 0; i < sizeof bl / sizeof *bl; i++) {
        renderer_begin(NULL, 0);
        renderer_clear(RENDERER_CLEAR_COLOR, 0x00FF0000u, 1.0f);
        renderer_state_default(&rs);
        rs.blend_enable = 1;
        rs.blend_src = bl[i].src;
        rs.blend_dst = bl[i].dst;
        rs.blend_eq = bl[i].eq;
        rs.blend_color = 0x40000000u;
        renderer_set_state(&rs);
        quad(q, 0, 0, 1, 1, bl[i].color);
        renderer_draw(&full, 0, q, 4, qi, 6);
        r = renderer_end(px, W * 4);
        CHECK(r == 0 && dist(px[c], bl[i].want) <= 2, "blend %s: %06x (want ~%06x)", bl[i].what, px[c] & 0xFFFFFFu, bl[i].want);
    }

    /* Blending off ignores the source alpha (also when the factors are set). */
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR, 0x00FF0000u, 1.0f);
    renderer_state_default(&rs);
    rs.blend_src = 0x302;
    rs.blend_dst = 0x303;
    renderer_set_state(&rs);
    quad(q, 0, 0, 1, 1, 0x2000FF00u);
    renderer_draw(&full, 0, q, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && (px[c] & 0xFFFFFFu) == 0x00FF00u, "blend disabled writes the source color (%06x)", px[c] & 0xFFFFFFu);

    /* Color mask: only red and blue written. */
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR, 0x00204060u, 1.0f);
    renderer_state_default(&rs);
    rs.color_mask = 0x00010001u;
    renderer_set_state(&rs);
    quad(q, 0, 0, 1, 1, 0xFFFFFFFFu);
    renderer_draw(&full, 0, q, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && (px[c] & 0xFFFFFFu) == 0xFF40FFu, "color mask R+B keeps green (%06x)", px[c] & 0xFFFFFFu);

    /* Depth write off: a near quad drawn without writes does not hide a later far quad. */
    RVertex nearq[4], farq[4];
    quad(nearq, 0, 0, 1, 1, 0xFF0000FFu);
    quad(farq, 0, 0, 1, 1, 0xFFFFFF00u);
    for (int i = 0; i < 4; i++) { nearq[i].z = 0.2f; farq[i].z = 0.8f; }
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_state_default(&rs);
    rs.depth_write = 0;
    renderer_set_state(&rs);
    renderer_draw(&full, RENDERER_DEPTH_TEST, nearq, 4, qi, 6);
    renderer_state_default(&rs);
    renderer_set_state(&rs);
    renderer_draw(&full, RENDERER_DEPTH_TEST, farq, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && (px[c] & 0xFFFFFFu) == 0xFFFF00u, "depth write off: the later far quad passes (%06x)", px[c] & 0xFFFFFFu);

    /* Depth func GREATER: the far quad wins over the near one drawn first. */
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 0.0f);
    renderer_state_default(&rs);
    rs.depth_func = 0x204;
    renderer_set_state(&rs);
    renderer_draw(&full, RENDERER_DEPTH_TEST, nearq, 4, qi, 6);
    renderer_draw(&full, RENDERER_DEPTH_TEST, farq, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && (px[c] & 0xFFFFFFu) == 0xFFFF00u, "depth func GREATER keeps the farther quad (%06x)", px[c] & 0xFFFFFFu);

    /* Culling (D3DRS_CULLMODE): quad() is clockwise on screen. */
    static const struct { uint32_t cull; int cw_drawn; } cu[] = {{0, 1}, {0x900, 0}, {0x901, 1}};
    for (unsigned i = 0; i < sizeof cu / sizeof *cu; i++) {
        for (int ccw = 0; ccw < 2; ccw++) {
            renderer_begin(NULL, 0);
            renderer_clear(RENDERER_CLEAR_COLOR, 0x00000040u, 1.0f);
            renderer_state_default(&rs);
            rs.cull = cu[i].cull;
            renderer_set_state(&rs);
            quad(q, 0, 0, 1, 1, 0xFF00FF00u);
            const uint32_t qccw[6] = {0, 2, 1, 0, 3, 2};
            renderer_draw(&full, 0, q, 4, ccw ? qccw : qi, 6);
            r = renderer_end(px, W * 4);
            int drawn = (px[c] & 0xFFFFFFu) == 0x00FF00u, want = ccw ? cu[i].cull != 0x901 : cu[i].cw_drawn;
            CHECK(r == 0 && drawn == want, "cull mode %x, %s quad %s", cu[i].cull, ccw ? "counterclockwise" : "clockwise",
                  want ? "drawn" : "culled");
        }
    }

    /* Pipeline cache: the same state twice creates one pipeline. */
    renderer_state_default(&rs);
    rs.blend_enable = 1;
    rs.blend_src = 0x300;
    rs.blend_dst = 0x307;
    renderer_begin(NULL, 0);
    renderer_set_state(&rs);
    renderer_draw(&full, 0, q, 4, qi, 6);
    renderer_end(px, W * 4);
    uint32_t n = renderer_pipeline_count();
    renderer_begin(NULL, 0);
    renderer_set_state(&rs);
    renderer_draw(&full, 0, q, 4, qi, 6);
    renderer_draw(&full, 0, q, 4, qi, 6);
    renderer_end(px, W * 4);
    CHECK(n > 0 && renderer_pipeline_count() == n, "pipeline variants are cached (%u, then %u)", n, renderer_pipeline_count());
    RendererTiming tm;
    renderer_last_timing(&tm);
    CHECK(tm.record_ms >= 0 && tm.gpu_ms >= 0 && tm.readback_ms >= 0 && tm.record_ms + tm.gpu_ms + tm.readback_ms < 5000,
          "renderer_end timing split (record %.2f, GPU %.2f, readback %.2f ms)", tm.record_ms, tm.gpu_ms, tm.readback_ms);

    /* renderer_begin resets the state to the default. */
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR, 0x00FF0000u, 1.0f);
    quad(q, 0, 0, 1, 1, 0x0000FF00u);
    renderer_draw(&full, 0, q, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && (px[c] & 0xFFFFFFu) == 0x00FF00u, "renderer_begin resets blending (%06x)", px[c] & 0xFFFFFFu);
}

/* ---- register combiners (renderer_set_pixel_shader) ---- */
enum { ZERO = 0, C0 = 1, C1 = 2, FOG = 3, V0 = 4, V1 = 5, T0 = 8, T1 = 9, R0 = 12, R1 = 13, SUM = 14, EF = 15 };
enum { UID = 0, UINV = 1, EXPN = 2, SID = 6, SNEG = 7 };
#define ALPHA 0x10u
#define IN(reg, map) ((uint32_t)(reg) | (uint32_t)(map) << 5)
#define INS(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define OUT(ab, cd, sum, flags) ((uint32_t)(flags) << 12 | (uint32_t)(sum) << 8 | (uint32_t)(ab) << 4 | (uint32_t)(cd))
#define CD_DOT 0x1u
#define AB_DOT 0x2u
#define MUX 0x4u
#define SHL1 (2u << 3)
#define AB_BLUE_TO_ALPHA 0x80u
#define ONE IN(ZERO, UINV)
static RPixelShader program(uint32_t stages) {
    RPixelShader ps = {0};
    ps.combiners = stages;
    ps.tex_modes = 1 | 1 << 5 | 1 << 10 | 1 << 15;   /* project2d on every stage */
    ps.fog = 0xFF000000u;
    return ps;
}
/* Draws a full-screen quad with the program and returns the center pixel; shade_fog = the vertex
 * fog amount (1 - fog factor) of its vertices. */
static float shade_fog;
static uint32_t shade(const RPixelShader *ps, uint32_t t0, uint32_t t1, uint32_t diffuse, uint32_t specular, const float *t1coord) {
    static uint32_t px[W * H];
    const RViewport full = {0, 0, W, H, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    quad(q, 0, 0, 1, 1, diffuse);
    for (int i = 0; i < 4; i++) {
        q[i].specular = specular;
        q[i].fog = shade_fog;
        if (t1coord)
            for (int k = 0; k < 4; k++) q[i].tex[1][k] = t1coord[k];
    }
    renderer_begin(NULL, 0);
    renderer_set_stage_texture(0, t0);
    renderer_set_stage_texture(1, t1);
    renderer_set_pixel_shader(ps);
    renderer_draw(&full, 0, q, 4, qi, 6);
    int r = renderer_end(px, W * 4);
    return r ? 0xDEADBEEFu : px[(H / 2) * W + W / 2] & 0xFFFFFFu;
}

static void test_combiners(void) {
    const uint32_t a[4] = {0xFF804020u, 0xFF804020u, 0xFF804020u, 0xFF804020u};
    const uint32_t b[4] = {0x40804020u, 0x40804020u, 0x40804020u, 0x40804020u};
    const uint32_t lr[2] = {0xFFFF0000u, 0xFF0000FFu};   /* 2x1: red left, blue right */
    uint32_t ta = renderer_texture_create(0xC001, RENDERER_TEX_BGRA8, 2, 2, 1, a);
    uint32_t tb = renderer_texture_create(0xC002, RENDERER_TEX_BGRA8, 2, 2, 1, b);
    uint32_t tlr = renderer_texture_create(0xC003, RENDERER_TEX_BGRA8, 2, 1, 1, lr);
    uint32_t px;

    /* The title's one-stage shader: r0 = t0 * v0; final A = fog.a, B = r0, C = fog.rgb, D = 0, G = r0.a. */
    RPixelShader ps = program(1);
    ps.rgb_in[0] = INS(IN(T0, UID), IN(V0, UID), IN(ZERO, UID), IN(ZERO, UID));
    ps.alpha_in[0] = INS(IN(T0, UID) | ALPHA, IN(V0, UID) | ALPHA, ALPHA, ALPHA);
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.alpha_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.final_abcd = INS(IN(FOG, UID) | ALPHA, IN(R0, UID), IN(FOG, UID), IN(ZERO, UID));
    ps.final_efg = INS(0, 0, IN(R0, UID) | ALPHA, 0x80);
    px = shade(&ps, ta, 0, 0xFF808080u, 0, NULL);
    CHECK(dist(px, 0x402010u) <= 2, "t0 x v0 through the final combiner, fog factor 1 (%06x, want ~402010)", px);
    ps.fog = 0x80FF0000u;   /* fog factor 0.5 toward red */
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(dist(px, 0xC02010u) <= 2, "final combiner mix(fog, r0, fog.a) (%06x, want ~c02010)", px);
    CHECK(renderer_pixel_shader_count() == 2, "programs are deduplicated per frame (%u)", renderer_pixel_shader_count());
    /* Programs are compiled per structure (specialization constants): a new structure adds pipeline
     * variants, a program that only differs in its constants (here the fog color) does not. */
    uint32_t pipes = renderer_pipeline_count();
    ps.fog = 0x40FF0000u;
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(renderer_pipeline_count() == pipes, "same structure, other constants: no new pipeline (%u, then %u)", pipes,
          renderer_pipeline_count());
    ps.fog = 0x80FF0000u;
    ps.final_efg = INS(0, 0, IN(R0, UID) | ALPHA, 0);   /* no V1+R0 clamp flag: another structure */
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(renderer_pipeline_count() > pipes && dist(px, 0xC02010u) <= 2, "new structure: its own pipeline (%u, then %u; %06x)",
          pipes, renderer_pipeline_count(), px);
    ps.final_efg = INS(0, 0, IN(R0, UID) | ALPHA, 0x80);
    /* Per-vertex fog (D3DRS_FOGENABLE: factor per vertex, interpolated): amount 0.5 toward red. */
    ps.fog = 0xFFFF0000u;
    shade_fog = 0.5f;
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(dist(px, 0xBF2010u) <= 2, "vertex fog amount 0.5 mixes r0 with the fog color (%06x, want ~bf2010)", px);
    shade_fog = 1;
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(dist(px, 0xFF0000u) <= 2, "vertex fog amount 1 gives the fog color (%06x)", px);
    shade_fog = 0;

    /* Mux on r0.a (MSB): r0.a = v1.a; r1 = r0.a >= 0.5 ? c0 (CD) : c1 (AB); r0.rgb = r1. */
    ps = program(3 | 0x100);
    ps.alpha_in[0] = INS(IN(V1, SID) | ALPHA, ONE | ALPHA, ALPHA, ALPHA);
    ps.alpha_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.rgb_in[1] = INS(IN(C1, UID), ONE, IN(C0, UID), ONE);
    ps.rgb_out[1] = OUT(ZERO, ZERO, R1, MUX);
    ps.c0[1] = 0xFF00FF00u;
    ps.c1[1] = 0xFFFF0000u;
    ps.rgb_in[2] = INS(IN(R1, UID), ONE, 0, 0);
    ps.rgb_out[2] = OUT(R0, ZERO, ZERO, 0);
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0xFF000000u, NULL);
    CHECK(px == 0x00FF00u, "mux with r0.a = 1 selects CD = c0 (%06x)", px);
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0x00000000u, NULL);
    CHECK(px == 0xFF0000u, "mux with r0.a = 0 selects AB = c1 (%06x)", px);

    /* Input and output mappings: r0 = (expand(t0) + c0) * 2: t0 = (0.502, 0.251, 0.125), c0 = 0.502. */
    ps = program(1);
    ps.rgb_in[0] = INS(IN(T0, EXPN), ONE, IN(C0, UID), ONE);
    ps.rgb_out[0] = OUT(ZERO, ZERO, R0, SHL1);
    ps.c0[0] = 0xFF808080u;
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(dist(px, 0xFF0000u) <= 3, "expand-normal input, sum, shift-left output, clamp (%06x, want ~ff0000)", px);

    /* Dot product AB: dot(v0, c0) = 3 * 0.251 * 0.502 = 0.378, replicated. */
    ps = program(1);
    ps.rgb_in[0] = INS(IN(V0, UID), IN(C0, UID), 0, 0);
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, AB_DOT);
    ps.c0[0] = 0xFF808080u;
    px = shade(&ps, ta, 0, 0xFF404040u, 0, NULL);
    CHECK(dist(px, 0x606060u) <= 2, "AB dot product (%06x, want ~606060)", px);

    /* Texture mode none: t0 = 0. */
    ps = program(1);
    ps.tex_modes = 0;
    ps.rgb_in[0] = INS(IN(T0, UID), ONE, 0, 0);
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(px == 0, "texture mode none samples 0 (%06x)", px);

    /* Stage 1 texture with a projective coordinate (1.5, 0.5, 0, 2) -> (0.75, 0.25): the blue texel. */
    const float proj[4] = {1.5f, 0.5f, 0, 2};
    ps = program(1);
    ps.rgb_in[0] = INS(IN(T1, UID), ONE, 0, 0);
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    px = shade(&ps, ta, tlr, 0xFFFFFFFFu, 0, proj);
    CHECK(px == 0x0000FFu, "stage 1 texture, project2d divides by q (%06x)", px);
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, proj);
    CHECK(px == 0xFFFFFFu, "an unbound stage samples white (%06x)", px);

    /* Final combiner: D = E * F with E = v0 (0.5 grey), F = final c0 (red); then D = v1 + r0 clamped. */
    ps = program(1);
    ps.rgb_in[0] = INS(IN(T0, UID), ONE, 0, 0);
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.final_abcd = INS(0, 0, 0, IN(EF, UID));
    ps.final_efg = INS(IN(V0, UID), IN(C0, UID), 0, 0);
    ps.final_c0 = 0xFFFF0000u;
    px = shade(&ps, ta, 0, 0xFF808080u, 0, NULL);
    CHECK(dist(px, 0x800000u) <= 2, "final E x F product (%06x, want ~800000)", px);
    ps.final_abcd = INS(0, 0, 0, IN(SUM, UID));
    ps.final_efg = INS(0, 0, 0, 0x80);
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0x00800000u, NULL);
    CHECK(dist(px, 0xFF4020u) <= 2, "final v1 + r0 sum, clamped (%06x, want ~ff4020)", px);

    /* Alpha portion: r0.a = t0.a * v0.a (0.25), shown through the final combiner as D = r0.a. */
    ps = program(1);
    ps.alpha_in[0] = INS(IN(T0, UID) | ALPHA, IN(V0, UID) | ALPHA, ALPHA, ALPHA);
    ps.alpha_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.final_abcd = INS(0, 0, 0, IN(R0, UID) | ALPHA);
    px = shade(&ps, tb, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(dist(px, 0x404040u) <= 2, "alpha portion t0.a x v0.a (%06x, want ~404040)", px);
    /* AB blue to alpha: r0 = t0 with r0.a = t0.b (0.125). */
    ps = program(1);
    ps.rgb_in[0] = INS(IN(T0, UID), ONE, 0, 0);
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, AB_BLUE_TO_ALPHA);
    ps.final_abcd = INS(0, 0, 0, IN(R0, UID) | ALPHA);
    px = shade(&ps, ta, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(dist(px, 0x202020u) <= 2, "AB blue to alpha (%06x, want ~202020)", px);
    /* Without stages and a final combiner the output is r0 = (0, 0, 0, t0.a). */
    ps = program(0);
    px = shade(&ps, tb, 0, 0xFFFFFFFFu, 0, NULL);
    CHECK(px == 0, "zero stages: output r0 = (0, 0, 0, t0.a) (%06x)", px);
}


/* ---- offscreen surfaces (title render targets) and depth-texture compare ----
 * A D16 surface rendered in its own pass before the frame's main pass, then sampled by a stage
 * whose texture is that surface: PROJECT3D compares texel x 0xFFFF <func> clamp(r/q, 0, 0xFFFF)
 * (NV2A shadow compare, xemu psh.c psh_append_shadowmap; func = NV097_SET_SHADOW_DEPTH_FUNC
 * value), PROJECT2D compares the texel with 0, outside the texture (border) reads as depth 1. */
static uint32_t shadow_shade(uint32_t surf, uint32_t mode, uint32_t func, float u, float r_ref) {
    RPixelShader ps = program(1);
    ps.rgb_in[0] = INS(IN(T1, UID), ONE, IN(ZERO, UID), IN(ZERO, UID));
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.tex_modes = 1 | mode << 5;
    ps.shadow_func = func;
    const float t1[4] = {u * 2, 0.5f * 2, r_ref * 2, 2};   /* q = 2: s/q, t/q, r/q */
    return shade(&ps, 0, surf, 0xFFFFFFFFu, 0, t1);
}
static void test_surfaces(void) {
    enum { SW2 = 32, SH2 = 16 };
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    uint32_t d = renderer_surface(0x5001, RENDERER_SURFACE_DEPTH16, SW2, SH2);
    CHECK(d != 0 && renderer_surface(0x5001, RENDERER_SURFACE_DEPTH16, SW2, SH2) == d && renderer_surface_count() >= 1,
          "depth surface created once per key (%u)", d);
    int sw = 0, sh = 0;
    renderer_surface_size(d, &sw, &sh);
    CHECK(sw == SW2 && sh == SH2, "surface at the guest size without output scaling (%dx%d)", sw, sh);
    /* A new surface holds depth 1.0: every compare with a smaller reference is lit. */
    uint32_t px = shadow_shade(d, 2, 4, 0.5f, 30000);
    CHECK(px == 0xFFFFFFu, "new depth surface reads 1.0: GREATER than 30000 -> 1 (%06x)", px);

    /* Depth pass: the left half of the surface at depth 0.25 (16384 in D16 units). */
    static uint32_t frame[W * H];
    const RViewport svp = {0, 0, SW2, SH2, 0, 1}, full = {0, 0, W, H, 0, 1};
    RVertex left[4];
    quad(left, 0, 0, 1, 1, 0xFF0000FFu);
    for (int i = 0; i < 4; i++) { left[i].x = left[i].x < 0 ? -1.0f : 0.0f; left[i].z = 0.25f; }
    RState st;
    renderer_state_default(&st);
    st.color_mask = 0;
    renderer_begin(NULL, 0);
    renderer_set_target(0, d);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_set_state(&st);
    renderer_draw(&svp, RENDERER_DEPTH_TEST, left, 4, qi, 6);
    renderer_set_target(0, 0);
    renderer_state_default(&st);
    renderer_set_state(&st);
    RPixelShader ps = program(1);
    ps.rgb_in[0] = INS(IN(T1, UID), ONE, IN(ZERO, UID), IN(ZERO, UID));
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.tex_modes = 1 | 2 << 5;
    ps.shadow_func = 4;   /* GREATER: lit where the stored depth is beyond the reference */
    RVertex q[4];
    quad(q, 0, 0, 1, 1, 0xFFFFFFFFu);
    for (int i = 0; i < 4; i++) q[i].tex[1][2] = 30000;   /* r/q */
    renderer_set_stage_texture(1, d);
    renderer_set_pixel_shader(&ps);
    renderer_draw(&full, 0, q, 4, qi, 6);
    int r = renderer_end(frame, W * 4);
    uint32_t pl = frame[(H / 2) * W + W / 8] & 0xFFFFFFu, pr = frame[(H / 2) * W + W * 7 / 8] & 0xFFFFFFu;
    CHECK(r == 0 && pl == 0 && pr == 0xFFFFFFu, "rendered depth pass sampled in the same frame: caster half shadowed (%06x), other lit (%06x)",
          pl, pr);
    CHECK(shadow_shade(d, 2, 4, 0.25f, 10000) == 0xFFFFFFu, "stored 16384 GREATER reference 10000 -> lit");
    CHECK(shadow_shade(d, 2, 1, 0.25f, 30000) == 0xFFFFFFu, "func LESS: stored 16384 < 30000 -> 1");
    CHECK(shadow_shade(d, 2, 0, 0.75f, 0) == 0 && shadow_shade(d, 2, 7, 0.25f, 65535) == 0xFFFFFFu, "func NEVER -> 0, ALWAYS -> 1");
    CHECK(shadow_shade(d, 2, 4, 0.25f, 1e9f) == 0, "reference clamped to 0xFFFF: 16384 > 65535 is false -> 0");
    CHECK(shadow_shade(d, 1, 4, 0.25f, 30000) == 0xFFFFFFu && shadow_shade(d, 1, 1, 0.75f, 0) == 0,
          "PROJECT2D compares the texel with 0 (GREATER -> 1, LESS -> 0)");
    CHECK(shadow_shade(d, 2, 4, -0.5f, 30000) == 0xFFFFFFu && shadow_shade(d, 2, 4, 1.5f, 30000) == 0xFFFFFFu,
          "outside the surface (border) reads as depth 1: lit");
    /* The depth pass did not touch the frame target. */
    renderer_begin(NULL, 0);
    renderer_set_target(0, d);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 0.0f);
    renderer_set_target(0, 0);
    r = renderer_end(frame, W * 4);
    uint32_t first = 0;
    CHECK(r == 0 && mismatches(frame, 0, &first) == 0, "offscreen clears leave the frame target alone");
    CHECK(shadow_shade(d, 2, 4, 0.75f, 100) == 0, "surface content persists across frames (cleared to 0: 0 > 100 false)");

    /* Color surface: render red into it, sample it in the main pass. */
    uint32_t c = renderer_surface(0x5002, RENDERER_SURFACE_COLOR, SW2, SH2);
    RVertex red[4];
    quad(red, 0, 0, 1, 1, 0xFFFF0000u);
    renderer_begin(NULL, 0);
    renderer_set_target(c, 0);
    renderer_clear(RENDERER_CLEAR_COLOR, 0x000000FFu, 1.0f);
    renderer_draw(&svp, 0, red, 4, qi, 6);
    renderer_set_target(0, 0);
    renderer_set_stage_texture(0, c);
    renderer_draw(&full, 0, q, 4, qi, 6);
    r = renderer_end(frame, W * 4);
    CHECK(r == 0 && c != 0 && (frame[(H / 2) * W + W / 2] & 0xFFFFFFu) == 0xFF0000u, "color surface rendered then sampled (%06x)",
          frame[(H / 2) * W + W / 2] & 0xFFFFFFu);
}

/* Output scaling, MSAA and anisotropic filtering: a renderer configured for a 2W x 2H target
 * (the caller still records W x H viewports), then one with 4 samples. */
static void test_scaled(void) {
    enum { SW = W * 2, SH = H * 2 };
    static uint32_t px[SW * SH], base[W * H];
    const RViewport full = {0, 0, W, H, 0, 1}, sub = {W / 2, 0, W / 2, H / 2, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RendererConfig c = {.out_w = SW, .out_h = SH, .samples = 1};
    renderer_configure(&c);
    if (renderer_init(W, H)) {
        CHECK(0, "renderer_init at %dx%d: %s", SW, SH, renderer_error());
        return;
    }
    int ow = 0, oh = 0;
    renderer_output_size(&ow, &oh);
    CHECK(ow == SW && oh == SH, "output size %dx%d (want %dx%d)", ow, oh, SW, SH);
    int sw = 0, sh = 0;
    renderer_surface_size(renderer_surface(0x5003, RENDERER_SURFACE_DEPTH16, 32, 16), &sw, &sh);
    CHECK(sw == 64 && sh == 32, "offscreen surfaces scale with the output height (%dx%d)", sw, sh);

    /* The W x H base image is stretched over the whole target: left half red, right half blue. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) base[y * W + x] = x < W / 2 ? 0x00FF0000u : 0x000000FFu;
    renderer_begin(base, W * 4);
    int r = renderer_end(px, SW * 4);
    CHECK(r == 0 && px[4 * SW + 4] == 0x00FF0000u && px[(SH - 5) * SW + SW / 2 - 4] == 0x00FF0000u &&
              px[4 * SW + SW - 5] == 0x000000FFu && px[(SH - 5) * SW + SW / 2 + 4] == 0x000000FFu,
          "base image scaled to the target (%08x %08x %08x %08x)", px[4 * SW + 4], px[(SH - 5) * SW + SW / 2 - 4], px[4 * SW + SW - 5],
          px[(SH - 5) * SW + SW / 2 + 4]);

    /* Viewports are scaled with the target: the top-right quarter of the W x H frame. */
    quad(q, 0, 0, 1, 1, 0xFF00FF00u);
    renderer_begin(NULL, 0);
    renderer_draw(&sub, 0, q, 4, qi, 6);
    r = renderer_end(px, SW * 4);
    int inside = 0, outside = 0;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            int in = x >= SW / 2 && y < SH / 2;
            if (in) inside += px[y * SW + x] == 0x0000FF00u;
            else outside += px[y * SW + x] != 0;
        }
    CHECK(r == 0 && inside == SW * SH / 4 && outside == 0, "scaled viewport covers exactly the top-right quarter (%d of %d, %d outside)",
          inside, SW * SH / 4, outside);

    /* No MSAA: a diagonal edge has only the two flat colors. */
    const RVertex tri[3] = {{-1, -1, 0.5f, 1, 0xFFFFFFFFu, 0, {{0}}, 0}, {1, -1, 0.5f, 1, 0xFFFFFFFFu, 0, {{0}}, 0},
                            {1, 0.9f, 0.5f, 1, 0xFFFFFFFFu, 0, {{0}}, 0}};
    const uint32_t ti[3] = {0, 1, 2};
    renderer_begin(NULL, 0);
    renderer_draw(&full, 0, tri, 3, ti, 3);
    r = renderer_end(px, SW * 4);
    int mid = 0;
    for (int i = 0; i < SW * SH; i++) mid += px[i] != 0 && px[i] != 0x00FFFFFFu;
    CHECK(r == 0 && mid == 0 && renderer_samples() == 1, "1 sample: no intermediate edge pixels (%d)", mid);
    renderer_shutdown();

    /* 4 samples (when the device has them): the same edge is resolved to intermediate values. */
    c.samples = 4;
    c.aniso = 16;
    renderer_configure(&c);
    if (renderer_init(W, H)) {
        CHECK(0, "renderer_init with MSAA: %s", renderer_error());
        return;
    }
    renderer_begin(NULL, 0);
    renderer_draw(&full, 0, tri, 3, ti, 3);
    r = renderer_end(px, SW * 4);
    mid = 0;
    for (int i = 0; i < SW * SH; i++) mid += px[i] != 0 && px[i] != 0x00FFFFFFu;
    printf("     samples %d, anisotropy %.0f\n", renderer_samples(), renderer_anisotropy());
    CHECK(r == 0 && (renderer_samples() == 1 || mid > SW / 2), "%d samples: %d intermediate edge pixels", renderer_samples(), mid);
    /* The base image still reaches the multisampled target. */
    renderer_begin(base, W * 4);
    r = renderer_end(px, SW * 4);
    CHECK(r == 0 && px[4 * SW + 4] == 0x00FF0000u && px[4 * SW + SW - 5] == 0x000000FFu, "base image with MSAA (%08x %08x)", px[4 * SW + 4],
          px[4 * SW + SW - 5]);
    renderer_shutdown();
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* Glass operations (renderer_glass) on the copy path, the scaled path and with MSAA: over a base
 * of one-pixel black/white stripes, a frame with a glass rect (no tint, no rim, square corners)
 * differs from the same frame without it only inside the rect, where the stripes are blurred to
 * gray; a draw recorded after the glass op is still drawn (the pass resumes); a full tint gives
 * the tint color; a glass op while an offscreen target is set is ignored. */
static void test_glass(void) {
    static uint32_t base[W * H], a[W * H * 4], b[W * H * 4];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) base[y * W + x] = x & 1 ? 0x00FFFFFFu : 0;
    const RViewport corner = {W - 8, 0, 8, 8, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    quad(q, 0, 0, 1, 1, 0xFFFF0000u);
    const RGlass g = {8, 8, 40, 24, 3, 0, 0x00000000u, 0x00000000u};
    /* The last one supersampled: rendered at 2W x 2H, read back at W x H. */
    const struct { int scale, samples, ss; } cfgs[] = {{1, 1, 1}, {2, 1, 1}, {2, 4, 1}, {1, 1, 2}};
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sc = cfgs[k].scale, OW = W * sc, OH = H * sc;
        RendererConfig c = {.out_w = OW, .out_h = OH, .samples = cfgs[k].samples, .glass = 1, .supersample = cfgs[k].ss};
        renderer_configure(&c);
        if (renderer_init(W, H)) {
            CHECK(0, "renderer_init for glass (%dx, %d samples): %s", sc, cfgs[k].samples, renderer_error());
            continue;
        }
        renderer_begin(base, W * 4);
        renderer_draw(&corner, 0, q, 4, qi, 6);
        int r = renderer_end(a, OW * 4);
        CHECK(r == 0 && renderer_glass_count() == 0, "glass %dx/%d: frame without glass", sc, cfgs[k].samples);
        renderer_begin(base, W * 4);
        renderer_glass(&g);
        renderer_draw(&corner, 0, q, 4, qi, 6);
        r = renderer_end(b, OW * 4);
        int outside_diff = 0, inside_gray = 0, inside = 0;
        for (int y = 0; y < OH; y++)
            for (int x = 0; x < OW; x++) {
                const int in = x >= g.x0 * sc && x < g.x1 * sc && y >= g.y0 * sc && y < g.y1 * sc;
                if (!in) outside_diff += a[y * OW + x] != b[y * OW + x];
                else {
                    inside++;
                    inside_gray += dist(b[y * OW + x], 0x00808080u) <= 24;
                }
            }
        CHECK(r == 0 && renderer_glass_count() == 1 && outside_diff == 0, "glass %dx/%d: only the rect changes (%d pixels outside differ)", sc,
              cfgs[k].samples, outside_diff);
        CHECK(inside_gray == inside, "glass %dx/%d: stripes blurred to gray inside (%d of %d)", sc, cfgs[k].samples, inside_gray, inside);
        CHECK(b[2 * sc * OW + OW - 3] == 0x00FF0000u, "glass %dx/%d: a draw after the glass op is drawn (%08x)", sc, cfgs[k].samples,
              b[2 * sc * OW + OW - 3]);
        /* Frost (--hud-hd-frost): square corners, no tint, no rim over a flat gray: nothing visible
         * changes, the corner and edge pixels included. */
        static uint32_t flat[W * H];
        for (int i = 0; i < W * H; i++) flat[i] = 0x00606060u;
        const RGlass fr = {8.5f, 8.25f, 40.5f, 24.75f, 3, 0, 0, 0};
        renderer_begin(flat, W * 4);
        renderer_glass(&fr);
        r = renderer_end(b, OW * 4);
        int off = 0;
        for (int i = 0; i < OW * OH; i++) off += dist(b[i], 0x00606060u) > 1;
        CHECK(r == 0 && renderer_glass_count() == 1 && off == 0, "glass %dx/%d: frost op is invisible on flat gray (%d pixels off)", sc,
              cfgs[k].samples, off);
        RGlass t = g;
        t.tint = 0xFF00FF00u;
        renderer_begin(base, W * 4);
        renderer_glass(&t);
        r = renderer_end(b, OW * 4);
        const uint32_t mid = b[(16 * sc) * OW + 24 * sc];
        CHECK(r == 0 && dist(mid, 0x0000FF00u) <= 2, "glass %dx/%d: full tint (%08x)", sc, cfgs[k].samples, mid);
        renderer_begin(base, W * 4);
        renderer_set_target(renderer_surface(0x6001, RENDERER_SURFACE_COLOR, 16, 16), 0);
        renderer_glass(&g);
        renderer_set_target(0, 0);
        r = renderer_end(b, OW * 4);
        CHECK(r == 0 && renderer_glass_count() == 0 && !memcmp(a + 0, b + 0, 4) , "glass %dx/%d: ignored on an offscreen target", sc,
              cfgs[k].samples);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* Shadow edge of a depth pass (left half of a 32x16 D16 surface at depth 0.25) sampled with
 * GREATER 30000 by a full-frame receiver (u 0.25-0.75: the edge in the middle), with renderer configuration c: the middle row of the
 * W x H frame (0 = shadowed, 0xFFFFFF = lit) and the surface's actual size. Returns 0 on success. */
static int shadow_edge(const RendererConfig *c, uint32_t row[W], int *sw, int *sh) {
    static uint32_t frame[W * H];
    renderer_configure(c);
    if (renderer_init(W, H)) return -1;
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    uint32_t d = renderer_surface(0x7001, RENDERER_SURFACE_DEPTH16, 32, 16);
    renderer_surface_size(d, sw, sh);
    const RViewport svp = {0, 0, 32, 16, 0, 1}, full = {0, 0, W, H, 0, 1};
    RVertex left[4], q[4];
    quad(left, 0, 0, 1, 1, 0xFF0000FFu);
    for (int i = 0; i < 4; i++) { left[i].x = left[i].x < 0 ? -1.0f : 0.0f; left[i].z = 0.25f; }
    RState st;
    renderer_state_default(&st);
    st.color_mask = 0;
    renderer_begin(NULL, 0);
    renderer_set_target(0, d);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_set_state(&st);
    renderer_draw(&svp, RENDERER_DEPTH_TEST, left, 4, qi, 6);
    renderer_set_target(0, 0);
    renderer_state_default(&st);
    renderer_set_state(&st);
    RPixelShader ps = program(1);
    ps.rgb_in[0] = INS(IN(T1, UID), ONE, IN(ZERO, UID), IN(ZERO, UID));
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.tex_modes = 1 | 2 << 5;
    ps.shadow_func = 4;
    quad(q, 0.25f, 0, 0.75f, 1, 0xFFFFFFFFu);   /* 16 texels over W pixels, away from the border */
    for (int i = 0; i < 4; i++) q[i].tex[1][2] = 30000;
    renderer_set_stage_texture(1, d);
    renderer_set_pixel_shader(&ps);
    renderer_draw(&full, 0, q, 4, qi, 6);
    int r = renderer_end(frame, W * 4);
    for (int x = 0; x < W; x++) row[x] = frame[(H / 2) * W + x] & 0xFFFFFFu;
    renderer_shutdown();
    return r;
}
/* Pixels of a shadow-edge row strictly between shadowed and lit; -1 when the row is not a clean
 * ramp (not shadowed at the left end, not lit at the right end, or not non-decreasing). */
static int ramp_width(const uint32_t row[W]) {
    int n = 0;
    if (row[0] != 0 || row[W - 1] != 0xFFFFFFu) return -1;
    for (int x = 0; x < W; x++) {
        if ((row[x] & 0xFF) != ((row[x] >> 8) & 0xFF) || (row[x] & 0xFF) != ((row[x] >> 16) & 0xFF)) return -1;
        if (x && (row[x] & 0xFF) < (row[x - 1] & 0xFF)) return -1;
        n += row[x] != 0 && row[x] != 0xFFFFFFu;
    }
    return n;
}

/* --shadow-scale / --shadow-soft (RendererConfig.shadow_scale, shadow_soft): surfaces at N x the
 * output-scaled size, a finer edge; soft filtering widens the edge into a smooth ramp. */
static void test_shadow_quality(void) {
    uint32_t hard[W], scaled[W], soft[W];
    int sw = 0, sh = 0;
    RendererConfig c = {.samples = 1};
    int r = shadow_edge(&c, hard, &sw, &sh);
    const int n_hard = ramp_width(hard);
    CHECK(r == 0 && sw == 32 && sh == 16 && n_hard > 0, "default: surface 32x16 (%dx%d), bilinear edge %d pixels wide", sw, sh, n_hard);
    c.shadow_scale = 2;
    r = shadow_edge(&c, scaled, &sw, &sh);
    const int n_scaled = ramp_width(scaled);
    CHECK(r == 0 && sw == 64 && sh == 32, "shadow scale 2: surface 64x32 (%dx%d)", sw, sh);
    CHECK(n_scaled >= 0 && n_scaled < n_hard, "shadow scale 2: sharper edge (%d < %d pixels)", n_scaled, n_hard);
    c.shadow_scale = 0;
    c.shadow_soft = 1;
    r = shadow_edge(&c, soft, &sw, &sh);
    const int n_soft = ramp_width(soft);
    CHECK(r == 0 && sw == 32 && n_soft > n_hard, "soft: wider smooth edge (%d > %d pixels, a monotonic ramp)", n_soft, n_hard);
    int levels = 0;
    for (int x = 1; x < W; x++) levels += soft[x] != soft[x - 1];
    CHECK(levels > 3, "soft: the edge has more than three steps (%d)", levels);
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* --shadow-range K (RendererConfig.shadow_range): with the light projection of the draw
 * (RPixelShader.shadow_depth: r = A q - B), a depth stage's compare also counts as lit where the
 * receiver lies more than K view units behind the stored caster depth, fading in between K and
 * 1.5 K; nearer receivers are shadowed exactly as without it. */
static uint32_t range_shade(uint32_t surf, uint32_t func, float u, float ref, float a, float b) {
    RPixelShader ps = program(1);
    ps.rgb_in[0] = INS(IN(T1, UID), ONE, IN(ZERO, UID), IN(ZERO, UID));
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.tex_modes = 1 | 2 << 5;
    ps.shadow_func = func;
    ps.shadow_depth[0] = a;
    ps.shadow_depth[1] = b;
    const float t1[4] = {u * 2, 0.5f * 2, ref * 2, 2};
    return shade(&ps, 0, surf, 0xFFFFFFFFu, 0, t1);
}
/* A 32x16 D16 surface whose left half holds a caster at depth 0.25 (16384). */
static uint32_t range_caster(void) {
    static uint32_t frame[W * H];
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    uint32_t d = renderer_surface(0x7101, RENDERER_SURFACE_DEPTH16, 32, 16);
    const RViewport svp = {0, 0, 32, 16, 0, 1};
    RVertex left[4];
    quad(left, 0, 0, 1, 1, 0xFF0000FFu);
    for (int i = 0; i < 4; i++) { left[i].x = left[i].x < 0 ? -1.0f : 0.0f; left[i].z = 0.25f; }
    RState st;
    renderer_state_default(&st);
    st.color_mask = 0;
    renderer_begin(NULL, 0);
    renderer_set_target(0, d);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_set_state(&st);
    renderer_draw(&svp, RENDERER_DEPTH_TEST, left, 4, qi, 6);
    renderer_set_target(0, 0);
    renderer_state_default(&st);
    renderer_set_state(&st);
    renderer_end(frame, W * 4);
    return d;
}
static void test_shadow_range(void) {
    /* A light with near 10 and far 110 view units: depth = A - B / q (D16 units). */
    const double A = 65535.0 * 110.0 / 100.0, B = A * 10.0;
    const double qc = B / (A - 16384.0);   /* the caster at stored depth 16384 */
    const float K = 2.0f;
    for (int soft = 0; soft < 2; soft++) {
        RendererConfig c = {.samples = 1, .shadow_range = K, .shadow_soft = soft};
        renderer_configure(&c);
        if (renderer_init(W, H)) {
            CHECK(0, "renderer_init with shadow range: %s", renderer_error());
            continue;
        }
        uint32_t d = range_caster();
#define REF(dq) (float)(A - B / (qc + (dq)))
        const uint32_t near = range_shade(d, 4, 0.25f, REF(1.0), (float)A, (float)B);
        const uint32_t mid = range_shade(d, 4, 0.25f, REF(2.5), (float)A, (float)B);
        const uint32_t far = range_shade(d, 4, 0.25f, REF(3.5), (float)A, (float)B);
        CHECK(near == 0, "soft %d: receiver 1 unit behind the caster stays shadowed (%06x)", soft, near);
        CHECK((mid & 0xFF) > 0x60 && (mid & 0xFF) < 0xA0, "soft %d: 1.25 K behind fades half way (%06x)", soft, mid);
        CHECK(far == 0xFFFFFFu, "soft %d: beyond 1.5 K behind the caster is lit (%06x)", soft, far);
        CHECK(range_shade(d, 6, 0.25f, REF(3.5), (float)A, (float)B) == 0xFFFFFFu, "soft %d: GEQUAL follows the same range", soft);
        CHECK(range_shade(d, 4, 0.25f, REF(3.5), 0, 0) == 0, "soft %d: no light projection (A = 0): the original compare", soft);
        CHECK(range_shade(d, 4, 0.25f, REF(-1.0), (float)A, (float)B) == 0xFFFFFFu, "soft %d: receiver in front of the caster is lit", soft);
        CHECK(range_shade(d, 4, 0.75f, REF(3.5), (float)A, (float)B) == 0xFFFFFFu, "soft %d: no caster (depth 1.0): lit", soft);
        renderer_shutdown();
    }
    /* Off (K = 0): the same receiver far behind stays shadowed, as the title draws it. */
    RendererConfig off = {.samples = 1};
    renderer_configure(&off);
    if (!renderer_init(W, H)) {
        uint32_t d = range_caster();
        CHECK(range_shade(d, 4, 0.25f, REF(3.5), (float)A, (float)B) == 0, "off: receiver far behind the caster shadowed (original)");
#undef REF
        renderer_shutdown();
    }
}

/* --supersample N (RendererConfig.supersample): rendered at N x the output size, read back (and
 * presented) at the output size after a box downsample. */
static void test_supersample(void) {
    static uint32_t px[W * 2 * H * 2], base[W * H];
    const RViewport full = {0, 0, W, H, 0, 1};
    const RVertex tri[3] = {{-1, -1, 0.5f, 1, 0xFFFFFFFFu, 0, {{0}}, 0}, {1, -1, 0.5f, 1, 0xFFFFFFFFu, 0, {{0}}, 0},
                            {1, 0.9f, 0.5f, 1, 0xFFFFFFFFu, 0, {{0}}, 0}};
    const uint32_t ti[3] = {0, 1, 2};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) base[y * W + x] = (uint32_t)(x * 4) << 16 | (uint32_t)(y * 5) << 8 | ((x ^ y) & 1 ? 0xF0 : 0x0F);
    const struct { int ss, samples, out; } cfgs[] = {{2, 1, 1}, {4, 4, 1}, {2, 1, 2}};
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int OW = W * cfgs[k].out, OH = H * cfgs[k].out;
        RendererConfig c = {.out_w = OW, .out_h = OH, .samples = cfgs[k].samples, .supersample = cfgs[k].ss};
        renderer_configure(&c);
        if (renderer_init(W, H)) {
            CHECK(0, "renderer_init with supersample %d: %s", cfgs[k].ss, renderer_error());
            continue;
        }
        int ow = 0, oh = 0, sw = 0, sh = 0;
        renderer_output_size(&ow, &oh);
        CHECK(ow == OW && oh == OH && renderer_supersample() == cfgs[k].ss, "supersample %d (out %dx): output %dx%d, factor %d", cfgs[k].ss,
              cfgs[k].out, ow, oh, renderer_supersample());
        renderer_surface_size(renderer_surface(0x7002, RENDERER_SURFACE_DEPTH16, 32, 16), &sw, &sh);
        CHECK(sw == 32 * cfgs[k].out * cfgs[k].ss && sh == 16 * cfgs[k].out * cfgs[k].ss,
              "supersample %d: surfaces follow the internal height (%dx%d)", cfgs[k].ss, sw, sh);
        renderer_begin(NULL, 0);
        renderer_clear(RENDERER_CLEAR_COLOR, 0x00336699u, 1.0f);
        int r = renderer_end(px, OW * 4), bad = 0;
        for (int i = 0; i < OW * OH; i++) bad += px[i] != 0x00336699u;
        CHECK(r == 0 && bad == 0, "supersample %d: a clear reads back exactly (%d mismatches)", cfgs[k].ss, bad);
        renderer_begin(base, W * 4);
        r = renderer_end(px, OW * 4);
        bad = 0;
        if (cfgs[k].out == 1) {
            for (int i = 0; i < W * H; i++) bad += px[i] != base[i];
            CHECK(r == 0 && bad == 0, "supersample %d: base image at the output size reads back unchanged (%d mismatches)", cfgs[k].ss, bad);
        } else {
            const uint32_t got = px[(OH / 2) * OW + OW / 2], want = base[(H / 2) * W + W / 2];
            CHECK(r == 0 && dist(got & 0xFFFF00u, want & 0xFFFF00u) < 8, "supersample %d, scaled output: base image stretched (%06x, %06x)",
                  cfgs[k].ss, got, want);
        }
        renderer_begin(NULL, 0);
        renderer_draw(&full, 0, tri, 3, ti, 3);
        r = renderer_end(px, OW * 4);
        int mid = 0;
        for (int i = 0; i < OW * OH; i++) mid += px[i] != 0 && px[i] != 0x00FFFFFFu;
        CHECK(r == 0 && mid > OW / 2, "supersample %d: the edge is antialiased (%d intermediate pixels)", cfgs[k].ss, mid);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* Direct present without a window: a surface callback that fails leaves the renderer offscreen
 * (renderer_end still works, renderer_present refuses) and renderer_present_info says why. */
static int surface_calls;
static int failing_surface(void *instance, uint64_t *surface) {
    (void)instance;
    (void)surface;
    surface_calls++;
    return -1;
}
static void test_present_fallback(void) {
    static uint32_t px[W * H];
    static const char *ext[] = {"VK_KHR_surface"};
    RendererConfig c = {.samples = 1, .instance_ext = ext, .instance_ext_count = 1, .create_surface = failing_surface};
    renderer_configure(&c);
    if (renderer_init(W, H)) {
        CHECK(0, "renderer_init with a failing surface callback: %s", renderer_error());
        return;
    }
    const char *why = renderer_present_info();
    CHECK(!renderer_can_present() && why[0] && (surface_calls == 1 || strstr(why, "instance without")),
          "failed surface: offscreen (%d callback calls, reason: %s)", surface_calls, why);
    CHECK(renderer_present() == -1, "renderer_present without a surface fails (%s)", renderer_error());
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR, 0x00123456u, 1.0f);
    int r = renderer_end(px, W * 4);
    uint32_t first = 0;
    CHECK(r == 0 && mismatches(px, 0x00123456u, &first) == 0, "offscreen frames still read back");
    renderer_shutdown();
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* Frames in flight: a headless window surface (VK_EXT_headless_surface, e.g. lavapipe) so
 * renderer_present runs its swapchain path without a display. */
typedef struct {
    int32_t sType;
    const void *pNext;
    uint32_t flags;
} HeadlessSurfaceInfo;
typedef void *(*GetInstanceProc)(void *instance, const char *name);
typedef int32_t (*CreateHeadlessSurface)(void *instance, const HeadlessSurfaceInfo *info, const void *alloc, uint64_t *surface);
static int headless_surface(void *instance, uint64_t *surface) {
    void *lib = plat_dlopen(PLAT_LIB_VULKAN);
    GetInstanceProc gipa = lib ? (GetInstanceProc)plat_dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    CreateHeadlessSurface create = gipa ? (CreateHeadlessSurface)gipa(instance, "vkCreateHeadlessSurfaceEXT") : NULL;
    HeadlessSurfaceInfo info = {1000256000, NULL, 0};   /* VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT */
    return create && create(instance, &info, NULL, surface) == 0 ? 0 : -1;
}
static void headless_size(int *w, int *h) {
    *w = W * 2;
    *h = H * 2;
}

/* Presented frames (no wait for the GPU with more than one frame in flight) interleaved with
 * read-back frames, while textures, surfaces and grown geometry are created and the texture cache
 * is dropped between them: every read-back frame holds exactly its own content. */
static void test_frames_in_flight(void) {
    static const char *ext[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    static uint32_t px[W * H], base[W * H];
    static RVertex filler[60000];
    static uint32_t filler_idx[3 * 20000];
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    /* the 6th supersampled 2x, the 7th also sharpened (flat colors: the frames stay exact) */
    const int asked[] = {1, 2, 3, 0, 9, 2, 2}, expect[] = {1, 2, 3, 2, 3, 2, 2};
    for (unsigned i = 0; i < sizeof filler_idx / sizeof *filler_idx; i++) filler_idx[i] = i;   /* zero-area triangles */
    for (unsigned k = 0; k < sizeof asked / sizeof *asked; k++) {
        RendererConfig c = {.samples = 1, .instance_ext = ext, .instance_ext_count = 2, .create_surface = headless_surface,
                            .drawable_size = headless_size, .present_mode = RENDERER_PRESENT_FIFO, .frames_in_flight = asked[k],
                            .supersample = k >= 5 ? 2 : 0, .sharpen = k == 6 ? 60 : 0};
        renderer_configure(&c);
        if (renderer_init(W, H)) {
            CHECK(0, "renderer_init with a headless surface: %s", renderer_error());
            continue;
        }
        CHECK(renderer_frames_in_flight() == expect[k], "frames_in_flight %d -> %d (%d)", asked[k], expect[k], renderer_frames_in_flight());
        if (!renderer_can_present()) {
            printf("skip frames in flight %d: no headless surface (%s)\n", asked[k], renderer_present_info());
            renderer_shutdown();
            continue;
        }
        int bad_present = 0, bad_frames = 0, checked = 0;
        for (uint32_t f = 0; f < 24; f++) {
            const uint32_t tex_color = 0x00080000u * (f + 1) | 0x40u, surf_color = 0x00000800u * (f + 1) | 0x200000u,
                           base_color = 0x00030303u * (f + 1);
            for (int i = 0; i < W * H; i++) base[i] = base_color;
            if (f == 9) renderer_set_texture_budget(1);   /* the next renderer_begin drops the cache */
            renderer_begin(base, W * 4);
            if (f == 10) renderer_set_texture_budget(0);
            uint32_t t = renderer_texture_create(0xF1F000u + k * 64 + f, RENDERER_TEX_BGRA8, 1, 1, 1, &tex_color);
            uint32_t sf = renderer_surface(0xF2F000u + (f % 3), RENDERER_SURFACE_COLOR, 16, 16);
            renderer_set_target(sf, 0);
            renderer_clear(RENDERER_CLEAR_COLOR, surf_color, 1.0f);
            renderer_set_target(0, 0);
            /* Grown geometry: more zero-area triangles every frame (several MiB by the end). */
            const uint32_t nfill = 2500 * (f + 1) < 60000 ? 2500 * (f + 1) : 60000;
            const RViewport full = {0, 0, W, H, 0, 1};
            renderer_draw(&full, 0, filler, nfill, filler_idx, nfill);
            RVertex q[4];
            quad(q, 0, 0, 1, 1, 0xFFFFFFFFu);   /* left half: the new texture, top 3/4 of the rows */
            for (int i = 0; i < 4; i++) { q[i].x = q[i].x < 0 ? -1.0f : 0.0f; q[i].y = q[i].y < 0 ? -0.5f : 1.0f; }
            renderer_set_stage_texture(0, t);
            renderer_draw(&full, 0, q, 4, qi, 6);
            quad(q, 0, 0, 1, 1, 0xFFFFFFFFu);   /* right half: the surface */
            for (int i = 0; i < 4; i++) { q[i].x = q[i].x < 0 ? 0.0f : 1.0f; q[i].y = q[i].y < 0 ? -0.5f : 1.0f; }
            renderer_set_stage_texture(0, sf);
            renderer_draw(&full, 0, q, 4, qi, 6);
            if (f % 4 != 3) {
                bad_present += renderer_present() != 0;
                continue;
            }
            checked++;
            int r = renderer_end(px, W * 4);
            uint32_t l = px[(H / 4) * W + W / 4] & 0xFFFFFFu, rt = px[(H / 4) * W + W * 3 / 4] & 0xFFFFFFu,
                     b = px[(H - 2) * W + W / 2] & 0xFFFFFFu;
            if (r || l != tex_color || rt != surf_color || b != base_color) {
                if (!bad_frames) printf("     frame %u: %06x %06x %06x, expected %06x %06x %06x\n", f, l, rt, b, tex_color, surf_color, base_color);
                bad_frames++;
            }
        }
        CHECK(bad_present == 0, "frames in flight %d: 18 frames presented (%d failed: %s)", expect[k], bad_present, renderer_error());
        CHECK(bad_frames == 0 && checked == 6, "frames in flight %d: read-back frames between presented ones are exact (%d of %d bad)",
              expect[k], bad_frames, checked);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* --sharpen N (RendererConfig.sharpen): contrast-adaptive sharpening of the finished frame before
 * the readback (and the swapchain blit). 0 leaves the frame as it was; a step edge gets darker /
 * brighter pixels beside it, flat areas stay exact, and stronger sharpening overshoots more. */
static void test_sharpen(void) {
    static uint32_t px[W * 2 * H * 2], base[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) base[y * W + x] = x < W / 2 ? 0x00404040u : 0x00B0B0B0u;
    const int amounts[] = {0, 50, 100};
    int dark[3] = {0}, light[3] = {0};
    for (int ss = 1; ss <= 2; ss++)
        for (int k = 0; k < 3; k++) {
            RendererConfig c = {.samples = 1, .supersample = ss, .sharpen = amounts[k]};
            renderer_configure(&c);
            if (renderer_init(W, H)) {
                CHECK(0, "renderer_init with sharpen %d: %s", amounts[k], renderer_error());
                continue;
            }
            CHECK(renderer_sharpen() == amounts[k], "sharpen %d (supersample %d): in use %d", amounts[k], ss, renderer_sharpen());
            renderer_begin(base, W * 4);
            int r = renderer_end(px, W * 4), changed = 0, flat_bad = 0;
            for (int i = 0; i < W * H; i++) {
                int x = i % W;
                changed += px[i] != base[i];
                if (x < W / 2 - 2 || x > W / 2 + 1) flat_bad += px[i] != base[i];
            }
            if (amounts[k] == 0) CHECK(r == 0 && changed == 0, "sharpen 0 (supersample %d): the frame is unchanged (%d)", ss, changed);
            else
                CHECK(r == 0 && changed > 0 && flat_bad == 0, "sharpen %d (supersample %d): only the edge changes (%d changed, %d in flat areas)",
                      amounts[k], ss, changed, flat_bad);
            const uint32_t l = px[(H / 2) * W + W / 2 - 1], h = px[(H / 2) * W + W / 2];
            dark[k] = 0x40 - (int)(l & 0xFF);
            light[k] = (int)(h & 0xFF) - 0xB0;
            if (amounts[k]) CHECK(dark[k] > 0 && light[k] > 0 && (l & 0xFF) == ((l >> 8) & 0xFF),
                                  "sharpen %d (supersample %d): overshoot at the edge (%06x | %06x)", amounts[k], ss, l, h);
            renderer_shutdown();
            if (k == 2) CHECK(dark[2] > dark[1] && light[2] > light[1], "sharpen 100 overshoots more than 50 (%d > %d, %d > %d)", dark[2],
                              dark[1], light[2], light[1]);
        }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
    (void)px;
}

/* --bloom (RendererConfig.bloom, renderer_bloom): the scene recorded before renderer_bloom()
 * glows into its surroundings, draws after it (the HUD) do not; additive and alpha-blended effect
 * draws (depth write off) bloom more than an opaque draw of the same color; a frame without renderer_bloom() is
 * byte-identical to the same frame without --bloom. BW x BH back buffer, pixel rects. */
enum { BW = 320, BH = 240 };
static void rect_quad(RVertex q[4], float x0, float y0, float x1, float y1, uint32_t color) {
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; i++) {
        q[i] = (RVertex){xy[i][0] / BW * 2 - 1, 1 - xy[i][1] / BH * 2, 0.5f, 1, color, 0, {{0}}, 0};
        q[i].tex[0][3] = 1;
    }
}
/* split: call renderer_bloom() after the scene; hud: then draw a white quad (no depth test). */
static void bloom_frame(int split, int hud) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RState opaque, add;
    renderer_state_default(&opaque);
    add = opaque;
    add.blend_enable = 1;
    add.blend_src = 1;   /* ONE */
    add.blend_dst = 1;   /* ONE */
    add.depth_write = 0;
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_set_state(&opaque);
    rect_quad(q, 52, 52, 68, 68, 0xFFFFFFFFu);   /* bright scene object */
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    rect_quad(q, 252, 182, 268, 198, 0xFF909090u);   /* mid gray, opaque */
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    renderer_set_state(&add);
    rect_quad(q, 252, 52, 268, 68, 0xFF909090u);   /* the same gray as a glow sprite (ONE/ONE) */
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    RState over = add;   /* and as an alpha-blended effect sprite: depth test, no depth write */
    over.blend_src = 0x302;
    over.blend_dst = 0x303;
    renderer_set_state(&over);
    rect_quad(q, 152, 182, 168, 198, 0xFF909090u);
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    if (split) renderer_bloom();
    if (hud) {
        renderer_set_state(&opaque);
        rect_quad(q, 52, 182, 68, 198, 0xFFFFFFFFu);   /* HUD: as bright as the scene object */
        renderer_draw(&full, 0, q, 4, qi, 6);
    }
}
static void test_bloom(void) {
    static uint32_t off[BW * BH], a[BW * BH], b[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}};
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sm = cfgs[k].samples, ss = cfgs[k].ss;
        RendererConfig c = {.samples = sm, .supersample = ss};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init without bloom: %s", renderer_error());
            continue;
        }
        CHECK(!renderer_bloom_on(), "bloom %d/%dx: off unless configured", sm, ss);
        bloom_frame(1, 1);   /* renderer_bloom() is ignored when off */
        int r = renderer_end(off, BW * 4);
        CHECK(r == 0 && renderer_bloom_count() == 0, "bloom %d/%dx: off, the split is ignored", sm, ss);
        renderer_shutdown();
        c.bloom = 1;
        c.bloom_strength = 1.0f;
        c.bloom_threshold = 0.7f;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with bloom: %s", renderer_error());
            continue;
        }
        CHECK(renderer_bloom_on(), "bloom %d/%dx: in use", sm, ss);
        bloom_frame(0, 1);
        r = renderer_end(a, BW * 4);
        int diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_bloom_count() == 0 && diff == 0, "bloom %d/%dx: a frame without the split is unchanged (%d pixels differ)",
              sm, ss, diff);
        bloom_frame(1, 1);
        r = renderer_end(a, BW * 4);
        const uint32_t glow = a[60 * BW + 76], center = a[60 * BW + 60], far = a[120 * BW + 160];
        CHECK(r == 0 && renderer_bloom_count() == 1 && (glow & 0xFF) >= 8 && (off[60 * BW + 76] & 0xFFFFFF) == 0,
              "bloom %d/%dx: the bright object glows beside it (%06x)", sm, ss, glow & 0xFFFFFF);
        CHECK(center == 0x00FFFFFFu && (far & 0xFFFFFF) == 0, "bloom %d/%dx: object stays white (%06x), far away stays black (%06x)", sm,
              ss, center & 0xFFFFFF, far & 0xFFFFFF);
        const int add_glow = (int)(a[60 * BW + 276] & 0xFF), opq_glow = (int)(a[190 * BW + 276] & 0xFF);
        CHECK(add_glow > opq_glow && add_glow <= 2 * opq_glow + 2,
              "bloom %d/%dx: an additive draw blooms a little more than an opaque draw, at most about 1.3x (%d vs %d)", sm, ss, add_glow,
              opq_glow);
        const int over_glow = (int)(a[190 * BW + 176] & 0xFF);
        CHECK(over_glow <= opq_glow + 1, "bloom %d/%dx: an alpha-blended halo sprite blooms like an opaque draw, not as a light (%d vs %d)",
              sm, ss, over_glow, opq_glow);
        {   /* a large near-white area (a lamp's own halo sprite, 0xD8) alone in a frame */
            const RViewport full = {0, 0, BW, BH, 0, 1};
            const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
            RVertex q[4];
            RState opaque;
            renderer_state_default(&opaque);
            renderer_begin(NULL, 0);
            renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 1.0f);
            renderer_set_state(&opaque);
            rect_quad(q, 130, 90, 190, 150, 0xFFD8D8D8u);
            renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
            renderer_bloom();
            r = renderer_end(b, BW * 4);
            const int halo = (int)(b[120 * BW + 160] & 0xFF);
            CHECK(r == 0 && halo >= 0xD8 && halo < 0xFF, "bloom %d/%dx: a near-white area brightens but does not clip to flat white (216 -> %d)",
                  sm, ss, halo);
        }
        bloom_frame(1, 0);
        r = renderer_end(b, BW * 4);
        int hud_diff = 0, hud_px = 0;
        for (int y = 160; y < 220; y++)
            for (int x = 30; x < 90; x++) {
                const int in = x >= 52 && x < 68 && y >= 182 && y < 198;
                if (!in) hud_diff += a[y * BW + x] != b[y * BW + x];
                else hud_px += a[y * BW + x] == 0x00FFFFFFu;
            }
        CHECK(r == 0 && hud_diff == 0 && hud_px == 16 * 16,
              "bloom %d/%dx: the HUD draw after the split does not glow (%d pixels around it differ)", sm, ss, hud_diff);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* --rain-hd (RendererConfig.rain, renderer_rain): procedural streak slices over the frame target,
 * depth tested against the scene (a nearer wall hides them) and not written; ignored unless
 * configured; a frame without renderer_rain() is byte-identical to the same frame without the
 * option. */
static void rain_slice(RRain *r, float width) {
    memset(r, 0, sizeof *r);
    r->slices = 1;
    r->color = 0xFFFFFFFFu;
    r->slice[0] = (RRainSlice){0.5f, 0.05f, 0.25f, 0.6f, width, 0.01f, 0.02f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
}
/* Lit streaks and per-streak depth: a frame cleared to grey `bg`, an opaque plane over the right
 * half at NDC depth `plane` (none when 0), then the rain. */
static void rain_lit_frame(const RRain *r, uint32_t bg, float plane) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RState opaque;
    renderer_state_default(&opaque);
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, bg, 1.0f);
    renderer_set_state(&opaque);
    if (plane > 0) {
        rect_quad(q, BW / 2, 0, BW, BH, bg);
        for (int i = 0; i < 4; i++) q[i].z = plane;
        renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    }
    renderer_rain(r);
}
/* Pixels of the half (0 left, 1 right) that differ from bg, and the brightest red channel. */
static int rain_changed(const uint32_t *px, uint32_t bg, int half, int *max) {
    int n = 0;
    *max = 0;
    for (int y = 0; y < BH; y++)
        for (int x = half ? BW / 2 : 0; x < (half ? BW : BW / 2); x++) {
            const uint32_t p = px[y * BW + x] & 0xFFFFFF;
            n += p != (bg & 0xFFFFFF);
            if ((int)(p >> 16 & 255) > *max) *max = (int)(p >> 16 & 255);
        }
    return n;
}
static void test_rain_lit(void) {
    static uint32_t a[BW * BH];
    RendererConfig c = {.samples = 1, .rain = 1};
    renderer_configure(&c);
    if (renderer_init(BW, BH)) {
        CHECK(0, "renderer_init with rain: %s", renderer_error());
        return;
    }
    RRain r;
    rain_slice(&r, 0.01f);
    r.slice[0].alpha = 0;
    r.slice[0].lit = 1;
    int max, max_bg;
    rain_lit_frame(&r, 0x00404040u, 0);
    renderer_end(a, BW * 4);
    const int lit = rain_changed(a, 0x00404040u, 0, &max) + rain_changed(a, 0x00404040u, 1, &max_bg);
    CHECK(lit > BW * BH / 50 && max > 0x50 && max <= 0x80, "lit rain brightens a grey scene behind the streaks (%d pixels, max %d)", lit, max);
    rain_lit_frame(&r, 0, 0);
    renderer_end(a, BW * 4);
    CHECK(rain_changed(a, 0, 0, &max) + rain_changed(a, 0, 1, &max) == 0, "lit rain over black stays black: it only catches the scene's light");
    /* Per-streak depth: projection a 1, b 0.1; the slice at distance 1.5 (depth 0.933), its streaks
     * spread to 4.5; a plane at depth 0.95 (distance 2) hides the streaks behind it only. */
    rain_slice(&r, 0.01f);
    r.slice[0].alpha = 0;
    r.slice[0].lit = 1;
    r.slice[0].depth = 1.0f - 0.1f / 1.5f;
    r.slice[0].dist = 1.5f;
    r.slice[0].spread = 3;
    rain_lit_frame(&r, 0x00404040u, 0.95f);
    renderer_end(a, BW * 4);
    const int flat_l = rain_changed(a, 0x00404040u, 0, &max), flat_r = rain_changed(a, 0x00404040u, 1, &max);
    CHECK(flat_l > 0 && flat_r > flat_l / 2, "depth_a 0: every streak at the slice's depth, in front of the plane (%d, %d)", flat_l, flat_r);
    r.depth_a = 1.0f;
    r.depth_b = 0.1f;
    rain_lit_frame(&r, 0x00404040u, 0.95f);
    renderer_end(a, BW * 4);
    const int open_l = rain_changed(a, 0x00404040u, 0, &max), behind_r = rain_changed(a, 0x00404040u, 1, &max);
    CHECK(open_l > 0 && behind_r > 0 && behind_r < open_l * 3 / 4,
          "own depth per streak: the plane hides the farther streaks only (%d open, %d in front of the plane)", open_l, behind_r);
    renderer_shutdown();
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}
/* --rain-hd-cover (RendererConfig.rain_cover, renderer_rain_cover): a grey frame with a "roof" over
 * the left half of the map (a quad at clip x -1..0, behind the rain, flagged RENDERER_RAIN_COVER;
 * the map rows take clip x, y straight to the map, depth 0.2), then lit rain whose streaks all sit
 * at distance 1.5 and map to u = 0.5 + view x / 3 (screen left half: u < 0.5), depth 0.5: the roof
 * lies 3 world units above them. cover: call renderer_rain_cover. */
static void cover_frame(int cover) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RState opaque;
    renderer_state_default(&opaque);
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0x00404040u, 1.0f);
    renderer_set_state(&opaque);
    rect_quad(q, 0, 0, BW / 2, BH, 0x00404040u);
    for (int i = 0; i < 4; i++) q[i].z = 0.999f;
    renderer_draw(&full, RENDERER_DEPTH_TEST | RENDERER_RAIN_COVER, q, 4, qi, 6);
    if (cover) {
        const RRainCover c = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0.2f}},
                              {{1.0f / 3, 0, 0, 0.5f}, {0, 1.0f / 3, 0, 0.5f}, {0, 0, 0, 0.5f}}, 10, 0.3f, 0.4f, 1, 1};
        renderer_rain_cover(&c);
    }
    RRain r;
    rain_slice(&r, 0.01f);
    r.slice[0].alpha = 0;
    r.slice[0].lit = 1;
    r.slice[0].depth = 1.0f - 0.1f / 1.5f;
    r.slice[0].dist = 1.5f;
    r.slice[0].spread = 1;
    r.depth_a = 1.0f;
    r.depth_b = 0.1f;
    renderer_rain(&r);
}
/* Lit pixels in columns x0..x1-1. */
static int cover_lit(const uint32_t *px, int x0, int x1) {
    int n = 0;
    for (int y = 0; y < BH; y++)
        for (int x = x0; x < x1; x++) n += (px[y * BW + x] & 0xFFFFFF) != 0x404040u;
    return n;
}
static void test_rain_cover(void) {
    static uint32_t a[BW * BH], b[BW * BH];
    RendererConfig c = {.samples = 1, .rain = 1};
    renderer_configure(&c);
    if (renderer_init(BW, BH)) {
        CHECK(0, "renderer_init with rain: %s", renderer_error());
        return;
    }
    CHECK(!renderer_rain_cover_on(), "cover: off unless configured");
    cover_frame(1);   /* ignored: the frame as without it */
    CHECK(renderer_end(b, BW * 4) == 0 && renderer_rain_cover_count() == 0, "cover off: renderer_rain_cover is ignored");
    renderer_shutdown();
    c.rain_cover = 1;
    renderer_configure(&c);
    if (renderer_init(BW, BH)) {
        CHECK(0, "renderer_init with rain cover: %s", renderer_error());
        return;
    }
    CHECK(renderer_rain_cover_on(), "cover: in use");
    cover_frame(0);
    CHECK(renderer_end(a, BW * 4) == 0 && renderer_rain_cover_count() == 0, "cover: no map without renderer_rain_cover");
    int diff = 0;
    for (int i = 0; i < BW * BH; i++) diff += a[i] != b[i];
    CHECK(diff == 0, "cover: a frame without a map is the frame of the option off (%d pixels differ)", diff);
    const int open_l = cover_lit(a, 0, BW / 2), open_r = cover_lit(a, BW / 2, BW);
    cover_frame(1);
    CHECK(renderer_end(a, BW * 4) == 0 && renderer_rain_cover_count() == 1, "cover: one map drawn (%u)", renderer_rain_cover_count());
    const int left = cover_lit(a, 0, BW / 2 - 4), right = cover_lit(a, BW / 2, BW);
    CHECK(open_l > 0 && open_r > 0 && left == 0 && right > open_r / 2 && right <= open_r,
          "cover: no streaks under the roof (left %d of %d), the open half keeps them (%d of %d)", left, open_l, right, open_r);
    renderer_shutdown();
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}
/* A black frame with an opaque wall (left half, NDC depth 0.2), then rain (when r) and a HUD
 * quad without depth test. */
static void rain_frame(const RRain *r) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RState opaque;
    renderer_state_default(&opaque);
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_set_state(&opaque);
    rect_quad(q, 0, 0, BW / 2, BH, 0xFF202020u);
    for (int i = 0; i < 4; i++) q[i].z = 0.2f;
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    if (r) renderer_rain(r);
    rect_quad(q, 250, 10, 290, 30, 0xFF0000FFu);
    renderer_draw(&full, 0, q, 4, qi, 6);
}
static void test_rain(void) {
    static uint32_t off[BW * BH], a[BW * BH], b[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}};
    RRain r;
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sm = cfgs[k].samples, ss = cfgs[k].ss;
        RendererConfig c = {.samples = sm, .supersample = ss};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init without rain: %s", renderer_error());
            continue;
        }
        CHECK(!renderer_rain_on(), "rain %d/%dx: off unless configured", sm, ss);
        rain_frame(NULL);
        int ret = renderer_end(off, BW * 4);
        rain_slice(&r, 0.01f);
        rain_frame(&r);
        ret |= renderer_end(a, BW * 4);
        int diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(ret == 0 && renderer_rain_count() == 0 && diff == 0, "rain %d/%dx: off, renderer_rain is ignored (%d pixels differ)", sm, ss, diff);
        renderer_shutdown();
        c.rain = 1;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with rain: %s", renderer_error());
            continue;
        }
        CHECK(renderer_rain_on(), "rain %d/%dx: in use", sm, ss);
        rain_frame(NULL);
        ret = renderer_end(a, BW * 4);
        diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(ret == 0 && renderer_rain_count() == 0 && diff == 0, "rain %d/%dx: a frame without renderer_rain is unchanged (%d differ)", sm, ss,
              diff);
        rain_frame(&r);
        ret = renderer_end(a, BW * 4);
        int lit = 0, wall = 0, hud = 0, max_wide = 0;
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < BW; x++) {
                const uint32_t p = a[y * BW + x];
                const int in_hud = x >= 250 && x < 290 && y >= 10 && y < 30;
                if (in_hud) hud += p == 0x000000FFu;
                else if (x < BW / 2) wall += p == 0x00202020u;
                else if (p & 0xFFFFFF) {
                    lit++;
                    if ((int)(p & 0xFF) > max_wide) max_wide = (int)(p & 0xFF);
                }
            }
        const int right = BW / 2 * BH - 40 * 20;
        CHECK(ret == 0 && renderer_rain_count() == 1, "rain %d/%dx: one rain operation executed (%u)", sm, ss, renderer_rain_count());
        CHECK(lit > right / 50 && lit < right / 2 && max_wide > 0x80, "rain %d/%dx: streaks behind the wall's depth (%d of %d pixels, max %d)", sm,
              ss, lit, right, max_wide);
        CHECK(wall == BW / 2 * BH, "rain %d/%dx: the nearer wall hides the slice (%d of %d wall pixels unchanged)", sm, ss, wall, BW / 2 * BH);
        CHECK(hud == 40 * 20, "rain %d/%dx: the HUD drawn after it is unchanged (%d)", sm, ss, hud);
        int col[BW] = {0};   /* tilt 0: streaks are vertical, so lit pixels gather in a few columns */
        for (int y = 0; y < BH; y++)
            for (int x = BW / 2; x < BW; x++) col[x] += (a[y * BW + x] & 0xFF) > 0x40;
        int cols = 0;
        for (int x = BW / 2; x < BW; x++) cols += col[x] > 0;
        CHECK(cols > 4 && cols < BW / 2 - 8, "rain %d/%dx: vertical streaks light %d of %d columns", sm, ss, cols, BW / 2);
        rain_slice(&r, 0.0005f);   /* a tenth of a pixel wide: faint, not gone */
        rain_frame(&r);
        renderer_end(b, BW * 4);
        int max_thin = 0, lit_thin = 0;
        for (int i = 0; i < BW * BH; i++)
            if (i % BW >= BW / 2 && !(i % BW >= 250 && i % BW < 290 && i / BW >= 10 && i / BW < 30) && (b[i] & 0xFF)) {
                lit_thin++;
                if ((int)(b[i] & 0xFF) > max_thin) max_thin = (int)(b[i] & 0xFF);
            }
        CHECK(lit_thin > 0 && max_thin < max_wide / 2, "rain %d/%dx: sub-pixel streaks fade (max %d vs %d)", sm, ss, max_thin, max_wide);
        rain_slice(&r, 0.0005f);   /* a veil: every pixel behind the slice's depth, none in front of it */
        r.slice[0].alpha = 0;
        r.slice[0].haze = 0.25f;
        rain_frame(&r);
        renderer_end(b, BW * 4);
        int veiled = 0, wall_kept = 0;
        for (int y = 40; y < BH; y++)
            for (int x = 0; x < BW; x++) {
                const int g = (int)(b[y * BW + x] & 0xFF);
                if (x < BW / 2) wall_kept += b[y * BW + x] == 0x00202020u;
                else veiled += g >= 0x3C && g <= 0x42;   /* 0.25 x 255 */
            }
        CHECK(veiled == BW / 2 * (BH - 40) && wall_kept == BW / 2 * (BH - 40), "rain %d/%dx: haze veils what lies behind the slice only (%d, %d)",
              sm, ss, veiled, wall_kept);
        r.slices = 0;
        rain_frame(&r);
        renderer_end(b, BW * 4);
        diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += b[i] != off[i];
        CHECK(diff == 0, "rain %d/%dx: no slices, no change (%d)", sm, ss, diff);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* --ssao (RendererConfig.ssao, renderer_ssao): a wall with a nearer box in front of its left part
 * (perspective depth: view depth 6 and 5) darkens beside the box's edge, stays exactly as it was
 * away from it, the box and the far plane (nothing drawn) keep their colors, a draw after the
 * split (the HUD) is not darkened, a higher strength darkens more; ignored unless configured; a
 * frame without renderer_ssao() is byte-identical to the same frame without the option. */
static const RSsao ssao_proj = {1.01f, 0.101f, 1.4f, 2.0f};
static float ssao_ndc_z(float d) { return ssao_proj.depth_a - ssao_proj.depth_b / d; }
/* Wall (gray, view depth 6) over rows 0-199, box (light gray, depth 5) over x < 160 of rows 40-159;
 * rows 200-239 stay the clear color (far plane). Vertices at w = view depth, so the depth is
 * perspective-correct like the title's. */
static void ssao_quad(RVertex q[4], float x0, float y0, float x1, float y1, float d, uint32_t color) {
    rect_quad(q, x0, y0, x1, y1, color);
    for (int i = 0; i < 4; i++) {
        q[i].x *= d;
        q[i].y *= d;
        q[i].z = ssao_ndc_z(d) * d;
        q[i].w = d;
    }
}
static void ssao_frame(int split, int hud) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RState opaque;
    renderer_state_default(&opaque);
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0xFF204060u, 1.0f);
    renderer_set_state(&opaque);
    ssao_quad(q, 0, 0, BW, 200, 6, 0xFF808080u);
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    ssao_quad(q, 0, 40, 160, 160, 5, 0xFFC0C0C0u);
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    if (split) renderer_ssao(&ssao_proj);
    if (hud) {
        rect_quad(q, 150, 90, 190, 110, 0xFF0000FFu);   /* across the edge, no depth test */
        renderer_draw(&full, 0, q, 4, qi, 6);
    }
}
static void test_ssao(void) {
    static uint32_t off[BW * BH], a[BW * BH], b[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}};
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sm = cfgs[k].samples, ss = cfgs[k].ss;
        RendererConfig c = {.samples = sm, .supersample = ss};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init without ssao: %s", renderer_error());
            continue;
        }
        CHECK(!renderer_ssao_on(), "ssao %d/%dx: off unless configured", sm, ss);
        ssao_frame(0, 1);
        int r = renderer_end(off, BW * 4);
        ssao_frame(1, 1);   /* renderer_ssao() is ignored when off */
        r |= renderer_end(a, BW * 4);
        int diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_ssao_count() == 0 && diff == 0, "ssao %d/%dx: off, the split is ignored (%d pixels differ)", sm, ss, diff);
        renderer_shutdown();
        c.ssao = 1;
        c.ssao_strength = 1.0f;
        c.ssao_radius = 1.5f;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with ssao: %s", renderer_error());
            continue;
        }
        CHECK(renderer_ssao_on(), "ssao %d/%dx: in use", sm, ss);
        ssao_frame(0, 1);
        r = renderer_end(a, BW * 4);
        diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_ssao_count() == 0 && diff == 0, "ssao %d/%dx: a frame without the split is unchanged (%d pixels differ)", sm,
              ss, diff);
        ssao_frame(1, 1);
        r = renderer_end(a, BW * 4);
        CHECK(r == 0 && renderer_ssao_count() == 1, "ssao %d/%dx: one split executed (%u)", sm, ss, renderer_ssao_count());
        const int edge = (int)(a[130 * BW + 166] & 0xFF), wall = (int)(off[130 * BW + 166] & 0xFF);
        CHECK(edge < wall - 8, "ssao %d/%dx: the wall beside the box's edge darkens (%d vs %d)", sm, ss, edge, wall);
        int open_diff = 0, box_diff = 0, sky_diff = 0;
        for (int y = 0; y < 200; y++)
            for (int x = 280; x < BW; x++) open_diff += a[y * BW + x] != off[y * BW + x];
        for (int y = 44; y < 156; y++)
            for (int x = 4; x < 156; x++) box_diff += a[y * BW + x] != off[y * BW + x];
        for (int y = 200; y < BH; y++)
            for (int x = 0; x < BW; x++) sky_diff += a[y * BW + x] != off[y * BW + x];
        CHECK(open_diff == 0, "ssao %d/%dx: the open wall away from the box keeps its exact color (%d pixels differ)", sm, ss, open_diff);
        CHECK(box_diff == 0, "ssao %d/%dx: the box's face (nothing in front of it) keeps its color (%d pixels differ)", sm, ss, box_diff);
        CHECK(sky_diff == 0, "ssao %d/%dx: the far plane (nothing drawn) keeps the clear color (%d pixels differ)", sm, ss, sky_diff);
        int hud = 0;
        for (int y = 90; y < 110; y++)
            for (int x = 150; x < 190; x++) hud += a[y * BW + x] == 0x000000FFu;
        CHECK(hud == 40 * 20, "ssao %d/%dx: the HUD drawn after the split is not darkened (%d of %d)", sm, ss, hud, 40 * 20);
        renderer_shutdown();
        c.ssao_strength = 2.0f;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with ssao strength 2: %s", renderer_error());
            continue;
        }
        ssao_frame(1, 0);
        r = renderer_end(b, BW * 4);
        const int strong = (int)(b[130 * BW + 166] & 0xFF);
        CHECK(r == 0 && strong < edge, "ssao %d/%dx: strength 2 darkens more than 1 (%d vs %d)", sm, ss, strong, edge);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* Rain splashes (renderer_rain_splash): a camera 2 units above a floor (world z = 0, z up) and a
 * wall (y = 6) facing it; vertices projected on the CPU with the camera's axes, w = view
 * distance (perspective-correct depth, like the title's). */
/* c, sn: cosine and sine of the pitch (negative: looking down), the camera looking along +y. */
static RRainSplash splash_cam(float c, float sn) {
    RRainSplash p = {.depth_a = 1.01f, .depth_b = 0.101f, .px = 1.4f, .py = 2.0f, .eye = {0, 0, 2},
                     .right = {1, 0, 0}, .up_v = {0, -sn, c}, .back = {0, -c, -sn}, .up = {0, 0, 1}, .time = 1.234f,
                     .cell = 0.3f, .period = 0.6f, .chance = 1, .splash = 4, .ripple = 4, .fade = 30, .color = 0xFFFFFFFFu};
    return p;
}
static void splash_vertex(RVertex *v, const RRainSplash *p, float x, float y, float z, uint32_t color) {
    const float w[3] = {x - p->eye[0], y - p->eye[1], z - p->eye[2]};
    const float vx = w[0] * p->right[0] + w[1] * p->right[1] + w[2] * p->right[2];
    const float vy = w[0] * p->up_v[0] + w[1] * p->up_v[1] + w[2] * p->up_v[2];
    const float d = -(w[0] * p->back[0] + w[1] * p->back[1] + w[2] * p->back[2]);
    *v = (RVertex){p->px * vx, p->py * vy, p->depth_a * d - p->depth_b, d, color, 0, {{0}}, 0};
    v->tex[0][3] = 1;
}
/* floor: floor quad of that colour (x -20..20, y 0.3..30 with the wall, else -30..30); wall: grey
 * wall at y = 6 (z 0..6); then the
 * splash (split), then a blue HUD quad without depth test (hud). */
static void splash_frame(const RRainSplash *p, uint32_t floor, int wall, int split, int ssao, int hud) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RState opaque;
    renderer_state_default(&opaque);
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0xFF000000u, 1.0f);
    renderer_set_state(&opaque);
    splash_vertex(&q[0], p, -20, 30, 0, floor);
    splash_vertex(&q[1], p, 20, 30, 0, floor);
    splash_vertex(&q[2], p, 20, wall ? 0.3f : -30, 0, floor);
    splash_vertex(&q[3], p, -20, wall ? 0.3f : -30, 0, floor);
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    if (wall) {
        splash_vertex(&q[0], p, -20, 6, 6, 0xFF808080u);
        splash_vertex(&q[1], p, 20, 6, 6, 0xFF808080u);
        splash_vertex(&q[2], p, 20, 6, 0, 0xFF808080u);
        splash_vertex(&q[3], p, -20, 6, 0, 0xFF808080u);
        renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    }
    if (ssao) {
        const RSsao ao = {p->depth_a, p->depth_b, p->px, p->py};
        renderer_ssao(&ao);
    }
    if (split) renderer_rain_splash(p);
    if (hud) {
        rect_quad(q, 20, 200, 60, 220, 0xFF0000FFu);
        renderer_draw(&full, 0, q, 4, qi, 6);
    }
}
/* Rows of the wall (above its foot at the screen) for the tilted camera. */
static int splash_wall_rows(const RRainSplash *p) {
    RVertex v;
    splash_vertex(&v, p, 0, 6, 0, 0);
    return (int)((1 - v.y / v.w) * 0.5f * BH) - 2;
}
static void test_rain_splash(void) {
    static uint32_t off[BW * BH], a[BW * BH], b[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}};
    const RRainSplash p = splash_cam(0.76604444f, -0.64278761f);   /* 40 degrees down */
    const int wall_rows = splash_wall_rows(&p);
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sm = cfgs[k].samples, ss = cfgs[k].ss;
        RendererConfig c = {.samples = sm, .supersample = ss};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init without rain: %s", renderer_error());
            continue;
        }
        CHECK(!renderer_rain_splash_on(), "splash %d/%dx: off without rain", sm, ss);
        splash_frame(&p, 0xFF808080u, 1, 0, 0, 1);
        int r = renderer_end(off, BW * 4);
        splash_frame(&p, 0xFF808080u, 1, 1, 0, 1);   /* ignored */
        r |= renderer_end(a, BW * 4);
        int diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_rain_splash_count() == 0 && diff == 0, "splash %d/%dx: rain off, ignored (%d pixels differ)", sm, ss, diff);
        renderer_shutdown();
        c.rain = 1;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with rain: %s", renderer_error());
            continue;
        }
        CHECK(renderer_rain_splash_on(), "splash %d/%dx: in use with rain", sm, ss);
        splash_frame(&p, 0xFF808080u, 1, 0, 0, 1);
        r = renderer_end(a, BW * 4);
        diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_rain_splash_count() == 0 && diff == 0, "splash %d/%dx: a frame without it is unchanged (%d pixels differ)", sm, ss,
              diff);
        splash_frame(&p, 0xFF808080u, 1, 1, 0, 1);
        r = renderer_end(a, BW * 4);
        CHECK(r == 0 && renderer_rain_splash_count() == 1, "splash %d/%dx: executed (%u)", sm, ss, renderer_rain_splash_count());
        int floor_lit = 0, darker = 0, wall_diff = 0, hud = 0;
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < BW; x++) {
                const uint32_t u = a[y * BW + x], o = off[y * BW + x];
                if ((u & 0xFF) < (o & 0xFF)) darker++;
                if (y < wall_rows) wall_diff += u != o;
                else if (y < 196 && u != o) floor_lit++;
            }
        for (int y = 200; y < 220; y++)
            for (int x = 20; x < 60; x++) hud += a[y * BW + x] == 0x000000FFu;
        CHECK(floor_lit > 300, "splash %d/%dx: drops on the floor (%d pixels brightened)", sm, ss, floor_lit);
        CHECK(darker == 0, "splash %d/%dx: they only brighten (%d darker pixels)", sm, ss, darker);
        CHECK(wall_diff == 0, "splash %d/%dx: none on the wall (%d of its %d rows' pixels differ)", sm, ss, wall_diff, wall_rows);
        CHECK(hud == 40 * 20, "splash %d/%dx: the HUD drawn after it is untouched (%d of %d)", sm, ss, hud, 40 * 20);
        splash_frame(&p, 0xFF808080u, 1, 1, 0, 1);
        r = renderer_end(b, BW * 4);
        diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != b[i];
        CHECK(r == 0 && diff == 0, "splash %d/%dx: the same time and camera give the same frame (%d differ)", sm, ss, diff);
        RRainSplash later = p;
        later.time += 0.37f;
        splash_frame(&later, 0xFF808080u, 1, 1, 0, 1);
        r = renderer_end(b, BW * 4);
        diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != b[i];
        CHECK(r == 0 && diff > 100, "splash %d/%dx: they change with time (%d differ)", sm, ss, diff);
        splash_frame(&p, 0xFF000000u, 1, 0, 0, 0);
        r = renderer_end(a, BW * 4);
        splash_frame(&p, 0xFF000000u, 1, 1, 0, 0);
        r |= renderer_end(b, BW * 4);
        int lit_black = 0;
        for (int i = 0; i < BW * BH; i++) lit_black += a[i] != b[i];
        CHECK(r == 0 && lit_black == 0, "splash %d/%dx: a black floor stays black (lit by the scene; %d pixels differ)", sm, ss, lit_black);
        RRainSplash flat = p;
        flat.flat = 0.2f;
        splash_frame(&flat, 0xFF000000u, 1, 1, 0, 0);
        r = renderer_end(b, BW * 4);
        int flat_floor = 0, flat_wall = 0;
        for (int i = 0; i < BW * BH; i++) {
            if (i < wall_rows * BW) flat_wall += a[i] != b[i];
            else flat_floor += a[i] != b[i];
        }
        CHECK(r == 0 && flat_floor > 300 && flat_wall == 0, "splash %d/%dx: their flat colour shows on a black floor (%d pixels), not on the wall (%d)", sm,
              ss, flat_floor, flat_wall);
        /* World anchored: a camera looking straight down, moved 8 pixels' worth of world along x,
         * sees the same drops 8 pixels further left. */
        RRainSplash top = splash_cam(0, -1);   /* straight down */
        const float step = 8 * 2 * 2.0f / (top.px * BW);   /* 8 pixels at view distance 2 */
        splash_frame(&top, 0xFF808080u, 0, 1, 0, 0);
        r = renderer_end(a, BW * 4);
        top.eye[0] = step;
        splash_frame(&top, 0xFF808080u, 0, 1, 0, 0);
        r |= renderer_end(b, BW * 4);
        int moved = 0, same = 0, n = 0;
        for (int y = 8; y < BH - 8; y++)
            for (int x = 16; x < BW - 16; x++) {
                n++;
                moved += dist(b[y * BW + x], a[y * BW + x + 8]) > 3;
                same += dist(b[y * BW + x], a[y * BW + x]) > 3;
            }
        CHECK(r == 0 && same > 200 && moved * 50 < same, "splash %d/%dx: fixed in the world (%d of %d differ after the shift, %d without it)", sm,
              ss, moved, n, same);
        renderer_shutdown();
        c.ssao = 1;
        c.ssao_strength = 1;
        c.ssao_radius = 1;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with rain and ssao: %s", renderer_error());
            continue;
        }
        splash_frame(&p, 0xFF808080u, 1, 0, 1, 1);
        r = renderer_end(off, BW * 4);
        splash_frame(&p, 0xFF808080u, 1, 1, 1, 1);
        r |= renderer_end(a, BW * 4);
        floor_lit = 0;
        for (int i = wall_rows * BW; i < 196 * BW; i++) floor_lit += a[i] != off[i];
        CHECK(r == 0 && renderer_ssao_count() == 1 && renderer_rain_splash_count() == 1 && floor_lit > 300,
              "splash %d/%dx: with ssao in the same split (%u %u, %d pixels)", sm, ss, renderer_ssao_count(), renderer_rain_splash_count(), floor_lit);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* Wet ground (renderer_rain_wet): the splash camera over a grey floor, a grey wall at y = 6 with a
 * white lamp on it (x -1..1, z 0.5..1.5), then (wet) the wet op, then a blue HUD quad (hud). */
static RRainWet wet_params(const RRainSplash *c) {
    RRainWet w = {.depth_a = c->depth_a, .depth_b = c->depth_b, .px = c->px, .py = c->py,
                  .up_view = {c->right[2], c->up_v[2], c->back[2]}, .reflect = 2.5f, .darken = 0.3f, .reach = 40};
    return w;
}
static void wet_frame(const RRainSplash *p, const RRainWet *w, int hud) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    splash_frame(p, 0xFF606060u, 1, 0, 0, 0);
    splash_vertex(&q[0], p, -1, 5.99f, 1.5f, 0xFFFFFFFFu);
    splash_vertex(&q[1], p, 1, 5.99f, 1.5f, 0xFFFFFFFFu);
    splash_vertex(&q[2], p, 1, 5.99f, 0.5f, 0xFFFFFFFFu);
    splash_vertex(&q[3], p, -1, 5.99f, 0.5f, 0xFFFFFFFFu);
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    if (w) renderer_rain_wet(w);
    if (hud) {
        rect_quad(q, 20, 200, 60, 220, 0xFF0000FFu);
        renderer_draw(&full, 0, q, 4, qi, 6);
    }
}
/* Pixel of a world point under the splash camera. */
static int wet_pixel(const RRainSplash *p, float x, float y, float z) {
    RVertex v;
    splash_vertex(&v, p, x, y, z, 0);
    const int px = (int)((v.x / v.w + 1) * 0.5f * BW), py = (int)((1 - v.y / v.w) * 0.5f * BH);
    return py * BW + px;
}
static int luma(uint32_t c) { return (int)((c >> 16 & 255) + (c >> 8 & 255) + (c & 255)); }
static void test_rain_wet(void) {
    static uint32_t off[BW * BH], a[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}};
    const RRainSplash p = splash_cam(0.90630779f, -0.42261826f);   /* 25 degrees down: the lamp in view */
    const RRainWet w = wet_params(&p);
    const int wall_rows = splash_wall_rows(&p);
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sm = cfgs[k].samples, ss = cfgs[k].ss;
        RendererConfig c = {.samples = sm, .supersample = ss, .rain = 1};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with rain: %s", renderer_error());
            continue;
        }
        CHECK(!renderer_rain_wet_on(), "wet %d/%dx: off without rain_wet", sm, ss);
        wet_frame(&p, NULL, 1);
        int r = renderer_end(off, BW * 4);
        wet_frame(&p, &w, 1);   /* ignored */
        r |= renderer_end(a, BW * 4);
        int diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_rain_wet_count() == 0 && diff == 0, "wet %d/%dx: rain_wet off, ignored (%d pixels differ)", sm, ss, diff);
        renderer_shutdown();
        c.rain_wet = 1;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with rain_wet: %s", renderer_error());
            continue;
        }
        CHECK(renderer_rain_wet_on(), "wet %d/%dx: in use with rain_wet", sm, ss);
        wet_frame(&p, NULL, 1);
        r = renderer_end(a, BW * 4);
        diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_rain_wet_count() == 0 && diff == 0, "wet %d/%dx: a frame without it is unchanged, no pass (%d pixels differ)", sm,
              ss, diff);
        wet_frame(&p, &w, 1);
        r = renderer_end(a, BW * 4);
        CHECK(r == 0 && renderer_rain_wet_count() == 1, "wet %d/%dx: executed (%u%s%s)", sm, ss, renderer_rain_wet_count(), r ? ", " : "", r ? renderer_error() : "");
        int wall_diff = 0, hud = 0, darker = 0, floor_n = 0;
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < BW; x++) {
                const uint32_t u = a[y * BW + x], o = off[y * BW + x];
                if (y < wall_rows - 4) wall_diff += u != o;   /* the reduced-size mask is bilinear at the wall's foot */
                else if (y >= wall_rows && y < 196) {
                    floor_n++;
                    darker += luma(u) < luma(o);
                }
            }
        for (int y = 200; y < 220; y++)
            for (int x = 20; x < 60; x++) hud += a[y * BW + x] == 0x000000FFu;
        CHECK(wall_diff == 0, "wet %d/%dx: the wall is dry (%d of its %d rows' pixels differ)", sm, ss, wall_diff, wall_rows - 4);
        CHECK(darker * 2 > floor_n, "wet %d/%dx: the floor gets darker (%d of %d pixels)", sm, ss, darker, floor_n);
        CHECK(hud == 40 * 20, "wet %d/%dx: the HUD drawn after it is untouched (%d of %d)", sm, ss, hud, 40 * 20);
        /* The lamp's reflection: the floor in front of the wall under it (mirror z -0.5..-1.5: floor y 3.4..4.8). */
        int lamp = 0, side = 0;
        for (int j = 0; j < 3; j++) {
            const int i = wet_pixel(&p, 0, 3.7f + 0.4f * (float)j, 0), e = wet_pixel(&p, 3.5f, 3.7f + 0.4f * (float)j, 0);
            lamp += luma(a[i]) - luma(off[i]);
            side += luma(a[e]) - luma(off[e]);
        }
        CHECK(lamp > 30 && side < 0, "wet %d/%dx: the lamp is reflected on the floor (%+d under it, %+d beside it)", sm, ss, lamp, side);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* A dome (hemisphere of radius r, 12 rings of 24 quads) standing on the floor at (cx, cy): the
 * curved top of a character's shoulder or head. */
static void wet_dome(const RRainSplash *p, float cx, float cy, float r, uint32_t color) {
    enum { NR = 12, NS = 24 };
    static RVertex v[(NR + 1) * NS];
    static uint32_t idx[NR * NS * 6];
    const RViewport full = {0, 0, BW, BH, 0, 1};
    int n = 0;
    for (int i = 0; i <= NR; i++)
        for (int j = 0; j < NS; j++) {
            const float th = 1.5707963f * (float)i / NR, ph = 6.2831853f * (float)j / NS;
            splash_vertex(&v[i * NS + j], p, cx + r * sinf(th) * cosf(ph), cy + r * sinf(th) * sinf(ph), r * cosf(th), color);
        }
    for (int i = 0; i < NR; i++)
        for (int j = 0; j < NS; j++) {
            const uint32_t a = (uint32_t)(i * NS + j), b = (uint32_t)(i * NS + (j + 1) % NS);
            const uint32_t q[6] = {a, b, b + NS, a, b + NS, a + NS};
            for (int k = 0; k < 6; k++) idx[n++] = q[k];
        }
    renderer_draw(&full, RENDERER_DEPTH_TEST, v, (NR + 1) * NS, idx, n);
}
/* The wet ground reflects only what the colour holds at its op: the 3D scene's end, where the
 * replay puts it. A bright 2D overlay drawn without depth test after it (the score HUD over the
 * wall) is not reflected, while the same overlay drawn before the op would be (the bug it fixes).
 * And the curved top of a dome on the wet floor (a shoulder) is not darkened: the wet mask's
 * flatness test. */
static void test_rain_wet_scene(void) {
    static uint32_t a[BW * BH], b[BW * BH], c2[BW * BH], off[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}};
    const RRainSplash p = splash_cam(0.90630779f, -0.42261826f);   /* 25 degrees down */
    const RRainWet w = wet_params(&p);
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    const int wall_rows = splash_wall_rows(&p);
    RVertex q[4];
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sm = cfgs[k].samples, ss = cfgs[k].ss;
        RendererConfig c = {.samples = sm, .supersample = ss, .rain = 1, .rain_wet = 1};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with rain_wet: %s", renderer_error());
            continue;
        }
        if (!renderer_rain_wet_on()) {   /* the device cannot sample the depth here: nothing to run */
            renderer_shutdown();
            continue;
        }
        /* The overlay: white, over the wall right of the lamp (screen x 200-280, the wall's rows). */
        wet_frame(&p, &w, 0);
        int r = renderer_end(b, BW * 4);
        wet_frame(&p, &w, 0);
        rect_quad(q, 200, 0, 280, (float)(wall_rows - 2), 0xFFFFFFFFu);
        renderer_draw(&full, 0, q, 4, qi, 6);
        r |= renderer_end(a, BW * 4);
        wet_frame(&p, NULL, 0);   /* the overlay before the op: in the colour it reflects */
        renderer_draw(&full, 0, q, 4, qi, 6);
        renderer_rain_wet(&w);
        r |= renderer_end(c2, BW * 4);
        int after = 0, before = 0;
        for (int y = wall_rows + 2; y < 196; y++)
            for (int x = 200; x < 280; x++) {
                after += a[y * BW + x] != b[y * BW + x];
                before += luma(c2[y * BW + x]) > luma(b[y * BW + x]) + 30;
            }
        CHECK(r == 0 && after == 0 && before > 50,
              "wet %d/%dx: an overlay drawn after the op is not reflected (%d floor pixels differ; drawn before it: %d brighter)", sm, ss, after, before);
        /* The dome (radius 0.1, about a shoulder's curve) on the wet floor near the camera. */
        const float cx = -0.6f, cy = 2.3f, R = 0.1f;
        wet_frame(&p, NULL, 0);
        wet_dome(&p, cx, cy, R, 0xFFA0A0A0u);
        r = renderer_end(off, BW * 4);
        wet_frame(&p, NULL, 0);
        wet_dome(&p, cx, cy, R, 0xFFA0A0A0u);
        renderer_rain_wet(&w);
        r |= renderer_end(a, BW * 4);
        /* Its top, a little towards the camera (normal 10 degrees from up), 3 x 3 pixels: none more than 5%
         * darker (the bilinear composite may leave a faint fringe; wet ground darkens by 30%). */
        const int t = wet_pixel(&p, cx, cy - R * 0.17364818f, R * 0.98480775f);
        int top_darker = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) top_darker += luma(a[t + dy * BW + dx]) * 20 < luma(off[t + dy * BW + dx]) * 19;
        const int fl = wet_pixel(&p, cx + 0.6f, cy, 0);
        CHECK(r == 0 && top_darker == 0 && luma(a[fl]) < luma(off[fl]),
              "wet %d/%dx: the dome's curved top is not darkened (%d of 9 pixels; the floor beside it %+d)", sm, ss, top_darker, luma(a[fl]) - luma(off[fl]));
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

/* --rain-hd-cover with the splashes and wet ground (their RAIN_COVER variants, set 1 the cover's):
 * created and run in one frame with a map, single-sample and MSAA 4. */
static void test_rain_cover_passes(void) {
    static uint32_t a[BW * BH];
    for (int sm = 1; sm <= 4; sm += 3) {
        RendererConfig c = {.samples = sm, .rain = 1, .rain_wet = 1, .rain_cover = 1};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with rain cover: %s", renderer_error());
            continue;
        }
        if (!renderer_rain_splash_on()) {   /* the device cannot sample the depth here: nothing to run */
            renderer_shutdown();
            continue;
        }
        CHECK(renderer_rain_cover_on() && renderer_rain_wet_on(), "cover %dx: with splashes and wet ground", sm);
        const RRainSplash p = splash_cam(0.76604444f, -0.64278761f);
        const RRainWet w = wet_params(&p);
        cover_frame(1);
        renderer_rain_splash(&p);
        renderer_rain_wet(&w);
        CHECK(renderer_end(a, BW * 4) == 0 && renderer_rain_cover_count() == 1 && renderer_rain_splash_count() == 1 && renderer_rain_wet_count() == 1,
              "cover %dx: map, splashes and wet ground run in one frame (%u %u %u)", sm, renderer_rain_cover_count(), renderer_rain_splash_count(),
              renderer_rain_wet_count());
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

int main(void) {
    if (renderer_init(W, H)) {
        printf("SKIP renderer_init: %s\n", renderer_error());
        return 77;
    }
    printf("device: %s\n", renderer_device_name());
    static uint32_t px[W * H], base[W * H];
    uint32_t first = 0;

    /* Clear color parity: the CPU path writes the D3D color dword as is. */
    const uint32_t colors[] = {0x00000000u, 0x00336699u, 0xFFFF0000u, 0x0000FF00u, 0x000000FFu, 0x80FFFFFFu};
    for (unsigned c = 0; c < sizeof colors / sizeof *colors; c++) {
        renderer_begin(NULL, 0);
        renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, colors[c], 1.0f);
        int r = renderer_end(px, W * 4);
        int bad = mismatches(px, colors[c], &first);
        CHECK(r == 0 && bad == 0, "clear %08x read back exactly (%d mismatches, first %08x)", colors[c], bad, first);
        CHECK((px[0] >> 24) == 0, "alpha byte of the readback is 0 (%08x)", px[0]);
    }

    /* Base image upload with a pitch wider than the target, no clear: comes back unchanged. */
    static uint32_t wide[W * 2 * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) base[y * W + x] = wide[y * W * 2 + x] = (uint32_t)(x * 4) << 16 | (uint32_t)(y * 5) << 8 | 0x5A;
    renderer_begin(wide, W * 2 * 4);
    int r = renderer_end(px, W * 4);
    int bad = 0;
    for (int i = 0; i < W * H; i++) bad += px[i] != base[i];
    CHECK(r == 0 && bad == 0, "base image uploaded and read back unchanged (%d mismatches)", bad);

    /* NULL base is black; clears apply in order; a depth-only clear leaves the color. */
    renderer_begin(NULL, 0);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && mismatches(px, 0, &first) == 0, "NULL base reads back black");
    renderer_begin(base, W * 4);
    renderer_clear(RENDERER_CLEAR_COLOR, 0x00112233u, 1.0f);
    renderer_clear(RENDERER_CLEAR_COLOR, 0x00445566u, 1.0f);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0x00FFFFFFu, 0.5f);
    r = renderer_end(px, W * 4);
    bad = mismatches(px, 0x00445566u, &first);
    CHECK(r == 0 && bad == 0, "last color clear wins, depth clear keeps color (%d mismatches, first %08x)", bad, first);

    /* Draws: a centered triangle, depth test independent of order, a sub-rectangle viewport. */
    const RViewport full = {0, 0, W, H, 0, 1};
    const RVertex tri[3] = {{-0.5f, -0.5f, 0.5f, 1, 0x00FF0000u, 0, {{0}}, 0}, {0.5f, -0.5f, 0.5f, 1, 0x00FF0000u, 0, {{0}}, 0}, {0, 0.5f, 0.5f, 1, 0x00FF0000u, 0, {{0}}, 0}};
    const uint32_t ti[3] = {0, 1, 2};
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0x00000040u, 1.0f);
    renderer_draw(&full, RENDERER_DEPTH_TEST, tri, 3, ti, 3);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && px[(H / 2) * W + W / 2] == 0x00FF0000u && px[0] == 0x00000040u && px[W * H - 1] == 0x00000040u,
          "triangle covers the center, not the corners (center %08x, corner %08x)", px[(H / 2) * W + W / 2], px[0]);
    /* Clip space y up: the apex (y = +0.5) is near the top rows, the base near the bottom. */
    CHECK(px[(H / 4 + 2) * W + W / 2] == 0x00FF0000u && px[(H * 3 / 4 - 2) * W + W / 4 + 2] == 0x00FF0000u &&
              px[(H / 4 + 2) * W + W / 4 + 2] == 0x00000040u,
          "y axis points up in clip space");
    /* Clip-space w: the same triangle scaled by w = 2 covers the same pixels. */
    RVertex tw[3];
    for (int i = 0; i < 3; i++) { tw[i] = tri[i]; tw[i].x *= 2; tw[i].y *= 2; tw[i].z *= 2; tw[i].w = 2; tw[i].color = 0x0000FF00u; }
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_draw(&full, RENDERER_DEPTH_TEST, tw, 3, ti, 3);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && px[(H / 2) * W + W / 2] == 0x0000FF00u && px[0] == 0, "perspective divide by w");

    const RVertex nearq[4] = {{-1, -1, 0.2f, 1, 0x000000FFu, 0, {{0}}, 0}, {1, -1, 0.2f, 1, 0x000000FFu, 0, {{0}}, 0}, {1, 1, 0.2f, 1, 0x000000FFu, 0, {{0}}, 0}, {-1, 1, 0.2f, 1, 0x000000FFu, 0, {{0}}, 0}};
    RVertex farq[4];
    for (int i = 0; i < 4; i++) { farq[i] = nearq[i]; farq[i].z = 0.8f; farq[i].color = 0x00FFFF00u; }
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_draw(&full, RENDERER_DEPTH_TEST, nearq, 4, qi, 6);
    renderer_draw(&full, RENDERER_DEPTH_TEST, farq, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && mismatches(px, 0x000000FFu, &first) == 0, "depth test: the nearer quad wins when drawn first");
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_draw(&full, RENDERER_DEPTH_TEST, farq, 4, qi, 6);
    renderer_draw(&full, RENDERER_DEPTH_TEST, nearq, 4, qi, 6);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_draw(&full, RENDERER_DEPTH_TEST, farq, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && mismatches(px, 0x00FFFF00u, &first) == 0, "a depth clear between draws resets the test");
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_draw(&full, RENDERER_DEPTH_TEST, nearq, 4, qi, 6);
    renderer_draw(&full, 0, farq, 4, qi, 6);
    renderer_draw(&full, RENDERER_DEPTH_TEST, tri, 3, ti, 3);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && px[0] == 0x00FFFF00u && px[(H / 2) * W + W / 2] == 0x00FFFF00u,
          "without depth test a far draw overwrites and leaves the depth buffer unchanged (%08x)", px[(H / 2) * W + W / 2]);
    /* Depth clamp: a quad beyond the far plane (z/w = 1.5) is drawn at depth 1, not clipped. */
    RVertex beyond[4];
    for (int i = 0; i < 4; i++) { beyond[i] = nearq[i]; beyond[i].z = 1.5f; beyond[i].color = 0x00808080u; }
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_draw(&full, RENDERER_DEPTH_TEST, beyond, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && mismatches(px, 0x00808080u, &first) == 0, "depth beyond the far plane is clamped, not clipped");
    const RViewport sub = {W / 2, 0, W / 2, H / 2, 0, 1};
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_draw(&sub, RENDERER_DEPTH_TEST, nearq, 4, qi, 6);
    r = renderer_end(px, W * 4);
    CHECK(r == 0 && px[2 * W + W - 3] == 0x000000FFu && px[(H / 2 - 3) * W + W / 2 + 2] == 0x000000FFu && px[2 * W + 2] == 0 &&
              px[(H - 3) * W + W - 3] == 0,
          "viewport limits the draw to the top-right quarter");

    test_textures();
    test_state();
    test_combiners();
    test_surfaces();
    test_texture_budget();
    renderer_shutdown();
    test_scaled();
    test_present_fallback();
    test_glass();
    test_frames_in_flight();
    test_shadow_quality();
    test_shadow_range();
    test_supersample();
    test_sharpen();
    test_bloom();
    test_rain();
    test_rain_lit();
    test_rain_cover();
    test_ssao();
    test_rain_splash();
    test_rain_wet();
    test_rain_wet_scene();
    test_rain_cover_passes();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
