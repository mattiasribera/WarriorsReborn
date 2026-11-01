/* Game-independent test of src/geom.c: declaration and FVF layouts (including the three
 * declaration shapes the title builds), element decoding, matrix
 * and constant-register transforms, skinning, screen-space vertices and primitive expansion. */
#include "../src/geom.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int near4(const float *a, float x, float y, float z, float w) {
    return fabsf(a[0] - x) < 1e-4f && fabsf(a[1] - y) < 1e-4f && fabsf(a[2] - z) < 1e-4f && fabsf(a[3] - w) < 1e-4f;
}
static int has(const GeomLayout *l, int reg, int stream, int type, int offset) {
    const GeomElement *e = geom_find(l, reg);
    return e && e->stream == stream && e->type == type && e->offset == offset;
}

static void test_layouts(void) {
    GeomLayout l;
    CHECK(geom_type_size(GEOM_FLOAT3) == 12 && geom_type_size(GEOM_NORMPACKED3) == 4 && geom_type_size(GEOM_D3DCOLOR) == 4 &&
              geom_type_size(GEOM_SHORT3) == 6 && geom_type_size(GEOM_NORMSHORT2) == 4 && geom_type_size(GEOM_PBYTE4) == 4 &&
              geom_type_size(GEOM_FLOAT2H) == 12 && geom_type_size(GEOM_NONE) == 0 && geom_type_size(0x99) == -1,
          "element type sizes");

    /* Skinned character declaration (title 0xb9460). */
    const uint32_t chr[] = {0x20000000, 0x40320000, 0x40160001, 0x40220002, 0x40220003,
                            0x20000001, 0x40350004, 0x40320005, 0xFFFFFFFF};
    int r = geom_layout_from_decl(chr, 16, &l);
    CHECK(r == 0 && l.n == 6 && has(&l, 0, 0, GEOM_FLOAT3, 0) && has(&l, 1, 0, GEOM_NORMPACKED3, 12) &&
              has(&l, 2, 0, GEOM_FLOAT2, 16) && has(&l, 3, 0, GEOM_FLOAT2, 24) && has(&l, 4, 1, GEOM_SHORT3, 0) &&
              has(&l, 5, 1, GEOM_FLOAT3, 6) && l.size[0] == 32 && l.size[1] == 18,
          "character declaration: two streams, offsets and sizes (n=%d, sizes %u/%u)", l.n, l.size[0], l.size[1]);
    /* World declarations: sizes match the observed SetStreamSource strides 0x1c / 0x24 / 0x2c. */
    const uint32_t w1[] = {0x20000000, 0x40320000, 0x40160001, 0x40400002, 0x40220003, 0xFFFFFFFF};
    const uint32_t w2[] = {0x20000000, 0x40320000, 0x40320001, 0x40400002, 0x40220003, 0xFFFFFFFF};
    const uint32_t w3[] = {0x20000000, 0x40320000, 0x40320001, 0x40400002, 0x40220003, 0x40220004, 0xFFFFFFFF};
    int s1 = geom_layout_from_decl(w1, 16, &l) ? -1 : l.size[0];
    int s2 = geom_layout_from_decl(w2, 16, &l) ? -1 : l.size[0];
    int s3 = geom_layout_from_decl(w3, 16, &l) ? -1 : l.size[0];
    CHECK(s1 == 0x1c && s2 == 0x24 && s3 == 0x2c && has(&l, 2, 0, GEOM_D3DCOLOR, 24),
          "world declarations: sizes %#x %#x %#x, color at 24", s1, s2, s3);
    /* SKIP (dwords) and SKIPBYTES advance the offset. */
    const uint32_t sk[] = {0x20000000, 0x40320000, 0x50020000, 0x58030000, 0x40220001, 0xFFFFFFFF};
    r = geom_layout_from_decl(sk, 16, &l);
    CHECK(r == 0 && l.n == 2 && has(&l, 1, 0, GEOM_FLOAT2, 12 + 8 + 3) && l.size[0] == 31, "SKIP(2) and SKIPBYTES(3)");
    const uint32_t bad[] = {0x20000000, 0x40990000, 0xFFFFFFFF}, noend[] = {0x20000000, 0x40320000};
    CHECK(geom_layout_from_decl(bad, 16, &l) == -1 && geom_layout_from_decl(noend, 2, &l) == -1,
          "unknown type and missing END are rejected");

    int rhw = -1;
    CHECK(geom_layout_from_fvf(0x142, &l, &rhw) == 0x18 && !rhw && has(&l, GEOM_REG_DIFFUSE, 0, GEOM_D3DCOLOR, 12) &&
              has(&l, GEOM_REG_TEX0, 0, GEOM_FLOAT2, 16) && !geom_find(&l, GEOM_REG_NORMAL),
          "FVF 0x142 (XYZ|DIFFUSE|TEX1): 24 bytes");
    CHECK(geom_layout_from_fvf(0x152, &l, &rhw) == 0x24 && has(&l, GEOM_REG_NORMAL, 0, GEOM_FLOAT3, 12) &&
              has(&l, GEOM_REG_DIFFUSE, 0, GEOM_D3DCOLOR, 24),
          "FVF 0x152 (XYZ|NORMAL|DIFFUSE|TEX1): 36 bytes");
    CHECK(geom_layout_from_fvf(0x252, &l, &rhw) == 0x2c && has(&l, GEOM_REG_TEX0 + 1, 0, GEOM_FLOAT2, 36),
          "FVF 0x252 (XYZ|NORMAL|DIFFUSE|TEX2): 44 bytes");
    CHECK(geom_layout_from_fvf(0x244, &l, &rhw) == 0x24 && rhw == 1 && has(&l, 0, 0, GEOM_FLOAT4, 0),
          "FVF 0x244 (XYZRHW|DIFFUSE|TEX2): 36 bytes, rhw");
    CHECK(geom_layout_from_fvf(0x102 | (1u << 16), &l, &rhw) == 24 && has(&l, GEOM_REG_TEX0, 0, GEOM_FLOAT3, 12),
          "FVF texture coordinate size bits (TEXCOORDSIZE3)");
    CHECK(geom_layout_from_fvf(0x006, &l, &rhw) == 16 && has(&l, GEOM_REG_WEIGHT, 0, GEOM_FLOAT1, 12),
          "FVF XYZB1 blend weight");
}

static void test_fetch(void) {
    float o[4];
    const float f3[3] = {1, 2, 3};
    geom_fetch(GEOM_FLOAT3, (const uint8_t *)f3, o);
    CHECK(near4(o, 1, 2, 3, 1), "FLOAT3 (%g %g %g %g)", o[0], o[1], o[2], o[3]);
    const uint32_t col = 0x80FF4020;
    geom_fetch(GEOM_D3DCOLOR, (const uint8_t *)&col, o);
    CHECK(near4(o, 1, 64 / 255.0f, 32 / 255.0f, 128 / 255.0f), "D3DCOLOR is (R, G, B, A) from ARGB");
    uint32_t np = (1023u) | (0x401u << 11) | (511u << 22);   /* x = +1, y = -1023/1023, z = +1 */
    geom_fetch(GEOM_NORMPACKED3, (const uint8_t *)&np, o);
    CHECK(near4(o, 1, -1, 1, 1), "NORMPACKED3 11:11:10 signed (%g %g %g)", o[0], o[1], o[2]);
    np = (0x600u) | (0x200u << 11) | (0x300u << 22);   /* x = -512, y = 512, z = -256 */
    geom_fetch(GEOM_NORMPACKED3, (const uint8_t *)&np, o);
    CHECK(near4(o, -512 / 1023.0f, 512 / 1023.0f, -256 / 511.0f, 1), "NORMPACKED3 mixed signs (%g %g %g)", o[0], o[1], o[2]);
    const int16_t sh[4] = {-2, 7, 300, 9};
    geom_fetch(GEOM_SHORT3, (const uint8_t *)sh, o);
    CHECK(near4(o, -2, 7, 300, 1), "SHORT3 is not normalized");
    geom_fetch(GEOM_NORMSHORT2, (const uint8_t *)sh, o);
    CHECK(near4(o, -2 / 32767.0f, 7 / 32767.0f, 0, 1), "NORMSHORT2");
    const uint8_t pb[4] = {0, 51, 255, 102};
    geom_fetch(GEOM_PBYTE4, pb, o);
    CHECK(near4(o, 0, 0.2f, 1, 0.4f), "PBYTE4");
    const float f2h[3] = {4, 5, 2};
    geom_fetch(GEOM_FLOAT2H, (const uint8_t *)f2h, o);
    CHECK(near4(o, 4, 5, 0, 2), "FLOAT2H is (x, y, 0, w)");
}

static void test_math(void) {
    const float tr[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1};   /* translation in row 4 */
    const float sc[16] = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1};
    float m[16], o[4], v[4] = {1, 1, 1, 1};
    geom_mat_mul(tr, sc, m);   /* translate, then scale */
    geom_row_xform(v, m, o);
    CHECK(near4(o, 22, 63, 124, 1), "v * T * S (%g %g %g %g)", o[0], o[1], o[2], o[3]);
    /* Projection from the title (near 0.5): z = near maps to depth 0, w = -z_view. */
    const float proj[16] = {1.7218f, 0, 0, 0, 0, 2.45916f, 0, 0, 0, 0, -1.0101f, -1, 0, 0, -0.50505f, 0};
    float pv[4] = {0, 0, -0.5f, 1};
    geom_row_xform(pv, proj, o);
    CHECK(fabsf(o[2]) < 1e-3f && fabsf(o[3] - 0.5f) < 1e-6f, "title projection: near plane depth 0 (z %g w %g)", o[2], o[3]);
    /* dp4 with the rows of the transposed matrix equals the row-vector transform (c69..c72). */
    float rows[16], o2[4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) rows[i * 4 + j] = m[j * 4 + i];
    float p[4] = {3, -2, 5, 1};
    geom_row_xform(p, m, o);
    geom_dp4_rows(p, rows, o2);
    CHECK(near4(o2, o[0], o[1], o[2], o[3]), "dp4 rows of the transpose == row-vector transform");

    /* Skinning: bone 0 translates by (1, 2, 3), bone 1 by (-1, 0, 0); weights 0.5/0.5/0. */
    static float c[192][4];
    const int base = 96 - 36;   /* c[a0.x - 36] */
    for (int b = 0; b < 3; b++)
        for (int r = 0; r < 3; r++) {
            memset(c[base + b * 3 + r], 0, 16);
            c[base + b * 3 + r][r] = 1;
        }
    c[base + 0][3] = 1; c[base + 1][3] = 2; c[base + 2][3] = 3;
    c[base + 3][3] = -1;
    const float pos[4] = {1, 1, 1, 1}, idx[4] = {0, 3, 6, 0}, w[4] = {0.5f, 0.5f, 0, 0};
    geom_skin3(pos, idx, w, (const float (*)[4])c, base, o);
    CHECK(near4(o, 1, 2, 2.5f, 1), "three-bone skinning (%g %g %g %g)", o[0], o[1], o[2], o[3]);

    const float s0[4] = {320, 240, 0.5f, 1}, s1[4] = {0, 0, 0.25f, 0.5f};
    geom_screen_to_clip(s0, 0, 0, 640, 480, o);
    CHECK(near4(o, 0, 0, 0.5f, 1), "screen center -> clip origin");
    geom_screen_to_clip(s1, 0, 0, 640, 480, o);
    CHECK(near4(o, -2, 2, 0.5f, 2), "top-left with rhw 0.5 -> (-w, +w), w = 2 (%g %g %g %g)", o[0], o[1], o[2], o[3]);
}

static void test_prims(void) {
    uint32_t o[64];
    uint32_t n = geom_triangulate(5, 6, o, 64);
    CHECK(n == 6 && o[0] == 0 && o[5] == 5, "triangle list passes through");
    n = geom_triangulate(6, 5, o, 64);
    const uint32_t strip[] = {0, 1, 2, 2, 1, 3, 2, 3, 4};
    CHECK(n == 9 && !memcmp(o, strip, sizeof strip), "triangle strip keeps a consistent winding");
    n = geom_triangulate(7, 5, o, 64);
    const uint32_t fan[] = {0, 1, 2, 0, 2, 3, 0, 3, 4};
    CHECK(n == 9 && !memcmp(o, fan, sizeof fan), "triangle fan");
    n = geom_triangulate(8, 8, o, 64);
    const uint32_t quads[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    CHECK(n == 12 && !memcmp(o, quads, sizeof quads), "quad list");
    n = geom_triangulate(9, 6, o, 64);
    const uint32_t qstrip[] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
    CHECK(n == 12 && !memcmp(o, qstrip, sizeof qstrip), "quad strip");
    n = geom_triangulate(10, 4, o, 64);
    CHECK(n == 6 && o[3] == 0 && o[5] == 3, "polygon as a fan");
    CHECK(geom_triangulate(2, 4, o, 64) == 0 && geom_triangulate(1, 4, o, 64) == 0, "points and lines give no triangles");
    CHECK(geom_triangulate(5, 7, o, 64) == 6, "incomplete trailing triangle dropped");
    CHECK(geom_triangulate(5, 30, o, 9) == 9 && geom_triangulated_max(30) >= 84, "output capacity respected");
}

static void test_color(void) {
    const float up[3] = {0, 1, 0}, side[3] = {1, 0, 0}, p0[4] = {0, 0, 0, 1}, p1[4] = {5, 0, 0, 1};
    uint32_t a = geom_debug_color(up, p0), b = geom_debug_color(side, p0);
    uint32_t c = geom_debug_color(NULL, p0), d = geom_debug_color(NULL, p1);
    CHECK(a != b && (a & 0xFFFFFF) && (b & 0xFFFFFF) && !(a >> 24), "normal colors differ by direction (%06x %06x)", a, b);
    CHECK(c != d && c == geom_debug_color(NULL, p0), "position hash is deterministic and varies (%06x %06x)", c, d);
}

/* Fixed-function vertex lighting (D3D8 "Mathematics of Lighting"; Cxbx-Reloaded
 * FixedFunctionVertexShader.hlsl DoLight / DoMaterial): emissive + ambient * Ma + per light
 * attenuation * (La * Ma + Ld * Md * max(N.L, 0)), alpha = diffuse material alpha. */
static void test_lighting(void) {
    GeomLighting L;
    float d[4], sp[4];
    const float up[3] = {0, 1, 0}, origin[3] = {0, 0, 0};
    memset(&L, 0, sizeof L);
    L.mat = (GeomMaterial){{0.5f, 0.5f, 0.5f, 0.75f}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0};
    L.ambient[0] = L.ambient[1] = L.ambient[2] = 0.25f;
    L.n = 1;
    L.light[0].type = GEOM_LIGHT_DIRECTIONAL;
    L.light[0].diffuse[0] = L.light[0].diffuse[1] = L.light[0].diffuse[2] = 1;
    L.light[0].dir[1] = -1;   /* shining down onto an upward normal */
    geom_light(&L, origin, up, NULL, NULL, d, sp);
    CHECK(near4(d, 0.75f, 0.75f, 0.75f, 0.75f), "directional light: 0.25 ambient + 0.5 diffuse, material alpha (%g %g %g %g)", d[0], d[1], d[2], d[3]);
    L.light[0].dir[1] = 1;    /* from below: back-facing */
    geom_light(&L, origin, up, NULL, NULL, d, sp);
    CHECK(near4(d, 0.25f, 0.25f, 0.25f, 0.75f), "back-facing light adds nothing (%g %g %g)", d[0], d[1], d[2]);

    /* Material sources: emissive from the vertex diffuse (D3DMCS_COLOR1), as the title's FVF draws. */
    const float vcol[4] = {0.1f, 0.2f, 0.3f, 0.5f};
    L.color_vertex = 1;
    L.esrc = 1;
    geom_light(&L, origin, up, vcol, NULL, d, sp);
    CHECK(near4(d, 0.35f, 0.45f, 0.55f, 0.75f), "emissive from vertex color (%g %g %g %g)", d[0], d[1], d[2], d[3]);
    L.dsrc = 1;   /* diffuse alpha from the vertex too */
    geom_light(&L, origin, up, vcol, NULL, d, sp);
    CHECK(near4(d, 0.35f, 0.45f, 0.55f, 0.5f), "diffuse source COLOR1 gives the vertex alpha (%g)", d[3]);
    L.color_vertex = 0;   /* COLORVERTEX off: the material wins */
    geom_light(&L, origin, up, vcol, NULL, d, sp);
    CHECK(near4(d, 0.25f, 0.25f, 0.25f, 0.75f), "COLORVERTEX off ignores the vertex colors (%g %g %g %g)", d[0], d[1], d[2], d[3]);
    L.dsrc = L.esrc = 0;

    /* Point light 2 units above with attenuation 1 / (0 + 0.5 d): N.L = 1, att = 1, light ambient 0.1. */
    L.light[0] = (GeomLight){0};
    L.light[0].type = GEOM_LIGHT_POINT;
    L.light[0].diffuse[0] = 1;
    L.light[0].ambient[2] = 0.1f;
    L.light[0].pos[1] = 2;
    L.light[0].range = 10;
    L.light[0].att[1] = 0.5f;
    geom_light(&L, origin, up, NULL, NULL, d, sp);
    CHECK(near4(d, 0.75f, 0.25f, 0.35f, 0.75f), "point light with attenuation and light ambient (%g %g %g)", d[0], d[1], d[2]);
    L.light[0].range = 1;   /* out of range */
    geom_light(&L, origin, up, NULL, NULL, d, sp);
    CHECK(near4(d, 0.25f, 0.25f, 0.25f, 0.75f), "point light beyond its range adds nothing (%g %g %g)", d[0], d[1], d[2]);
    L.n = 0;
    geom_light(&L, origin, NULL, NULL, NULL, d, sp);
    CHECK(near4(d, 0.25f, 0.25f, 0.25f, 0.75f) && near4(sp, 0, 0, 0, 0), "no lights: ambient only, no specular");
    L.ambient[0] = 2;
    geom_light(&L, origin, NULL, NULL, NULL, d, sp);
    CHECK(d[0] == 1, "results saturate to 1 (%g)", d[0]);
}

/* Fog factor per D3DFOGMODE (xemu hw/xbox/nv2a/pgraph/glsl/vsh.c: linear f = (end - d) / (end - start),
 * exp f = e^(-density d), exp2 f = e^(-(density d)^2); mode none passes d through), clamped. */
static void test_fog(void) {
    CHECK(fabsf(geom_fog_factor(GEOM_FOG_LINEAR, 10, 110, 0, 60) - 0.5f) < 1e-5f, "linear fog halfway");
    CHECK(geom_fog_factor(GEOM_FOG_LINEAR, 10, 110, 0, 5) == 1 && geom_fog_factor(GEOM_FOG_LINEAR, 10, 110, 0, 500) == 0, "linear fog clamps");
    CHECK(fabsf(geom_fog_factor(GEOM_FOG_EXP, 0, 0, 0.5f, 2) - expf(-1)) < 1e-5f, "exp fog");
    CHECK(fabsf(geom_fog_factor(GEOM_FOG_EXP2, 0, 0, 0.5f, 2) - expf(-1)) < 1e-5f && fabsf(geom_fog_factor(GEOM_FOG_EXP2, 0, 0, 0.25f, 2) - expf(-0.25f)) < 1e-5f, "exp2 fog");
    CHECK(geom_fog_factor(GEOM_FOG_NONE, 0, 0, 0, 0.25f) == 0.25f, "vertex fog passes the distance through");
    /* The title's Coney settings: linear, start 0, end -1e7, d = -w (its programs write oFog = -oPos.w). */
    CHECK(geom_fog_factor(GEOM_FOG_LINEAR, 0, -1e7f, 0, -100) > 0.9999f, "Coney's fog range leaves the factor at ~1");
    CHECK(geom_fog_factor(GEOM_FOG_LINEAR, 5, 5, 0, 7) == 1, "empty linear range: no fog");
}

/* Batch fetch (the vertex-program input rows): every element type, bit-identical to geom_fetch
 * per vertex, a stride wider than the element, and rows past n left untouched. */
static void test_fetch_rows(void) {
    static const uint32_t types[] = {GEOM_FLOAT1, GEOM_FLOAT2, GEOM_FLOAT3, GEOM_FLOAT4, GEOM_D3DCOLOR, GEOM_SHORT1, GEOM_SHORT2,
                                     GEOM_SHORT3, GEOM_SHORT4, GEOM_NORMSHORT1, GEOM_NORMSHORT2, GEOM_NORMSHORT3, GEOM_NORMSHORT4,
                                     GEOM_NORMPACKED3, GEOM_PBYTE1, GEOM_PBYTE2, GEOM_PBYTE3, GEOM_PBYTE4, GEOM_FLOAT2H, GEOM_NONE};
    enum { N = 37, CAP = 40, STRIDE = 20 };
    uint8_t buf[N * STRIDE];
    uint32_t seed = 12345;
    for (size_t i = 0; i < sizeof buf; i++) { seed = seed * 1103515245u + 12345u; buf[i] = (uint8_t)(seed >> 16); }
    for (int i = 0; i < N; i++) {   /* finite floats in the float slots */
        float f = (float)i * 0.37f - 5.0f;
        for (int k = 0; k < 4; k++) memcpy(buf + i * STRIDE + 4 * k, &f, 4);
    }
    int bad = 0;
    for (size_t t = 0; t < sizeof types / sizeof types[0]; t++) {
        float rows[4][CAP], ref[4];
        for (int q = 0; q < 4; q++)
            for (int j = 0; j < CAP; j++) rows[q][j] = -77.0f;
        geom_fetch_rows(types[t], buf, STRIDE, N, &rows[0][0], CAP);
        for (int j = 0; j < N; j++) {
            geom_fetch(types[t], buf + j * STRIDE, ref);
            for (int q = 0; q < 4; q++)
                if (memcmp(&rows[q][j], &ref[q], 4)) bad++;
        }
        for (int q = 0; q < 4; q++)
            for (int j = N; j < CAP; j++)
                if (rows[q][j] != -77.0f) bad++;
    }
    CHECK(!bad, "batch fetch matches geom_fetch bit for bit for every type (%d mismatches)", bad);
}

int main(void) {
    test_layouts();
    test_fetch();
    test_fetch_rows();
    test_math();
    test_prims();
    test_color();
    test_lighting();
    test_fog();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
