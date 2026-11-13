/* Game-independent test of the Vulkan renderer (src/renderer.h): clear + readback parity with
 * the CPU clear path (XRGB8888 bytes B, G, R, X), the initial color upload and the order of
 * clears, draws, textures, output state and the register-combiner pixel shader. Exits 77 (skip)
 * when no Vulkan device can be initialized. */
#include "../src/renderer.h"
#include "../src/platform.h"
#include "../src/gpu_skin.h"
#include "../src/fps_overlay.h"
#include "../src/lamp_shadow.h"
#include "../src/fsr.h"
#include "../src/gamma_ramp.h"
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
        /* BC7 (texture-pack .dds) with levels smaller than a block: 4x4 red, 2x2 green, 1x1 blue,
         * one mode 6 block each (solid endpoint 0); minified, the 1x1 level is sampled. */
        const uint8_t bc7[48] = {0xC0, 0xFF, 0x1F, 0x00, 0x00, 0x00, 0xFE, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0,
                                 0x40, 0x00, 0xE0, 0xFF, 0x07, 0x00, 0xFE, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0,
                                 0x40, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0};
        uint32_t b7 = renderer_texture_create(0x2222, RENDERER_TEX_BC7, 4, 4, 3, bc7);
        renderer_begin(NULL, 0);
        renderer_set_texture(b7);
        quad(q, 0, 0, 256, 256, 0xFFFFFFFFu);
        renderer_draw(&full, 0, q, 4, qi, 6);
        r = renderer_end(px, W * 4);
        CHECK(b7 && r == 0 && dist(px[(H / 2) * W + W / 2], 0x0000FFu) < 12, "BC7 chain with 2x2 and 1x1 levels: last level blue (%06x)",
              px[(H / 2) * W + W / 2] & 0xFFFFFFu);
        renderer_begin(NULL, 0);
        renderer_set_texture(b7);
        quad(q, 0, 0, 1, 1, 0xFFFFFFFFu);
        renderer_draw(&full, 0, q, 4, qi, 6);
        r = renderer_end(px, W * 4);
        CHECK(r == 0 && dist(px[(H / 2) * W + W / 2], 0xFF0000u) < 12, "BC7 level 0 samples red (%06x)",
              px[(H / 2) * W + W / 2] & 0xFFFFFFu);
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

/* --lamp-shadows (RendererConfig.lamp_shadow, renderer_lamp_shadows): a light 100 units above z = 0
 * looks straight down (s = 0.5 q + 10 x, t = 0.5 q + 10 y, q = 100 - z, depth range 90-110 over
 * D16); the 32x16 shadow map holds a character column at x in [-0.5, 0.5], 2 units tall. A lamp at
 * (-3, 0, 3) shadows the ground on the column's far side, not on its own side. */
#define LS_A (65535.0 * 110.0 / 20.0)
#define LS_B (LS_A * 90.0)
static void ls_prj(float m[4][4]) {
    const float r[4][4] = {{10, 0, -0.5f, 50}, {0, 10, -0.5f, 50}, {0, 0, (float)-LS_A, (float)(LS_A * 100 - LS_B)}, {0, 0, -1, 100}};
    memcpy(m, r, sizeof r);
}
/* The caster: a wall along y, `half` units either side of x = 0, 2 units tall, in a map of
 * w x h texels (key: a separate surface per size). */
static uint32_t ls_caster_box(uint32_t key, int w, int h, float half, float half_y);
static uint32_t ls_caster_sized(uint32_t key, int w, int h, float half) { return ls_caster_box(key, w, h, half, 0); }
/* A column `half` units either side of x = 0 and `half_y` either side of y = 0 (0: the whole map
 * along y, a wall), 2 units tall. */
static uint32_t ls_caster_box(uint32_t key, int w, int h, float half, float half_y) {
    static uint32_t frame[W * H];
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    uint32_t d = renderer_surface(key, RENDERER_SURFACE_DEPTH16, w, h);
    const RViewport svp = {0, 0, (float)w, (float)h, 0, 1};
    RVertex col[4];
    quad(col, 0, 0, 1, 1, 0xFF0000FFu);
    const float u = 10 * half / 98.0f, z = (float)((LS_A - LS_B / 98.0) / 65535.0);   /* the column's top: q = 98 */
    const float uy = 10 * half_y / 98.0f;
    for (int i = 0; i < 4; i++) {
        col[i].x = col[i].x < 0 ? -2 * u : 2 * u;
        if (half_y > 0) col[i].y = col[i].y < 0 ? -2 * uy : 2 * uy;
        col[i].z = z;
    }
    RState st;
    renderer_state_default(&st);
    st.color_mask = 0;
    renderer_begin(NULL, 0);
    renderer_set_target(0, d);
    renderer_clear(RENDERER_CLEAR_DEPTH, 0, 1.0f);
    renderer_set_state(&st);
    renderer_draw(&svp, RENDERER_DEPTH_TEST, col, 4, qi, 6);
    renderer_set_target(0, 0);
    renderer_state_default(&st);
    renderer_set_state(&st);
    renderer_end(frame, W * 4);
    return d;
}
static uint32_t ls_caster(void) { return ls_caster_sized(0x7201, 32, 16, 0.5f); }
/* The ground point (x, 0, 0) drawn with r0 = v0 (white) and stage 1 the shadow map (PROJECT3D,
 * compare func); lights: NULL = no renderer_lamp_shadows this frame. */
static uint32_t ls_shade_at(uint32_t surf, float x, float y, uint32_t func, const RLampShadows *lights, int average);
static uint32_t ls_shade(uint32_t surf, float x, uint32_t func, const RLampShadows *lights) {
    return ls_shade_at(surf, x, 0, func, lights, 0);
}
/* average: the mean over every pixel of the frame (all at the same ground point), else the centre. */
static uint32_t ls_shade_at(uint32_t surf, float x, float y, uint32_t func, const RLampShadows *lights, int average) {
    static uint32_t px[W * H];
    float m[4][4];
    ls_prj(m);
    const float p[4] = {x, y, 0, 1};
    float t1[4];
    for (int r = 0; r < 4; r++) t1[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] * p[3];
    RPixelShader ps = program(1);
    ps.rgb_in[0] = INS(IN(V0, UID), ONE, IN(ZERO, UID), IN(ZERO, UID));
    ps.rgb_out[0] = OUT(R0, ZERO, ZERO, 0);
    ps.tex_modes = 1 | 2 << 5;
    ps.shadow_func = func;
    const RViewport full = {0, 0, W, H, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    quad(q, 0, 0, 1, 1, 0xFFFFFFFFu);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) q[i].tex[1][k] = t1[k];
    renderer_begin(NULL, 0);
    if (lights) renderer_lamp_shadows(lights);
    renderer_set_stage_texture(0, 0);
    renderer_set_stage_texture(1, surf);
    renderer_set_pixel_shader(&ps);
    renderer_draw(&full, 0, q, 4, qi, 6);
    int r = renderer_end(px, W * 4);
    if (r) return 0xDEADBEEFu;
    if (!average) return px[(H / 2) * W + W / 2] & 0xFF;
    uint32_t sum = 0;
    for (int i = 0; i < W * H; i++) sum += px[i] & 0xFF;
    return (sum + W * H / 2) / (W * H);
}
static void test_lamp_shadows(void) {
    float m[4][4];
    ls_prj(m);
    const float eye[3] = {0, -10, 3};
    LampShadowLight lamp = {{-3, 0, 3}, 12, 1};
    RLampShadows in, small;
    lamp_shadow_setup((const float (*)[4])m, &lamp, 1, eye, 0.6f, &in);
    lamp.radius = 4;
    lamp_shadow_setup((const float (*)[4])m, &lamp, 1, eye, 0.6f, &small);
    RendererConfig c = {.samples = 1, .lamp_shadow = 1};
    renderer_configure(&c);
    if (renderer_init(W, H)) {
        CHECK(0, "renderer_init with lamp shadows: %s", renderer_error());
        return;
    }
    CHECK(renderer_lamp_shadows_on(), "lamp shadows on");
    uint32_t d = ls_caster();
    const uint32_t far_side = ls_shade(d, 2, 4, &in), count = renderer_lamp_shadows_count();
    const uint32_t lamp_side = ls_shade(d, -1.5f, 4, &in);
    const uint32_t fading = ls_shade(d, 3.5f, 4, &in);
    const uint32_t beyond = ls_shade(d, 9, 4, &in);
    const uint32_t out_of_range = ls_shade(d, 2, 4, &small);
    const uint32_t in_dir_shadow = ls_shade(d, 2, 0, &in);
    const uint32_t none = ls_shade(d, 2, 4, NULL);
    CHECK(far_side >= 0x60 && far_side <= 0x6C && count == 1, "far side of the character: 1 - 0.6 (%02x, %u light)", far_side, count);
    CHECK(lamp_side == 0xFF, "the lamp's side of the character stays lit (%02x)", lamp_side);
    CHECK(fading > far_side + 0x10 && fading < 0xF0, "shadow fades with the distance from the character (%02x)", fading);
    CHECK(beyond == 0xFF, "beyond the shadow's length: lit (%02x)", beyond);
    CHECK(out_of_range == 0xFF, "light out of range (radius 4): lit (%02x)", out_of_range);
    CHECK(in_dir_shadow >= 0xAE && in_dir_shadow <= 0xB8, "inside the directional shadow: half the darkening (%02x)", in_dir_shadow);
    CHECK(none == 0xFF && renderer_lamp_shadows_count() == 0, "a frame without lights: unshadowed (%02x)", none);
    /* One lamp per character, chosen at the character: its
     * shadow points away from the lamp nearest to it, wherever the shadow falls. Choosing the lamp per
     * ground point gave each part of the ground around a character a different lamp, so a character
     * between several lamps got one shadow per lamp (S7: "mas de 8 sombras"), or none at all. The
     * caster is a character's width (0.2 units, the fine map of shadow-scale 4 at 1080p). */
    const uint32_t slim = ls_caster_sized(0x7202, 1024, 512, 0.1f);
    LampShadowLight two[2] = {{{-3, 0, 3}, 12, 1}, {{4, 0, 3}, 12, 1}};
    RLampShadows pair, weak;
    lamp_shadow_setup((const float (*)[4])m, two, 2, eye, 0.6f, &pair);
    two[1].pos[0] = 8;
    lamp_shadow_setup((const float (*)[4])m, two, 2, eye, 0.6f, &weak);
    const uint32_t nearest = ls_shade(slim, 2, 4, &pair), not_other = ls_shade(slim, -1.5f, 4, &pair);
    const uint32_t undiluted = ls_shade(slim, 2, 4, &weak);
    CHECK(nearest >= 0x60 && nearest <= 0x6C, "the character's nearest lamp casts its shadow, though the ground there is nearer the other lamp (%02x)", nearest);
    CHECK(not_other == 0xFF, "the farther lamp casts no second shadow on the other side (%02x)", not_other);
    CHECK(undiluted >= 0x60 && undiluted <= 0x6C, "a weaker lamp in reach: the dominant lamp's full shadow (%02x)", undiluted);
    /* Two high lamps on either side, about as bright at every ground point around the character (the
     * hideout's ceiling lamps): one shadow, from the one nearer the character, not one per lamp. */
    LampShadowLight high[2] = {{{-3, 0, 8}, 20, 1}, {{4, 0, 8}, 20, 1}};
    RLampShadows ceiling;
    lamp_shadow_setup((const float (*)[4])m, high, 2, eye, 0.6f, &ceiling);
    const uint32_t from_near = ls_shade(slim, 0.8f, 4, &ceiling), from_far = ls_shade(slim, -0.8f, 4, &ceiling);
    CHECK(from_near <= 0x80, "high lamps: the nearer lamp's shadow (%02x)", from_near);
    CHECK(from_far >= 0xF0, "high lamps: no second shadow from the farther lamp (%02x)", from_far);
    /* Exactly between two equal lamps: the crossfade point, each lamp's shadow at half strength (the
     * darkening of one shadow shared), not two full shadows. */
    high[1].pos[0] = 3;
    lamp_shadow_setup((const float (*)[4])m, high, 2, eye, 0.6f, &ceiling);
    const uint32_t tie_a = ls_shade(slim, 0.8f, 4, &ceiling), tie_b = ls_shade(slim, -0.8f, 4, &ceiling);
    CHECK(tie_a >= 0xA8 && tie_a <= 0xBC && tie_b >= 0xA8 && tie_b <= 0xBC,
          "between two equal lamps: two half shadows, not two full ones (%02x, %02x)", tie_a, tie_b);
    /* A light at the receiver's height (a light pool sprite on the ground, 0.15 above it) throws no
     * character shadow: its rays run along the ground under every character. */
    LampShadowLight pool = {{-3, 0, 0.15f}, 12, 1};
    RLampShadows low;
    lamp_shadow_setup((const float (*)[4])m, &pool, 1, eye, 0.6f, &low);
    const uint32_t from_pool = ls_shade(d, 2, 4, &low);
    CHECK(from_pool == 0xFF, "a light on the ground casts no shadow (%02x)", from_pool);
    /* One shadow per character, not a row of copies (S6): a thin caster (0.2 units across, as a
     * character's top-down outline is in the light's view) in a fine map (shadow-scale 4 at 1080p:
     * texels of 0.01 units). The march's steps were 0.3 units apart, wider than the caster, so the
     * ground behind it was shadowed only where a step landed on it: lit gaps between dark copies.
     * Every ground point from just behind the caster to 2 units away is shadowed. */
    {
        const uint32_t thin = slim;
        LampShadowLight side = {{-6, 0, 3}, 12, 1};
        RLampShadows far;
        lamp_shadow_setup((const float (*)[4])m, &side, 1, eye, 0.6f, &far);
        /* Shadowed all along and fading smoothly with the distance (the light's weight falls off): no
         * point brighter than the next one further out by more than a rounding step. */
        int gaps = 0;
        float first_gap = 0;
        uint32_t gap_shade = 0, prev = 0;
        for (int k = 0; k <= 34; k++) {
            const float x = 0.3f + 0.05f * (float)k;
            const uint32_t v = ls_shade(thin, x, 4, &far);
            if (v >= 0xC0 || (k && v + 0x08 < prev) || (k && v > prev + 0x08)) {
                if (!gaps++) first_gap = x, gap_shade = v;
            }
            prev = v;
        }
        CHECK(gaps == 0, "a thin caster in a fine map: one continuous shadow behind it (%d jumps, first at x %.2f: %02x)",
              gaps, first_gap, gap_shade);
    }
    /* One shadow, not a chain of copies (S7): a character-sized column (0.2 x 0.2 units) and a low
     * lamp beside it. The march samples the ray at points; with the same points for every pixel, the
     * shadow was the union of one copy of the column's outline per step, and its sides a chain of
     * notches (lit dents between the copies) that read as several overlapping shadows. Each pixel
     * now offsets its steps differently: averaged over the pixels (supersampling), the shade along
     * the shadow's side changes smoothly, without dents. */
    {
        const uint32_t column = ls_caster_box(0x7203, 1024, 512, 0.1f, 0.1f);
        LampShadowLight low_lamp = {{-2, 0, 2.5f}, 12, 1};
        RLampShadows side;
        lamp_shadow_setup((const float (*)[4])m, &low_lamp, 1, eye, 0.6f, &side);
        /* Ground points on the shadow's side (half-width 0.1 x (x + 2) / 2, about half shadowed), and
         * well inside it. */
        int dents = 0;
        float first = 0;
        uint32_t v[41];
        for (int k = 0; k <= 40; k++) {
            const float x = 0.4f + 0.04f * (float)k;
            v[k] = ls_shade_at(column, x, 0.1f * (x + 2) / 2, 4, &side, 1);
            if (k && (v[k] > v[k - 1] + 0x0C || v[k] + 0x0C < v[k - 1]) && !dents++) first = x;
        }
        const uint32_t inside = ls_shade_at(column, 1.0f, 0.05f, 4, &side, 1);
        CHECK(dents == 0 && inside >= 0x60 && inside <= 0x6C,
              "a column's shadow: one smooth side, no dents between copies (%d jumps, first at x %.2f; inside %02x)", dents, first, inside);
    }
    renderer_shutdown();
    /* Off: the call is ignored. */
    RendererConfig off = {.samples = 1};
    renderer_configure(&off);
    if (!renderer_init(W, H)) {
        d = ls_caster();
        CHECK(!renderer_lamp_shadows_on() && ls_shade(d, 2, 4, &in) == 0xFF, "off: no lamp shadow");
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
/* --direct-geometry (RendererConfig.direct_geometry): vertices written straight into the frame's
 * mapped geometry buffer give the same frames as the host copy, including frames that outgrow the
 * buffer mid-frame (the vertices so far move to the host copy). Each frame: a grid of quads in distinct colors, growing zero-area filler
 * (up to several MiB), more quads after it. */
static void test_direct_geometry(void) {
    enum { FRAMES = 10, GX = 8, GY = 6 };
    static uint32_t px[2][FRAMES][W * H];
    static RVertex filler[80000];
    static uint32_t filler_idx[3 * 26000];
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    const RViewport full = {0, 0, W, H, 0, 1};
    for (unsigned i = 0; i < sizeof filler_idx / sizeof *filler_idx; i++) filler_idx[i] = i % 3;   /* zero-area triangles */
    int ok_init = 1;
    for (int d = 0; d < 2; d++) {
        RendererConfig c = {.samples = 1, .direct_geometry = d};
        renderer_configure(&c);
        if (renderer_init(W, H)) {
            CHECK(0, "renderer_init (direct geometry %d): %s", d, renderer_error());
            ok_init = 0;
            break;
        }
        for (int f = 0; f < FRAMES; f++) {
            renderer_begin(NULL, 0);
            renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0x00102030u, 1.0f);
            for (int g = 0; g < GX * GY; g++) {
                if (g == GX * GY / 2) {   /* the filler in the middle of the frame: 1/3 MiB more every frame */
                    const uint32_t n = 4000u * (uint32_t)(f + 1) < 80000 ? 4000u * (uint32_t)(f + 1) : 80000;
                    renderer_draw(&full, 0, filler, n, filler_idx, n - n % 3);
                }
                RVertex q[4];
                const float x0 = (float)(g % GX) / GX * 2 - 1, y0 = 1 - (float)(g / GX) / GY * 2;
                quad(q, 0, 0, 1, 1, 0xFF000000u | (uint32_t)(g * 5 + f) << 16 | (uint32_t)(g * 3) << 8 | (uint32_t)(f * 20));
                for (int i = 0; i < 4; i++) {
                    q[i].x = q[i].x < 0 ? x0 : x0 + 2.0f / GX;
                    q[i].y = q[i].y > 0 ? y0 : y0 - 2.0f / GY;
                }
                renderer_draw(&full, 0, q, 4, qi, 6);
            }
            CHECK(renderer_end(px[d][f], W * 4) == 0, "direct geometry %d frame %d rendered", d, f);
        }
        renderer_shutdown();
    }
    if (ok_init) {
        int bad = 0, drawn = 0;
        for (int f = 0; f < FRAMES; f++)
            for (int i = 0; i < W * H; i++) {
                bad += px[0][f][i] != px[1][f][i];
                drawn += (px[1][f][i] & 0xFFFFFFu) != 0x00102030u;
            }
        CHECK(bad == 0 && drawn == FRAMES * W * H, "direct geometry: frames identical to the host copy (%d pixels differ, %d of %d drawn)", bad,
              drawn, FRAMES * W * H);
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}

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

/* --fsr (RendererConfig.fsr_w / fsr_h): the scene renders at the internal size (surfaces follow
 * it), EASU + RCAS upscale it to the output size that renderer_end reads back; a flat frame stays
 * flat, an edge stays an edge at the right place, supersampling and CAS are off, the gamma ramp
 * and the frame counter apply to the upscaled frame, and an internal size not below the output
 * leaves it off. */
static void test_fsr(void) {
    enum { OW = W * 2, OH = H * 2 };
    static uint32_t px[OW * OH], base[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) base[y * W + x] = x < W / 2 ? 0x00404040u : 0x00B0B0B0u;
    int rw = 0, rh = 0;
    fsr_render_size(FSR_PERFORMANCE, OW, OH, &rw, &rh);
    CHECK(rw == W && rh == H, "fsr performance: %dx%d renders at %dx%d", OW, OH, rw, rh);
    for (int k = 0; k < 3; k++) {   /* 0: plain, 1: with --supersample 2 --sharpen 60 asked, 2: MSAA 4 */
        RendererConfig c = {.out_w = OW, .out_h = OH, .samples = k == 2 ? 4 : 1, .supersample = k == 1 ? 2 : 0,
                            .sharpen = k == 1 ? 60 : 0, .fsr_w = rw, .fsr_h = rh, .fsr_sharpness = FSR_SHARPNESS_DEFAULT};
        renderer_configure(&c);
        if (renderer_init(W, H)) {
            CHECK(0, "renderer_init with fsr (%d): %s", k, renderer_error());
            continue;
        }
        int ow = 0, oh = 0, iw = 0, ih = 0, sw = 0, sh = 0;
        renderer_output_size(&ow, &oh);
        const int on = renderer_fsr(&iw, &ih);
        CHECK(on && ow == OW && oh == OH && iw == W && ih == H, "fsr (%d): in use %d, output %dx%d, internal %dx%d", k, on, ow, oh, iw, ih);
        CHECK(renderer_supersample() == 1 && renderer_sharpen() == 0, "fsr (%d): no supersampling (%d), no CAS (%d)", k,
              renderer_supersample(), renderer_sharpen());
        renderer_surface_size(renderer_surface(0x7003, RENDERER_SURFACE_DEPTH16, 32, 16), &sw, &sh);
        CHECK(sw == 32 && sh == 16, "fsr (%d): surfaces follow the internal size (%dx%d)", k, sw, sh);
        renderer_begin(NULL, 0);
        renderer_clear(RENDERER_CLEAR_COLOR, 0x00336699u, 1.0f);
        int r = renderer_end(px, OW * 4), bad = 0;
        for (int i = 0; i < OW * OH; i++) bad += dist(px[i], 0x00336699u) > 1;
        CHECK(r == 0 && bad == 0, "fsr (%d): a clear reads back flat at the output size (%d off by more than 1)", k, bad);
        /* A vertical step edge at x = W / 2 internal = OW / 2 output: flat away from it, darker /
         * brighter overshoot from RCAS at most a few pixels around it. */
        renderer_begin(base, W * 4);
        r = renderer_end(px, OW * 4);
        int flat_bad = 0;
        for (int y = 0; y < OH; y++)
            for (int x = 0; x < OW; x++) {
                const uint32_t p = px[y * OW + x];
                if (x < OW / 2 - 4) flat_bad += dist(p, 0x00404040u) > 1;
                else if (x >= OW / 2 + 4) flat_bad += dist(p, 0x00B0B0B0u) > 1;
            }
        const uint32_t l = px[(OH / 2) * OW + OW / 2 - 3], h = px[(OH / 2) * OW + OW / 2 + 2];
        CHECK(r == 0 && flat_bad == 0 && (l & 0xFF) < 0x80 && (h & 0xFF) > 0x80,
              "fsr (%d): the step stays at the middle, flat beside it (%d bad; %06x | %06x)", k, flat_bad, l, h);
        renderer_shutdown();
    }
    /* RCAS: sharpness 0 (strongest) overshoots at the edge more than 2 (weakest). */
    int over[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        RendererConfig c = {.out_w = OW, .out_h = OH, .samples = 1, .fsr_w = rw, .fsr_h = rh, .fsr_sharpness = k ? 2.0f : 0.0f};
        renderer_configure(&c);
        if (renderer_init(W, H)) {
            CHECK(0, "renderer_init with fsr sharpness %d: %s", k * 2, renderer_error());
            continue;
        }
        renderer_begin(base, W * 4);
        renderer_end(px, OW * 4);
        for (int y = 0; y < OH; y++)
            for (int x = 0; x < OW; x++) {
                const int v = (int)(px[y * OW + x] & 0xFF);
                over[k] += v < 0x40 ? 0x40 - v : v > 0xB0 ? v - 0xB0 : 0;
            }
        renderer_shutdown();
    }
    CHECK(over[0] > over[1], "fsr sharpness 0 overshoots more than 2 (%d > %d)", over[0], over[1]);
    /* The gamma ramp (an inverting table) applies to the upscaled frame; the frame counter is drawn
     * at the output size in its corner. */
    {
        RendererConfig c = {.out_w = OW, .out_h = OH, .samples = 1, .fsr_w = rw, .fsr_h = rh, .fsr_sharpness = FSR_SHARPNESS_DEFAULT,
                            .fps_overlay = 1};
        renderer_configure(&c);
        if (renderer_init(W, H)) CHECK(0, "renderer_init with fsr and the gamma ramp: %s", renderer_error());
        else {
            uint32_t lut[256];
            for (int i = 0; i < 256; i++) lut[i] = (uint32_t)(255 - i) * 0x010101u;
            renderer_gamma_ramp(lut);
            renderer_fps_overlay_text("60 FPS");
            uint8_t g[FPS_MAX_CHARS];
            const FpsBox b = fps_overlay_box(OW, OH, 0, fps_overlay_glyphs("60 FPS", g, FPS_MAX_CHARS));
            renderer_begin(NULL, 0);
            renderer_clear(RENDERER_CLEAR_COLOR, 0x00336699u, 1.0f);
            int r = renderer_end(px, OW * 4), bad = 0, inbox = 0;
            for (int y = 0; y < OH; y++)
                for (int x = 0; x < OW; x++) {
                    const int in = x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
                    if (in) inbox += px[y * OW + x] != px[(OH - 1) * OW + OW - 1];
                    else bad += dist(px[y * OW + x], 0x00CC9966u) > 1;
                }
            CHECK(r == 0 && renderer_gamma_ramp_on() && bad == 0, "fsr: the gamma ramp inverts the upscaled frame (%d bad, %06x)", bad,
                  px[(OH - 1) * OW + OW - 1]);
            CHECK(renderer_fps_overlay_on() && inbox > 0 && b.x + b.w <= OW && b.y + b.h <= OH,
                  "fsr: the frame counter is drawn at the output size (%d pixels in its %dx%d box)", inbox, b.w, b.h);
            renderer_shutdown();
        }
    }
    /* An internal size not below the output (or above it) leaves FSR off: the plain renderer. */
    const int sizes[2][2] = {{OW, OH}, {OW + 2, OH}};
    for (int k = 0; k < 2; k++) {
        RendererConfig c = {.out_w = OW, .out_h = OH, .samples = 1, .fsr_w = sizes[k][0], .fsr_h = sizes[k][1]};
        renderer_configure(&c);
        if (renderer_init(W, H)) {
            CHECK(0, "renderer_init with fsr at the output size: %s", renderer_error());
            continue;
        }
        int ow = 0, oh = 0, iw = 0, ih = 0;
        renderer_output_size(&ow, &oh);
        const int on = renderer_fsr(&iw, &ih);
        CHECK(!on && ow == OW && oh == OH && iw == OW && ih == OH, "fsr %dx%d for %dx%d: off (%d), output %dx%d, internal %dx%d",
              sizes[k][0], sizes[k][1], OW, OH, on, ow, oh, iw, ih);
        renderer_shutdown();
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
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
static uint32_t cover_roof_flags;   /* extra renderer_draw flags of the roof (test_rain_chars) */
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
    renderer_draw(&full, RENDERER_DEPTH_TEST | RENDERER_RAIN_COVER | cover_roof_flags, q, 4, qi, 6);
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
/* light: an additive quad (ONE / ONE, depth tested and written: part of the opaque world, like the
 * title's neon) just in front of the wall beside the box's edge, x 162-199 of rows 100-149: it
 * sets the light mask (alpha), so the occlusion leaves it alone. */
static void ssao_frame_lit(int split, int hud, int light) {
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
    if (light) {
        RState add = opaque;
        add.blend_enable = 1;
        add.blend_src = add.blend_dst = 1;   /* ONE / ONE */
        add.blend_eq = 0x8006;
        renderer_set_state(&add);
        ssao_quad(q, 162, 100, 200, 150, 5.99f, 0xFF101010u);
        renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
        renderer_set_state(&opaque);
    }
    if (split) renderer_ssao(&ssao_proj);
    if (hud) {
        rect_quad(q, 150, 90, 190, 110, 0xFF0000FFu);   /* across the edge, no depth test */
        renderer_draw(&full, 0, q, 4, qi, 6);
    }
}
static void ssao_frame(int split, int hud) { ssao_frame_lit(split, hud, 0); }
static void test_ssao(void) {
    static uint32_t off[BW * BH], a[BW * BH], b[BW * BH], lit_off[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}, {4, 2}};   /* 4/2x: the Quality preset (working size 1/4) */
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
        ssao_frame_lit(0, 0, 1);
        r |= renderer_end(lit_off, BW * 4);
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
        ssao_frame_lit(1, 0, 1);
        r = renderer_end(b, BW * 4);
        int lit_diff = 0, lit_n = 0;
        for (int y = 102; y < 148; y++)
            for (int x = 164; x < 198; x++, lit_n++) lit_diff += b[y * BW + x] != lit_off[y * BW + x];
        CHECK(r == 0 && lit_diff == 0, "ssao %d/%dx: an additive (light-masked) surface is not darkened (%d of %d pixels differ, %d vs %d)", sm,
              ss, lit_diff, lit_n, (int)(b[130 * BW + 166] & 0xFF), (int)(lit_off[130 * BW + 166] & 0xFF));
        const int above = (int)(b[70 * BW + 166] & 0xFF), above_off = (int)(lit_off[70 * BW + 166] & 0xFF);
        CHECK(above < above_off - 8, "ssao %d/%dx: the unlit wall beside the box still darkens (%d vs %d)", sm, ss, above, above_off);
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

/* Soft particles (RendererConfig.soft, renderer_soft): an opaque wall at view depth 6 over x < 160
 * (the clear color, the far plane, right of it) and particle quads across both halves drawn after
 * the split with the depth test on and no depth writes: A (white, alpha 1, depth 5.8, flagged
 * RENDERER_SOFT) fades to 0.4 (0.2 / 0.5) over the wall and stays exact over the far plane; B
 * (depth 4, flagged) is two units in front: exact; C (depth 5.8, not flagged) keeps its hard
 * edge; D (additive ONE / ONE, grey, depth 5.8, flagged) adds 0.4 of its colour over the wall.
 * Ignored unless configured; a frame without renderer_soft() (or one that writes depth after it)
 * is byte-identical to the same frame without the option. */
static const RSoft soft_p = {1.01f, 0.101f, 0.5f};
enum { SOFT_LATE_OPAQUE = 1, SOFT_SSAO = 2, SOFT_BLOOM = 4 };
static void soft_frame(int mark, int flag, int extra) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    RState st;
    renderer_state_default(&st);
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0xFF204060u, 1.0f);
    renderer_set_state(&st);
    ssao_quad(q, 0, 0, 160, BH, 6, 0xFF808080u);
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    if (extra & SOFT_SSAO) renderer_ssao(&ssao_proj);
    if (mark) renderer_soft(&soft_p);
    const uint32_t sf = RENDERER_DEPTH_TEST | (flag ? RENDERER_SOFT : 0);
    st.blend_enable = 1;
    st.blend_src = 0x302;
    st.blend_dst = 0x303;
    st.depth_write = 0;
    renderer_set_state(&st);
    ssao_quad(q, 40, 20, 280, 60, 5.8f, 0xFFFFFFFFu);
    renderer_draw(&full, sf, q, 4, qi, 6);
    ssao_quad(q, 40, 80, 280, 120, 4, 0xFFFFFFFFu);
    renderer_draw(&full, sf, q, 4, qi, 6);
    ssao_quad(q, 40, 140, 280, 180, 5.8f, 0xFFFFFFFFu);
    renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    st.blend_src = st.blend_dst = 1;
    renderer_set_state(&st);
    ssao_quad(q, 40, 190, 280, 230, 5.8f, 0xFF404040u);
    renderer_draw(&full, sf, q, 4, qi, 6);
    if (extra & SOFT_LATE_OPAQUE) {
        renderer_state_default(&st);
        renderer_set_state(&st);
        ssao_quad(q, 300, 0, 320, 10, 3, 0xFF00FF00u);
        renderer_draw(&full, RENDERER_DEPTH_TEST, q, 4, qi, 6);
    }
    if (extra & SOFT_BLOOM) renderer_bloom();
}
static int soft_diff(const uint32_t *a, const uint32_t *b) {
    int n = 0;
    for (int i = 0; i < BW * BH; i++) n += a[i] != b[i];
    return n;
}
static void test_soft_particles(void) {
    static uint32_t off[BW * BH], a[BW * BH], late[BW * BH];
    const struct { int samples, ss, extra; } cfgs[] = {{1, 1, 0}, {4, 1, 0}, {1, 2, 0}, {4, 2, SOFT_SSAO | SOFT_BLOOM}};
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sm = cfgs[k].samples, ss = cfgs[k].ss, ex = cfgs[k].extra;
        RendererConfig c = {.samples = sm, .supersample = ss};
        if (ex) {
            c.ssao = 1;
            c.ssao_strength = 1;
            c.ssao_radius = 1;
            c.bloom = 1;
            c.bloom_strength = 0.5f;
            c.bloom_threshold = 0.9f;
        }
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init without soft particles: %s", renderer_error());
            continue;
        }
        CHECK(!renderer_soft_on(), "soft %d/%dx/%d: off unless configured", sm, ss, ex);
        soft_frame(0, 0, ex);
        int r = renderer_end(off, BW * 4);
        soft_frame(1, 1, ex);   /* renderer_soft() and the flags are ignored when off */
        r |= renderer_end(a, BW * 4);
        soft_frame(0, 0, ex | SOFT_LATE_OPAQUE);
        r |= renderer_end(late, BW * 4);
        int diff = soft_diff(a, off);
        CHECK(r == 0 && renderer_soft_count() == 0 && diff == 0, "soft %d/%dx/%d: off, the split and flags are ignored (%d pixels differ)", sm, ss,
              ex, diff);
        renderer_shutdown();
        c.soft = 1;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with soft particles: %s", renderer_error());
            continue;
        }
        CHECK(renderer_soft_on(), "soft %d/%dx/%d: in use", sm, ss, ex);
        soft_frame(0, 1, ex);
        r = renderer_end(a, BW * 4);
        diff = soft_diff(a, off);
        CHECK(r == 0 && renderer_soft_count() == 0 && diff == 0, "soft %d/%dx/%d: a frame without the split is unchanged (%d pixels differ)", sm,
              ss, ex, diff);
        soft_frame(1, 1, ex | SOFT_LATE_OPAQUE);
        r = renderer_end(a, BW * 4);
        diff = soft_diff(a, late);
        CHECK(r == 0 && renderer_soft_count() == 0 && diff == 0, "soft %d/%dx/%d: a depth write after the split cancels it (%d pixels differ)", sm,
              ss, ex, diff);
        soft_frame(1, 1, ex);
        r = renderer_end(a, BW * 4);
        CHECK(r == 0 && renderer_soft_count() == 3, "soft %d/%dx/%d: three soft draws executed (%u)", sm, ss, ex, renderer_soft_count());
        /* Expected over the wall: A 0.4 x 255 + 0.6 x 128, D 128 + 0.4 x 64; with bloom (its glow lifts the
         * wall) clearly below the hard particles. */
        const int fa = (int)(a[40 * BW + 100] & 0xFF), fd = (int)(a[210 * BW + 100] & 0xFF);
        const int ha = (int)(off[40 * BW + 100] & 0xFF), hd = (int)(off[210 * BW + 100] & 0xFF);
        const int ok_a = ex & SOFT_BLOOM ? fa < ha - 40 : fa > 176 && fa < 182, ok_d = ex & SOFT_BLOOM ? fd < hd - 25 : fd > 151 && fd < 157;
        CHECK(ok_a, "soft %d/%dx/%d: A fades in front of the wall (%d, expected 179; hard %d)", sm, ss, ex, fa, ha);
        CHECK(ok_d, "soft %d/%dx/%d: additive D fades in front of the wall (%d, expected 154; hard %d)", sm, ss, ex, fd, hd);
        int far_diff = 0, near_diff = 0, hard_diff = 0;
        for (int y = 24; y < 56; y++)
            for (int x = 170; x < 276; x++) far_diff += a[y * BW + x] != off[y * BW + x];
        for (int y = 84; y < 116; y++)
            for (int x = 44; x < 276; x++) near_diff += a[y * BW + x] != off[y * BW + x];
        for (int y = 144; y < 176; y++)
            for (int x = 44; x < 276; x++) hard_diff += a[y * BW + x] != off[y * BW + x];
        if (!(ex & SOFT_BLOOM)) {   /* bloom's glow spreads the faded A and D a little over their neighbours */
            CHECK(far_diff == 0, "soft %d/%dx/%d: A over the far plane keeps its exact color (%d pixels differ)", sm, ss, ex, far_diff);
            CHECK(near_diff == 0, "soft %d/%dx/%d: B two units in front keeps its exact color (%d pixels differ)", sm, ss, ex, near_diff);
            CHECK(hard_diff == 0, "soft %d/%dx/%d: unflagged C keeps its hard edge (%d pixels differ)", sm, ss, ex, hard_diff);
        }
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
        /* Puddles (RRainWet.puddles): patches of the floor darker than the wet ground alone, the
         * rest of the floor as it was, the wall and the HUD untouched. */
        static uint32_t pud[BW * BH];
        RRainWet wp = w;
        wp.puddles = 1;
        wp.time = 2.5f;
        for (int v = 0; v < 3; v++) wp.eye[v] = p.eye[v], wp.right[v] = p.right[v], wp.up_v[v] = p.up_v[v], wp.back[v] = p.back[v];
        wet_frame(&p, &wp, 1);
        r = renderer_end(pud, BW * 4);
        int pwall = 0, phud = 0, pdark = 0, psame = 0, pfloor = 0;
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < BW; x++) {
                const int i = y * BW + x;
                if (y < wall_rows - 4) pwall += pud[i] != off[i];
                else if (y >= wall_rows + 4 && y < 196) {
                    pfloor++;
                    pdark += luma(pud[i]) < luma(a[i]) - 6;
                    psame += pud[i] == a[i];
                }
            }
        for (int y = 200; y < 220; y++)
            for (int x = 20; x < 60; x++) phud += pud[y * BW + x] == 0x000000FFu;
        CHECK(r == 0 && pwall == 0 && phud == 40 * 20, "puddles %d/%dx: the wall and HUD untouched (%d, %d of %d)", sm, ss, pwall, phud, 40 * 20);
        CHECK(pdark * 20 > pfloor && psame * 4 > pfloor, "puddles %d/%dx: darker patches (%d) and untouched floor (%d) of %d", sm, ss, pdark, psame,
              pfloor);
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

/* A box standing on the floor (x cx-h..cx+h, y cy-h..cy+h, z 0..top): its front, sides and flat
 * top, drawn with flags (a character's legs and shoulders, flat enough to pass the masks). */
static void chars_box(const RRainSplash *p, float cx, float cy, float h, float top, uint32_t color, uint32_t flags) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const float c[4][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};   /* corners: x, y sign */
    RVertex v[8];
    for (int i = 0; i < 8; i++) splash_vertex(&v[i], p, cx + h * c[i % 4][0], cy + h * c[i % 4][1], i < 4 ? 0 : top, color);
    /* front (y -h), left (x -h), right (x +h), top */
    const uint32_t idx[24] = {0, 1, 5, 0, 5, 4, 3, 0, 4, 3, 4, 7, 1, 2, 6, 1, 6, 5, 4, 5, 6, 4, 6, 7};
    renderer_draw(&full, RENDERER_DEPTH_TEST | flags, v, 8, idx, 24);
}
/* The rain's character mask (RENDERER_CHARACTER): a dark box (a character) on the wet floor. With
 * the flag its own pixels get no wet ground and no splashes, and the floor around it is as wet as
 * the floor elsewhere (no dry rim or footprint); the same box without the flag is reported for
 * comparison. And the rain cover map leaves character draws out: a roof flagged as a character
 * covers nothing. */
static void test_rain_chars(void) {
    static uint32_t base[BW * BH], base_wet[BW * BH], dryf[BW * BH], wetf[BW * BH], both[BW * BH];
    static uint8_t chr[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}};
    const RRainSplash p = splash_cam(0.90630779f, -0.42261826f);   /* 25 degrees down */
    const RRainWet w = wet_params(&p);
    const float cx = 1.6f, cy = 3.0f, h = 0.2f, top = 1.2f;
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
        CHECK(renderer_rain_chars_on() && renderer_rain_splash_on(), "chars %d/%dx: the character mask is in use with rain", sm, ss);
        int r = 0;
        wet_frame(&p, NULL, 0);
        r |= renderer_end(base, BW * 4);
        wet_frame(&p, &w, 0);
        r |= renderer_end(base_wet, BW * 4);
        int rim_dry[2] = {0, 0}, rim_n = 0, in_wet[2] = {0, 0}, in_n = 0, splashed = 0;
        for (int flagged = 1; flagged >= 0; flagged--) {
            const uint32_t fl = flagged ? RENDERER_CHARACTER : 0;
            wet_frame(&p, NULL, 0);
            chars_box(&p, cx, cy, h, top, 0xFF303030u, fl);
            r |= renderer_end(dryf, BW * 4);
            wet_frame(&p, NULL, 0);
            chars_box(&p, cx, cy, h, top, 0xFF303030u, fl);
            renderer_rain_wet(&w);
            r |= renderer_end(wetf, BW * 4);
            if (flagged) {
                CHECK(r == 0 && renderer_rain_chars_count() == 1, "chars %d/%dx: one character draw in the mask (%u)", sm, ss, renderer_rain_chars_count());
                wet_frame(&p, NULL, 0);
                chars_box(&p, cx, cy, h, top, 0xFF303030u, fl);
                renderer_rain_splash(&p);
                renderer_rain_wet(&w);
                r |= renderer_end(both, BW * 4);
            }
            for (int i = 0; i < BW * BH; i++) chr[i] = dryf[i] != base[i];
            for (int y = 5; y < BH - 5; y++)
                for (int x = 5; x < BW - 5; x++) {
                    const int i = y * BW + x;
                    if (chr[i]) {   /* the box's inside (one pixel in from its outline) */
                        if (!(chr[i - 1] && chr[i + 1] && chr[i - BW] && chr[i + BW])) continue;
                        in_n += flagged;
                        in_wet[flagged] += wetf[i] != dryf[i];
                        if (flagged) splashed += both[i] != dryf[i];
                        continue;
                    }
                    int near = 0;   /* floor within 4 pixels of the box */
                    for (int dy = -4; dy <= 4 && !near; dy++)
                        for (int dx = -4; dx <= 4; dx++) near |= chr[i + dy * BW + dx];
                    if (!near) continue;
                    /* dry: darkened less than the same floor without the box (the box only hides
                     * reflections, which leaves the floor darker, not lighter) */
                    rim_n += flagged;
                    rim_dry[flagged] += luma(dryf[i]) - luma(wetf[i]) < luma(base[i]) - luma(base_wet[i]) - 6;
                }
        }
        CHECK(r == 0 && in_n > 100 && in_wet[1] == 0 && splashed == 0,
              "chars %d/%dx: the character's own pixels get no wet ground and no splashes (%d and %d of %d; without the flag %d wet)", sm, ss, in_wet[1],
              splashed, in_n, in_wet[0]);
        CHECK(rim_n > 100 && rim_dry[1] * 20 <= rim_n,
              "chars %d/%dx: no dry rim or footprint around it (%d of %d floor pixels within 4 less wet than without the box; without the flag %d)", sm, ss,
              rim_dry[1], rim_n,
              rim_dry[0]);
        renderer_shutdown();
    }
    /* The cover map: the roof of cover_frame flagged as a character covers nothing. */
    static uint32_t a[BW * BH];
    RendererConfig c = {.samples = 1, .rain = 1, .rain_cover = 1};
    renderer_configure(&c);
    if (renderer_init(BW, BH) == 0) {
        cover_frame(0);
        int r = renderer_end(a, BW * 4);
        const int open_l = cover_lit(a, 0, BW / 2 - 4);
        cover_roof_flags = RENDERER_CHARACTER;
        cover_frame(1);
        r |= renderer_end(a, BW * 4);
        cover_roof_flags = 0;
        const int left = cover_lit(a, 0, BW / 2 - 4);
        CHECK(r == 0 && open_l > 0 && left == open_l,
              "chars: the cover map leaves character draws out (left half %d of %d streaks with a character roof)", left, open_l);
        renderer_shutdown();
    } else {
        CHECK(0, "renderer_init with rain cover: %s", renderer_error());
    }
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
}


/* Contact shadows (renderer_contact): the box of test_rain_chars on the floor of wet_frame (a wall
 * behind). Off, or a frame without the op: the exact frame. With it: the floor right around the
 * box darkens, the open floor away from the box and wall keeps its exact colour, the box flagged
 * as a character keeps all its pixels (nothing on people), and the HUD drawn after it is
 * untouched. With the lamp lights' and SSAO's split it runs once in the same split. */
static void contact_frame(const RRainSplash *p, uint32_t box_flags, const RContact *cs, int ssao) {
    const RViewport full = {0, 0, BW, BH, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    wet_frame(p, NULL, 0);
    chars_box(p, 1.6f, 3.0f, 0.2f, 1.2f, 0xFF909090u, box_flags);
    if (cs) renderer_contact(cs);
    if (ssao) {
        const RSsao ao = {p->depth_a, p->depth_b, p->px, p->py};
        renderer_ssao(&ao);
    }
    rect_quad(q, 20, 200, 60, 220, 0xFF0000FFu);
    renderer_draw(&full, 0, q, 4, qi, 6);
}
static void test_contact(void) {
    static uint32_t off[BW * BH], nobox[BW * BH], a[BW * BH];
    static uint8_t box[BW * BH];
    const struct { int samples, ss; } cfgs[] = {{1, 1}, {4, 1}, {1, 2}};
    const RRainSplash p = splash_cam(0.90630779f, -0.42261826f);   /* 25 degrees down */
    const RContact cs = {.depth_a = p.depth_a, .depth_b = p.depth_b, .px = p.px, .py = p.py,
                         .up_view = {p.right[2], p.up_v[2], p.back[2]}, .radius = 0.6f, .strength = 0.8f};
    const int wall_rows = splash_wall_rows(&p);
    for (unsigned k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
        const int sm = cfgs[k].samples, ss = cfgs[k].ss;
        RendererConfig c = {.samples = sm, .supersample = ss};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init: %s", renderer_error());
            continue;
        }
        CHECK(!renderer_contact_on(), "contact %d/%dx: off without contact", sm, ss);
        contact_frame(&p, RENDERER_CHARACTER, NULL, 0);
        int r = renderer_end(off, BW * 4);
        contact_frame(&p, RENDERER_CHARACTER, &cs, 0);   /* ignored */
        r |= renderer_end(a, BW * 4);
        int diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_contact_count() == 0 && diff == 0, "contact %d/%dx: off, ignored (%d pixels differ)", sm, ss, diff);
        renderer_shutdown();
        c.contact = 1;
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with contact: %s", renderer_error());
            continue;
        }
        CHECK(renderer_contact_on(), "contact %d/%dx: in use with contact", sm, ss);
        contact_frame(&p, RENDERER_CHARACTER, NULL, 0);
        r = renderer_end(a, BW * 4);
        diff = 0;
        for (int i = 0; i < BW * BH; i++) diff += a[i] != off[i];
        CHECK(r == 0 && renderer_contact_count() == 0 && diff == 0, "contact %d/%dx: a frame without it is unchanged (%d pixels differ)", sm, ss, diff);
        wet_frame(&p, NULL, 0);
        {
            const RViewport full = {0, 0, BW, BH, 0, 1};
            const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
            RVertex q[4];
            rect_quad(q, 20, 200, 60, 220, 0xFF0000FFu);
            renderer_draw(&full, 0, q, 4, qi, 6);
        }
        r |= renderer_end(nobox, BW * 4);
        for (int i = 0; i < BW * BH; i++) box[i] = off[i] != nobox[i];
        for (int ssao = 0; ssao <= 1; ssao++) {
            contact_frame(&p, RENDERER_CHARACTER, &cs, ssao);
            r |= renderer_end(a, BW * 4);
            CHECK(r == 0 && renderer_contact_count() == 1, "contact %d/%dx/ssao %d: executed once (%u%s%s)", sm, ss, ssao, renderer_contact_count(),
                  r ? ", " : "", r ? renderer_error() : "");
            if (ssao) break;   /* SSAO darkens on its own: only the shared split is checked */
            int on_box = 0, box_n = 0, near_n = 0, darker = 0, open_diff = 0, hud = 0;
            for (int y = 5; y < BH - 5; y++)
                for (int x = 5; x < BW - 5; x++) {
                    const int i = y * BW + x;
                    if (box[i]) {   /* inside; its outline too without MSAA or supersampling (they blend the outline with the darkened floor behind) */
                        const int inside = box[i - 1] && box[i + 1] && box[i - BW] && box[i + BW];
                        if (inside || (sm == 1 && ss == 1)) {
                            box_n++;
                            on_box += a[i] != off[i];
                        }
                        continue;
                    }
                    if (y < wall_rows + 4 || (y >= 196 && x < 64)) continue;   /* the wall, its foot, the HUD */
                    int near = 0;
                    for (int dy = -5; dy <= 5 && !near; dy++)
                        for (int dx = -5; dx <= 5; dx++) near |= box[i + dy * BW + dx];
                    if (near) {
                        near_n++;
                        darker += luma(a[i]) < luma(off[i]) - 3;
                    } else if (y > wall_rows + 40 && x < 100) {
                        open_diff += a[i] != off[i];
                    }
                }
            for (int y = 200; y < 220; y++)
                for (int x = 20; x < 60; x++) hud += a[y * BW + x] == 0x000000FFu;
            CHECK(box_n > 100 && on_box == 0, "contact %d/%dx: the character keeps its pixels (%d of %d differ)", sm, ss, on_box, box_n);
            CHECK(near_n > 50 && darker * 3 > near_n, "contact %d/%dx: the floor around it darkens (%d of %d)", sm, ss, darker, near_n);
            CHECK(open_diff == 0, "contact %d/%dx: the open floor keeps its exact colour (%d pixels differ)", sm, ss, open_diff);
            CHECK(hud == 40 * 20, "contact %d/%dx: the HUD drawn after it is untouched (%d of %d)", sm, ss, hud, 40 * 20);
        }
        renderer_shutdown();
        /* Shared with SSAO: needs ssao configured too. */
        c.ssao = 1;
        c.ssao_strength = 0.8f;
        c.ssao_radius = 0.8f;
        renderer_configure(&c);
        if (renderer_init(BW, BH) == 0) {
            contact_frame(&p, RENDERER_CHARACTER, &cs, 1);
            r = renderer_end(a, BW * 4);
            CHECK(r == 0 && renderer_contact_count() == 1 && renderer_ssao_count() == 1, "contact %d/%dx: in SSAO's split (%u, %u%s%s)", sm, ss,
                  renderer_contact_count(), renderer_ssao_count(), r ? ", " : "", r ? renderer_error() : "");
            renderer_shutdown();
        }
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

/* --neon-glow with --world-lighting / --char-lighting: a masked draw that is also world-lit keeps
 * both (lit, then glowing: renderer_emission_count and renderer_world_lights_count count it), and an
 * emission stage other than 0 does not change a character draw (push word 3 is the light mask). */
static void test_neon_lit(void) {
    RendererConfig c = {.samples = 1, .world_light = 1, .char_light = 1};
    renderer_configure(&c);
    if (renderer_init(W, H)) {
        CHECK(0, "renderer_init (neon + world light): %s", renderer_error());
        RendererConfig def = {.samples = 1};
        renderer_configure(&def);
        return;
    }
    if (!renderer_char_lights_on()) {
        printf("skip neon lit: no lights\n");
        renderer_shutdown();
        RendererConfig def = {.samples = 1};
        renderer_configure(&def);
        return;
    }
    static uint8_t full[16], none[16];
    memset(full, 0xFF, sizeof full);
    const uint32_t mfull = renderer_texture_create(0x4E454F4E00000001ull, RENDERER_TEX_L8, 4, 4, 1, full);
    const uint32_t mnone = renderer_texture_create(0x4E454F4E00000002ull, RENDERER_TEX_L8, 4, 4, 1, none);
    RVertex q[4];
    quad(q, 0, 0, 1, 1, 0xFF202020u);
    const float nrm[4 * 3] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    const RViewport fv = {0, 0, W, H, 0, 1};
    RLampLights lights;
    memset(&lights, 0, sizeof lights);
    lights.px = lights.py = 1;
    lights.strength = lights.char_gain = lights.world_gain = 1;
    lights.n = 1;
    lights.l[0] = (RLamp){{0, 0, -0.5f}, 2, {1, 0.8f, 0.6f}, 1};
    static uint32_t px[5][W * H];
    uint32_t emitted[5], lit[5];
    for (int f = 0; f < 5; f++) {
        renderer_begin(NULL, 0);
        renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 1.0f);
        renderer_char_lights(&lights);
        renderer_set_pixel_shader(NULL);
        for (int s = 0; s < RENDERER_STAGES; s++) renderer_set_stage_texture(s, 0);
        if (f == 0) renderer_set_emission(mfull, 0, 1, 0);
        if (f == 2) renderer_set_emission(mnone, 1, 1, 0);
        if (f == 4) renderer_set_emission(mfull, 0, 1, 0);
        if (f < 2) renderer_draw_world_lit(&fv, 0, q, 4, qi, 6, nrm, 1);
        else if (f < 4) renderer_draw_lit(&fv, 0, q, 4, qi, 6, nrm);
        else renderer_draw(&fv, 0, q, 4, qi, 6);
        renderer_set_emission(0, 0, 0, 0);
        CHECK(renderer_end(px[f], W * 4) == 0, "neon lit frame %d rendered", f);
        emitted[f] = renderer_emission_count();
        lit[f] = renderer_world_lights_count();
    }
    renderer_shutdown();
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
    const uint32_t mid = (H / 2) * W + W / 2;
    CHECK(emitted[0] == 1 && lit[0] == 1, "a masked world-lit draw is both emissive and world-lit (%u emissive, %u world-lit)", emitted[0], lit[0]);
    CHECK(emitted[1] == 0 && lit[1] == 1, "without a mask it is only world-lit (%u, %u)", emitted[1], lit[1]);
    CHECK((px[1][mid] & 0xFF0000u) > 0x200000u, "the light brightens the plain draw (%06x)", px[1][mid] & 0xFFFFFFu);
    CHECK(dist(px[0][mid], 0xFFFFFFu) <= 2, "the full mask makes the lit draw glow white (%06x)", px[0][mid] & 0xFFFFFFu);
    CHECK(emitted[2] == 1, "a masked character draw is emissive (%u)", emitted[2]);
    int bad = 0;
    for (int i = 0; i < W * H; i++) bad += dist(px[2][i], px[3][i]) > 1;
    CHECK(bad == 0 && (px[3][mid] & 0xFF0000u) > 0x200000u, "an empty mask on stage 1 leaves the lit character as is (%d pixels differ; %06x vs %06x)",
          bad, px[2][mid] & 0xFFFFFFu, px[3][mid] & 0xFFFFFFu);
    CHECK(emitted[4] == 1 && dist(px[4][mid], 0xFFFFFFu) <= 2, "a masked plain draw glows (%u, %06x)", emitted[4], px[4][mid] & 0xFFFFFFu);
}

/* --normal-maps (renderer_set_relief) on lit draws: a flat normal map changes nothing; a map tilted
 * towards +X (red) or +Y (green, OpenGL convention: up the image, the title's v grows down) brightens
 * the side of the quad facing the light that way and darkens the other (the light sits in front of
 * the quad's centre); a specular map adds a highlight to world-lit and character draws; the baked
 * (vertex) light of a tilted texel darkens by (1 + cos) / 2 with --normal-baked 1; a plain draw
 * ignores the maps (renderer_relief_count counts the relief draws). */
static int chan_sum(uint32_t p) { return (int)(p >> 16 & 0xFF) + (int)(p >> 8 & 0xFF) + (int)(p & 0xFF); }
static void test_relief(void) {
    RendererConfig c = {.samples = 1, .world_light = 1, .char_light = 1};
    renderer_configure(&c);
    if (renderer_init(W, H)) {
        CHECK(0, "renderer_init (relief): %s", renderer_error());
        RendererConfig def = {.samples = 1};
        renderer_configure(&def);
        return;
    }
    if (!renderer_char_lights_on()) {
        printf("skip relief: no lights\n");
        renderer_shutdown();
        RendererConfig def = {.samples = 1};
        renderer_configure(&def);
        return;
    }
    static uint32_t flat[16], tx[16], ty[16], spec[16];
    for (int i = 0; i < 16; i++) {
        flat[i] = 0xFF8080FFu;   /* (128, 128, 255): straight out */
        tx[i] = 0xFFFF80B4u;     /* +X tilt */
        ty[i] = 0xFF80FFB4u;     /* +Y tilt */
        spec[i] = 0xFFFF8000u;   /* intensity 1, gloss 0.5 */
    }
    const uint32_t nflat = renderer_texture_create(0x52454C4600000001ull, RENDERER_TEX_BGRA8, 4, 4, 1, flat);
    const uint32_t ntx = renderer_texture_create(0x52454C4600000002ull, RENDERER_TEX_BGRA8, 4, 4, 1, tx);
    const uint32_t nty = renderer_texture_create(0x52454C4600000003ull, RENDERER_TEX_BGRA8, 4, 4, 1, ty);
    const uint32_t sp = renderer_texture_create(0x52454C4600000004ull, RENDERER_TEX_BGRA8, 4, 4, 1, spec);
    RVertex q[4];
    quad(q, 0, 0, 1, 1, 0xFF404040u);
    const float nrm[4 * 3] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    const RViewport fv = {0, 0, W, H, 0, 1};
    RLampLights lights;
    memset(&lights, 0, sizeof lights);
    lights.px = lights.py = 1;
    lights.strength = lights.char_gain = lights.world_gain = 1;
    lights.n = 2;
    lights.l[0] = (RLamp){{0, 0, -0.5f}, 2, {1, 0.8f, 0.6f}, 1};
    lights.l[1] = (RLamp){{0, 0, -50.0f}, 1, {1, 1, 1}, 1};   /* out of reach of the quad */
    enum { F = 11 };
    /* normal map, specular map, baked weight, mode (0 world mask 1, 1 character, 2 plain, 3 world mask 2) */
    const struct { uint32_t n, s; float baked; int mode; } fr[F] = {
        {0, 0, 0, 0}, {nflat, 0, 0, 0}, {ntx, 0, 0, 0}, {nty, 0, 0, 0}, {nflat, sp, 0, 0},
        {0, 0, 0, 1}, {0, sp, 0, 1}, {ntx, sp, 0, 2}, {0, 0, 0, 2}, {0, 0, 0, 3}, {ntx, 0, 1, 3}};
    static uint32_t px[F][W * H];
    uint32_t counted[F];
    for (int f = 0; f < F; f++) {
        renderer_begin(NULL, 0);
        renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 1.0f);
        renderer_char_lights(&lights);
        renderer_set_pixel_shader(NULL);
        for (int s = 0; s < RENDERER_STAGES; s++) renderer_set_stage_texture(s, 0);
        renderer_set_relief(fr[f].n, fr[f].s, 0, 1, 1, fr[f].baked);
        if (fr[f].mode == 0) renderer_draw_world_lit(&fv, 0, q, 4, qi, 6, nrm, 1);
        else if (fr[f].mode == 1) renderer_draw_lit(&fv, 0, q, 4, qi, 6, nrm);
        else if (fr[f].mode == 2) renderer_draw(&fv, 0, q, 4, qi, 6);
        else renderer_draw_world_lit(&fv, 0, q, 4, qi, 6, nrm, 2);
        renderer_set_relief(0, 0, 0, 0, 0, 0);
        CHECK(renderer_end(px[f], W * 4) == 0, "relief frame %d rendered", f);
        counted[f] = renderer_relief_count();
    }
    renderer_shutdown();
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
    /* pixel pairs mirrored about the centre (pixel x samples at x + 0.5) */
    const uint32_t mid = (H / 2) * W + W / 2, left = (H / 2) * W + W / 4, right = (H / 2) * W + 3 * W / 4 - 1;
    const uint32_t top = (H / 4) * W + W / 2, bottom = (3 * H / 4 - 1) * W + W / 2;
    CHECK(counted[0] == 0 && counted[1] == 1 && counted[5] == 0 && counted[6] == 1 && counted[7] == 0 && counted[10] == 1,
          "relief draws counted: lit draws with maps only (%u %u %u %u %u %u)", counted[0], counted[1], counted[5], counted[6], counted[7], counted[10]);
    int bad = 0;
    for (int i = 0; i < W * H; i++) bad += dist(px[0][i], px[1][i]) > 2;
    CHECK(bad == 0, "a flat normal map leaves the world-lit draw as is (%d pixels differ)", bad);
    CHECK(abs(chan_sum(px[0][left]) - chan_sum(px[0][right])) <= 3, "without relief the light is symmetric (%06x %06x)", px[0][left] & 0xFFFFFFu,
          px[0][right] & 0xFFFFFFu);
    CHECK(chan_sum(px[2][left]) > chan_sum(px[2][right]) + 20, "a +X tilt faces the left half to the light (left %06x, right %06x)",
          px[2][left] & 0xFFFFFFu, px[2][right] & 0xFFFFFFu);
    CHECK(chan_sum(px[3][bottom]) > chan_sum(px[3][top]) + 20, "a +Y (OpenGL, up the image) tilt faces the lower half to the light (bottom %06x, top %06x)",
          px[3][bottom] & 0xFFFFFFu, px[3][top] & 0xFFFFFFu);
    CHECK(chan_sum(px[4][mid]) > chan_sum(px[0][mid]) + 30, "a specular map adds a world-lit highlight (%06x over %06x)", px[4][mid] & 0xFFFFFFu,
          px[0][mid] & 0xFFFFFFu);
    CHECK(chan_sum(px[6][mid]) > chan_sum(px[5][mid]) + 30, "and a character highlight (%06x over %06x)", px[6][mid] & 0xFFFFFFu, px[5][mid] & 0xFFFFFFu);
    bad = 0;
    for (int i = 0; i < W * H; i++) bad += px[7][i] != px[8][i];
    CHECK(bad == 0, "a plain draw ignores the maps (%d pixels differ)", bad);
    /* +X map (255, 128, 180): the tilted normal's cos to the mesh normal is about 0.38, so the baked
     * light keeps (1 + 0.38) / 2 of itself */
    const int base = (int)(px[9][mid] >> 8 & 0xFF), dark = (int)(px[10][mid] >> 8 & 0xFF);
    CHECK(base >= 0x3E && base <= 0x42 && abs(dark - (int)(base * 0.69f + 0.5f)) <= 3, "--normal-baked 1 darkens the tilted baked light (%02x -> %02x)",
          base, dark);
}

/* --world-lighting with --gpu-vertex-programs (renderer_draw_skinned_world_lit): a draw whose
 * vertices the compute pre-pass computes, lit with host view normals, gives the pixels of
 * renderer_draw_world_lit of the same vertices (as the device left them); light mask 0 draws plain. */
static void test_skinned_world_lit(void) {
    RendererConfig c = {.samples = 1, .gpu_skin = 1, .world_light = 1};
    renderer_configure(&c);
    if (renderer_init(W, H)) {
        CHECK(0, "renderer_init (gpu skin + world light): %s", renderer_error());
        RendererConfig def = {.samples = 1};
        renderer_configure(&def);
        return;
    }
    if (!renderer_skin_on() || !renderer_char_lights_on()) {
        printf("skip skinned world lit: skinning %d, lights %d\n", renderer_skin_on(), renderer_char_lights_on());
        renderer_shutdown();
        RendererConfig def = {.samples = 1};
        renderer_configure(&def);
        return;
    }
    /* mov oPos, v0; mov oD0, v3 */
    VshProgram prog;
    memset(&prog, 0, sizeof prog);
    prog.n = 2;
    for (int i = 0; i < 2; i++) {
        VshIns *x = &prog.ins[i];
        x->mac = 1;   /* MOV */
        x->o_reg = 1;
        x->o_mask = 0xF;
        x->o_addr = i ? VSH_OD0 : VSH_OPOS;
        x->vreg = i ? 3 : 0;
        x->a = (VshSrc){2, 0, 0, {0, 1, 2, 3}};
    }
    static uint32_t words[VSH_MAX_INSTRUCTIONS][4];
    gsk_pack_program(&prog, words);
    static float consts[VSH_NCONST][4];
    struct { float p[4]; uint32_t c; } data[4];
    const float xy[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int i = 0; i < 4; i++) {
        data[i].p[0] = xy[i][0];
        data[i].p[1] = xy[i][1];
        data[i].p[2] = 0.5f;
        data[i].p[3] = 1;
        data[i].c = 0xFF404040u;
    }
    const GeomLayout lay = {{{0, 0, GEOM_FLOAT4, 0}, {3, 0, GEOM_D3DCOLOR, 16}}, 2, {20, 0, 0, 0}};
    GskJob j;
    memset(&j, 0, sizeof j);
    gsk_job_layout(&j, &lay);
    j.nins = prog.n;
    j.nv = 4;
    j.stride[0] = sizeof data[0];
    j.fog_mode = GSK_FOG_OFF;
    for (int k = 0; k < 8; k++) j.tscale[k] = 1;
    const float nrm[4 * 3] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    const RViewport full = {0, 0, W, H, 0, 1};
    RLampLights lights;
    memset(&lights, 0, sizeof lights);
    lights.px = lights.py = 1;
    lights.strength = lights.char_gain = lights.world_gain = 1;
    lights.n = 1;
    lights.l[0] = (RLamp){{0, 0, -0.5f}, 2, {1, 0.8f, 0.6f}, 1};
    static uint8_t lit_mask[16];
    memset(lit_mask, 0xFF, sizeof lit_mask);
    const uint32_t mfull = renderer_texture_create(0x4E454F4E00000003ull, RENDERER_TEX_L8, 4, 4, 1, lit_mask);
    static uint32_t px[4][W * H];
    uint32_t emitted = 0;
    static RVertex dv[4];
    float dn[4 * 3] = {0};
    int got_dv = 0, got_dn = 0;
    for (int f = 0; f < 4; f++) {
        renderer_begin(NULL, 0);
        renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0, 1.0f);
        renderer_char_lights(&lights);
        renderer_set_pixel_shader(NULL);
        if (f == 3) renderer_set_emission(mfull, 0, 1, 0);
        if (f == 1) renderer_draw_world_lit(&full, 0, dv, 4, qi, 6, nrm, 1);
        else renderer_draw_skinned_world_lit(&full, 0, &j, (const uint32_t (*)[4])words, (const float (*)[4])consts, (const uint8_t *)data,
                                             sizeof data, qi, 6, nrm, f == 0 || f == 3 ? 1u : 0u);
        renderer_set_emission(0, 0, 0, 0);
        CHECK(renderer_end(px[f], W * 4) == 0, "skinned world lit frame %d rendered", f);
        if (f == 3) emitted = renderer_emission_count();
        if (f == 0 && renderer_debug_vertices()) {
            memcpy(dv, renderer_debug_vertices(), sizeof dv);
            got_dv = 1;
            if (renderer_debug_normals()) {
                memcpy(dn, renderer_debug_normals(), sizeof dn);
                got_dn = 1;
            }
        }
    }
    renderer_shutdown();
    RendererConfig def = {.samples = 1};
    renderer_configure(&def);
    CHECK(got_dv && dv[0].x == -1 && dv[2].y == -1 && dv[1].w == 1, "the device computed the vertices (%g, %g, %g)", dv[0].x, dv[2].y, dv[1].w);
    CHECK(got_dn && !memcmp(dn, nrm, sizeof nrm), "the host normals are the draw's normals (%g %g %g)", dn[0], dn[1], dn[2]);
    int bad = 0;
    for (int i = 0; i < W * H; i++) bad += dist(px[0][i], px[1][i]) > 1;
    const uint32_t mid = (H / 2) * W + W / 2;
    CHECK(bad == 0, "skinned world lit = CPU world lit of the device's vertices (%d pixels differ; %06x vs %06x)", bad, px[0][mid] & 0xFFFFFFu,
          px[1][mid] & 0xFFFFFFu);
    CHECK(dist(px[2][mid], 0x404040u) <= 1, "mask 0 draws plain (%06x)", px[2][mid] & 0xFFFFFFu);
    CHECK((px[0][mid] & 0xFF0000u) > (px[2][mid] & 0xFF0000u) + 0x100000u, "the light brightens it (%06x over %06x)", px[0][mid] & 0xFFFFFFu,
          px[2][mid] & 0xFFFFFFu);
    CHECK(emitted == 1 && dist(px[3][mid], 0xFFFFFFu) <= 2, "a masked skinned world-lit draw glows (%u emissive, %06x)", emitted,
          px[3][mid] & 0xFFFFFFu);
}


/* --fps-overlay (RendererConfig.fps_overlay): the counter box is drawn last, on the read-back frame
 * (after the sharpening and the supersampling downsample) in the configured corner: ink pixels are
 * near white, the rest of the box is darkened (translucent), and every pixel outside the box is
 * exactly the frame. Off, or with no text, the frame is byte-identical. With a headless window
 * surface the swapchain path presents with it. */
static void test_fps_overlay(void) {
    static uint32_t px[BW * BH], base[BW * BH];
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++) base[y * BW + x] = 0x00B0B0B0u + (uint32_t)((x * 3 + y) & 0x0F);
    const char *text = "60 FPS  16.6 ms";
    /* corner 3: the longest text (--fps-overlay-items fps,ms,cpu,gpu), every new glyph, 37 characters */
    const char *longest = "999 FPS  999.9 ms  CPU 100%  GPU --";
    uint8_t g[FPS_MAX_CHARS];
    for (int k = 0; k < 6; k++) {
        const int corner = k & 3, ss = k == 4 ? 2 : 1, sharpen = k == 5 ? 60 : 0;
        const char *shown = k == 3 ? longest : text;
        const int n = fps_overlay_glyphs(shown, g, FPS_MAX_CHARS);
        RendererConfig c = {.samples = 1, .supersample = ss, .sharpen = sharpen, .fps_overlay = 1, .fps_overlay_corner = corner};
        renderer_configure(&c);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with the fps overlay: %s", renderer_error());
            continue;
        }
        CHECK(renderer_fps_overlay_on(), "fps overlay in use (corner %d, supersample %d, sharpen %d)", corner, ss, sharpen);
        renderer_fps_overlay_text(shown);
        renderer_begin(base, BW * 4);
        int r = renderer_end(px, BW * 4);
        const FpsBox b = fps_overlay_box(BW, BH, corner, n);
        int outside = 0, ink = 0, ink_bad = 0, bg = 0, bg_bad = 0;
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < BW; x++) {
                const uint32_t p = px[y * BW + x], want = base[y * BW + x];
                const int in = x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
                if (!in) {
                    outside += sharpen ? 0 : p != want;
                    continue;
                }
                const int fx = (x - b.x) / b.scale - FPS_PAD, fy = (y - b.y) / b.scale - FPS_PAD;
                int outline = 0;   /* no box: only the one-font-pixel ring around the ink is darkened */
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) outline |= fps_overlay_ink(g, n, fx + dx, fy + dy);
                if (fps_overlay_ink(g, n, fx, fy)) {
                    ink++;
                    ink_bad += (p & 0xFF) < 0xE8;
                } else if (outline) {
                    bg++;
                    bg_bad += (p & 0xFF) >= (want & 0xFF) / 2;
                } else {
                    outside += sharpen ? 0 : p != want;
                }
            }
        if (sharpen) {   /* the sharpened frame moves at its gradient: outside the box it matches a run without the overlay */
            static uint32_t ref[BW * BH];
            renderer_fps_overlay_text("");
            renderer_begin(base, BW * 4);
            renderer_end(ref, BW * 4);
            for (int y = 0; y < BH; y++)
                for (int x = 0; x < BW; x++)
                    if (!(x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)) outside += px[y * BW + x] != ref[y * BW + x];
        }
        CHECK(r == 0 && outside == 0 && ink > 100 && ink_bad == 0 && bg > 100 && bg_bad == 0,
              "fps overlay corner %d (ss %d, sharpen %d): box %d,%d %dx%d, %d ink (%d not white), %d outline (%d not darkened), %d changed outside",
              corner, ss, sharpen, b.x, b.y, b.w, b.h, ink, ink_bad, bg, bg_bad, outside);
        renderer_fps_overlay_text("");
        renderer_begin(base, BW * 4);
        r = renderer_end(px, BW * 4);
        int changed = 0;
        if (!sharpen)
            for (int i = 0; i < BW * BH; i++) changed += px[i] != base[i];
        CHECK(r == 0 && changed == 0, "fps overlay without text: the frame is unchanged (%d)", changed);
        renderer_shutdown();
    }
    /* off: the text is ignored and the frame is byte-identical */
    RendererConfig off = {.samples = 1};
    renderer_configure(&off);
    if (!renderer_init(BW, BH)) {
        renderer_fps_overlay_text(text);
        renderer_begin(base, BW * 4);
        int r = renderer_end(px, BW * 4), changed = 0;
        for (int i = 0; i < BW * BH; i++) changed += px[i] != base[i];
        CHECK(r == 0 && changed == 0 && !renderer_fps_overlay_on(), "fps overlay off: text ignored, frame byte-identical (%d)", changed);
        renderer_shutdown();
    }
    /* the swapchain path (headless surface when the loader has it) */
    static const char *ext[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    RendererConfig pc = {.samples = 1, .instance_ext = ext, .instance_ext_count = 2, .create_surface = headless_surface,
                         .drawable_size = headless_size, .fps_overlay = 1, .fps_overlay_corner = FPS_CORNER_BR};
    renderer_configure(&pc);
    if (!renderer_init(BW, BH) && renderer_can_present()) {
        renderer_fps_overlay_text(text);
        int bad = 0;
        for (int i = 0; i < 4; i++) {
            renderer_begin(base, BW * 4);
            bad += renderer_present() != 0;
        }
        renderer_begin(base, BW * 4);
        int r = renderer_end(px, BW * 4);
        CHECK(bad == 0 && r == 0, "fps overlay on the swapchain: 4 frames presented (%d failed, %s), read-back frame after them ok", bad,
              renderer_present_info());
    } else {
        printf("skip fps overlay swapchain path: %s\n", renderer_present_info());
    }
    renderer_shutdown();
    /* --fps-overlay-items gpu (RendererConfig.fps_overlay_gpu): each completed frame's GPU time,
     * read when its slot is recorded again (never a wait); no query pool without it */
    double busy = -1;
    int frames = -1;
    for (int fif = 1; fif <= 3; fif++) {
        RendererConfig gc = {.samples = 1, .frames_in_flight = fif, .fps_overlay = 1, .fps_overlay_gpu = 1};
        renderer_configure(&gc);
        if (renderer_init(BW, BH)) {
            CHECK(0, "renderer_init with fps overlay gpu: %s", renderer_error());
            continue;
        }
        int on = renderer_fps_overlay_gpu(&busy, &frames);
        CHECK(on && busy == 0 && frames == 0, "fps overlay gpu (%d in flight): timestamps in use, nothing completed yet", fif);
        renderer_fps_overlay_text(text);
        for (int i = 0; i < 8; i++) {
            renderer_begin(base, BW * 4);
            renderer_end(px, BW * 4);
        }
        renderer_fps_overlay_gpu(&busy, &frames);
        /* frame i's stamps are read when its slot is recorded again: 8 - fif of 8 frames counted */
        CHECK(frames == 8 - fif && busy > 0 && busy / frames < 50, "fps overlay gpu (%d in flight): %d frames, %.3f ms each", fif, frames,
              frames ? busy / frames : 0);
        renderer_fps_overlay_gpu(&busy, &frames);
        CHECK(busy == 0 && frames == 0, "fps overlay gpu: taken values start again from 0");
        renderer_shutdown();
    }
    RendererConfig ng = {.samples = 1, .fps_overlay = 1};
    renderer_configure(&ng);
    if (!renderer_init(BW, BH)) {
        renderer_begin(base, BW * 4);
        renderer_end(px, BW * 4);
        renderer_begin(base, BW * 4);
        renderer_end(px, BW * 4);
        CHECK(!renderer_fps_overlay_gpu(&busy, &frames) && busy == 0 && frames == 0, "fps overlay without gpu: no timestamps");
        renderer_shutdown();
    }
    renderer_configure(&off);
}

/* Pipeline cache (RendererConfig.pipeline_cache_dir): the files are written at shutdown, the next
 * init pre-warms the listed pipelines (taken instead of compiled, same pixels), a damaged cache
 * file is dropped and rewritten, no_pipeline_cache writes nothing. */
static int pc_frame(uint32_t *px) {   /* three draws in three pipelines (blend, cull, no depth) */
    const RViewport full = {0, 0, W, H, 0, 1};
    const RVertex tri[3] = {{-0.5f, -0.5f, 0.5f, 1, 0x80FF0000u, 0, {{0}}, 0}, {0.5f, -0.5f, 0.5f, 1, 0x8000FF00u, 0, {{0}}, 0},
                            {0, 0.5f, 0.5f, 1, 0x800000FFu, 0, {{0}}, 0}};
    const uint32_t ti[3] = {0, 1, 2};
    RState rs;
    renderer_begin(NULL, 0);
    renderer_clear(RENDERER_CLEAR_COLOR | RENDERER_CLEAR_DEPTH, 0x00203040u, 1.0f);
    renderer_state_default(&rs);
    rs.blend_enable = 1;
    rs.blend_src = 0x302;
    rs.blend_dst = 0x303;
    renderer_set_state(&rs);
    renderer_draw(&full, RENDERER_DEPTH_TEST, tri, 3, ti, 3);
    renderer_state_default(&rs);
    rs.cull = 0x900;
    renderer_set_state(&rs);
    renderer_draw(&full, RENDERER_DEPTH_TEST, tri, 3, ti, 3);
    renderer_state_default(&rs);
    rs.color_mask = 0x00010101u;
    rs.blend_enable = 1;
    rs.blend_src = rs.blend_dst = 1;
    renderer_set_state(&rs);
    renderer_draw(&full, 0, tri, 3, ti, 3);
    return renderer_end(px, W * 4);
}
static int pc_report_has(const char *what, int wait_ms) {   /* drains the [pipeline] lines */
    char line[512];
    int found = 0;
    for (int t = 0; t <= wait_ms && !found; t += 10) {
        while (renderer_pipeline_report(line, sizeof line)) {
            printf("     %s\n", line);
            if (strstr(line, what)) found = 1;
        }
        if (!found && t < wait_ms) {
            struct timespec d = {0, 10000000};
            nanosleep(&d, NULL);
        }
    }
    return found;
}
static long pc_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}
static void test_pipeline_cache(void) {
    static uint32_t a[W * H], b[W * H];
    char dir[512], cache[600], keys[600];
    snprintf(dir, sizeof dir, "%s/xbrun_renderer_test_pc", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    plat_mkdir(dir);
    snprintf(cache, sizeof cache, "%s/pipeline-cache.bin", dir);
    snprintf(keys, sizeof keys, "%s/pipeline-keys.bin", dir);
    remove(cache);
    remove(keys);
    uint32_t od = 0, warm = 0;
    RendererConfig c = {.samples = 1, .pipeline_cache_dir = dir};
    renderer_configure(&c);
    if (renderer_init(W, H)) {
        CHECK(0, "renderer_init with a pipeline cache: %s", renderer_error());
        return;
    }
    CHECK(pc_report_has("none yet", 0), "pipeline cache: no file at the first start");
    CHECK(pc_frame(a) == 0, "pipeline cache: cold frame");
    renderer_pipeline_counts(&od, &warm);
    CHECK(od >= 5 && warm == 0, "pipeline cache: cold, %u compiled on demand, %u pre-warmed", od, warm);
    renderer_shutdown();
    CHECK(pc_report_has("saved", 0) && pc_file_size(cache) > 24 && pc_file_size(keys) > 32, "pipeline cache: both files written (%ld, %ld bytes)",
          pc_file_size(cache), pc_file_size(keys));

    renderer_configure(&c);
    if (!renderer_init(W, H)) {
        CHECK(pc_report_has("loaded", 0), "pipeline cache: loaded at the second start");
        CHECK(pc_report_has("pre-warm:", 5000), "pipeline cache: pre-warm finished");
        CHECK(pc_frame(b) == 0 && !memcmp(a, b, sizeof a), "pipeline cache: warm frame identical to the cold one");
        renderer_pipeline_counts(&od, &warm);
        CHECK(warm >= 3 && od == 2, "pipeline cache: warm, %u pre-warmed taken, %u compiled on demand (the 2 at init)", warm, od);
        renderer_shutdown();
        pc_report_has("", 0);
    }

    FILE *f = fopen(cache, "r+b");   /* one payload bit flipped */
    if (f) {
        fseek(f, 40, SEEK_SET);
        int ch = fgetc(f);
        fseek(f, 40, SEEK_SET);
        fputc(ch ^ 0x10, f);
        fclose(f);
    }
    renderer_configure(&c);
    if (!renderer_init(W, H)) {
        CHECK(pc_report_has("corrupt", 0), "pipeline cache: a damaged file is dropped");
        CHECK(pc_frame(b) == 0 && !memcmp(a, b, sizeof a), "pipeline cache: frame after the dropped file identical");
        renderer_shutdown();
        CHECK(pc_report_has("saved", 0), "pipeline cache: rewritten at shutdown");
    }
    renderer_configure(&c);
    if (!renderer_init(W, H)) {
        CHECK(pc_report_has("loaded", 0), "pipeline cache: the rewritten file loads");
        renderer_shutdown();
        pc_report_has("", 0);
    }

    remove(cache);
    remove(keys);
    RendererConfig off = {.samples = 1, .pipeline_cache_dir = dir, .no_pipeline_cache = 1};
    renderer_configure(&off);
    if (!renderer_init(W, H)) {
        CHECK(pc_frame(b) == 0 && !memcmp(a, b, sizeof a), "no pipeline cache: same frame");
        renderer_shutdown();
        CHECK(pc_file_size(cache) < 0 && pc_file_size(keys) < 0 && !pc_report_has("[pipeline]", 0), "no pipeline cache: no files, no lines");
    }
    remove(cache);
    remove(keys);
    remove(dir);
    RendererConfig plain = {.samples = 1};
    renderer_configure(&plain);
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
    test_direct_geometry();
    test_skinned_world_lit();
    test_neon_lit();
    test_relief();
    test_shadow_quality();
    test_shadow_range();
    test_lamp_shadows();
    test_supersample();
    test_sharpen();
    test_fsr();
    test_fps_overlay();
    test_bloom();
    test_rain();
    test_rain_lit();
    test_rain_cover();
    test_ssao();
    test_soft_particles();
    test_rain_splash();
    test_rain_wet();
    test_rain_wet_scene();
    test_rain_chars();
    test_rain_cover_passes();
    test_contact();
    test_pipeline_cache();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
