/* Game-independent test of the HUD bar recognizer and the glass rect math (src/hud_glass.c).
 * The quads reproduce the rage meter draw (player panel) recorded in a Quick Rumble fight (screen
 * positions, vertex colors and atlas UVs of the 6 quads). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/hud_glass.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 0.05f; }

static const RViewport VP = {0, 0, 640, 480, 0, 1};

typedef struct { float x0, y0, x1, y1; uint32_t color; float s0, t0, s1, t1; } Quad;

/* Clip-space vertices (w = 2, so the recognizer must divide) and quad indices 0 1 2 2 0 3. */
static uint32_t build(const Quad *q, int n, RVertex *v, uint32_t *idx) {
    for (int i = 0; i < n; i++) {
        float xs[4] = {q[i].x0, q[i].x1, q[i].x1, q[i].x0}, ys[4] = {q[i].y0, q[i].y0, q[i].y1, q[i].y1};
        float ss[4] = {q[i].s0, q[i].s1, q[i].s1, q[i].s0}, ts[4] = {q[i].t0, q[i].t0, q[i].t1, q[i].t1};
        for (int k = 0; k < 4; k++) {
            RVertex *o = &v[i * 4 + k];
            memset(o, 0, sizeof *o);
            o->w = 2;
            o->x = (xs[k] / VP.w * 2 - 1) * o->w;
            o->y = (1 - ys[k] / VP.h * 2) * o->w;
            o->z = 0.545f * o->w;
            o->color = q[i].color;
            o->tex[0][0] = ss[k];
            o->tex[0][1] = ts[k];
            o->tex[0][3] = 1;
        }
        static const uint32_t pat[6] = {0, 1, 2, 2, 0, 3};
        for (int k = 0; k < 6; k++) idx[i * 6 + k] = i * 4 + pat[k];
    }
    return n * 6;
}

#define TRACK HUD_GLASS_TRACK_S0, HUD_GLASS_TRACK_T0, HUD_GLASS_TRACK_S1, HUD_GLASS_TRACK_T1
static const Quad RAGE[6] = {
    {51.8f, 40.1f, 239.1f, 81.5f, 0x00111111, 0.8657f, 0.9106f, 0.8804f, 0.9253f},   /* backing */
    {58.7f, 64.9f, 59.8f, 68.4f, 0x00505050, 0.6509f, 0.9087f, 0.6538f, 0.9312f},    /* left cap */
    {60.2f, 64.9f, 168.2f, 68.4f, 0xff505050, TRACK},                                /* track */
    {60.2f, 64.9f, 146.6f, 68.4f, 0xffffffff, 0.6587f, 0.9087f, 0.6636f, 0.9312f},   /* fill */
    {60.2f, 64.9f, 61.6f, 68.4f, 0xffaa2b2b, 0.6587f, 0.9087f, 0.6636f, 0.9312f},    /* tick */
    {168.2f, 64.9f, 169.3f, 68.4f, 0x00505050, 0.3325f, 0.9360f, 0.3354f, 0.9585f},  /* right cap */
};

/* Screen y of a clip-space vertex in VP. */
static float sy_of(const RVertex *p) { return (0.5f - p->y / p->w * 0.5f) * VP.h; }
static float sx_of(const RVertex *p) { return (p->x / p->w * 0.5f + 0.5f) * VP.w; }
static int chan(uint32_t c, int s) { return (int)(c >> s & 0xFF); }

/* --hud-hd: the rage meter stays byte-identical; a green target bar (the same atlas parts on the
 * right half) gets the new geometry, colors and outline. */
static void test_hd(void) {
    RVertex v[64], keep[64];
    uint32_t idx[96], keep_idx[96];
    HudGlassRect bars[HUD_GLASS_MAX_BARS];
    uint8_t member[64];

    /* Rage meter: untouched (vertices, indices, counts), also not a glass target. */
    uint32_t ni = build(RAGE, 6, v, idx), nv = 24;
    memcpy(keep, v, sizeof v);
    memcpy(keep_idx, idx, sizeof idx);
    CHECK(hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 1, NULL) == 0, "rage meter restyled");
    CHECK(nv == 24 && ni == 36 && !memcmp(v, keep, sizeof v) && !memcmp(idx, keep_idx, sizeof idx), "rage meter changed");
    CHECK(hud_glass_find_targets(v, 24, idx, 36, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "rage meter is a glass target");

    /* Synthetic target bar: the rage meter's quads moved 400 px right, green fill, no tick. */
    Quad tq[6];
    memcpy(tq, RAGE, sizeof tq);
    for (int i = 0; i < 6; i++) { tq[i].x0 += 400; tq[i].x1 += 400; }
    tq[3].color = 0xffa8e000;
    tq[4].x1 = tq[4].x0; tq[4].color = 0;   /* tick: zero width, invisible */
    ni = build(tq, 6, v, idx);
    nv = 24;
    CHECK(hud_glass_find_targets(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member) == 1, "target bar for glass");
    memcpy(keep, v, sizeof v);
    int n = hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 1, NULL);
    CHECK(n == 1 && nv == 28 && ni == 42, "target restyled: %d bars, %u vertices, %u indices", n, nv, ni);
    if (n != 1 || nv != 28) return;
    const float h = HUD_HD_ASPECT * (568.2f - 460.2f);   /* 4.64 px, more than the original 3.5 */
    /* Outline drawn first: the first 6 indices use the 4 new vertices. */
    int first_new = 1;
    for (int k = 0; k < 6; k++) first_new &= idx[k] >= 24;
    CHECK(first_new, "outline quad first");
    float ox0 = 1e9f, oy0 = 1e9f, ox1 = -1e9f, oy1 = -1e9f;
    for (int k = 24; k < 28; k++) {
        ox0 = fminf(ox0, sx_of(&v[k])); ox1 = fmaxf(ox1, sx_of(&v[k]));
        oy0 = fminf(oy0, sy_of(&v[k])); oy1 = fmaxf(oy1, sy_of(&v[k]));
        CHECK(v[k].color == HUD_HD_OUTLINE, "outline color %08x", v[k].color);
        CHECK(near(v[k].tex[0][0], HUD_GLASS_TRACK_S0) || near(v[k].tex[0][0], HUD_GLASS_TRACK_S1), "outline uv %.4f", v[k].tex[0][0]);
        CHECK(near(v[k].z / v[k].w, 0.545f), "outline depth");
    }
    CHECK(near(ox0, 457.7f) && near(ox1, 570.3f) && near(oy0, 63.9f) && near(oy1, 64.9f + h + 1),
          "outline %.2f,%.2f-%.2f,%.2f", ox0, oy0, ox1, oy1);
    /* The rest of the indices: the original quads, in order. */
    int same = 1;
    for (int k = 0; k < 36; k++) same &= idx[6 + k] == keep_idx[k];
    CHECK(same, "original indices after the outline");
    /* Strip quads grow downward from the top edge; x and the fill ratio unchanged. */
    for (int k = 4; k < 24; k++) {
        float y = sy_of(&v[k]), y_old = sy_of(&keep[k]);
        CHECK(near(sx_of(&v[k]), sx_of(&keep[k])), "x of vertex %d moved", k);
        CHECK(near(y, y_old < 66 ? 64.9f : 64.9f + h), "vertex %d y %.2f (was %.2f)", k, y, y_old);
    }
    /* Backing quad untouched. */
    CHECK(!memcmp(v, keep, 4 * sizeof *v), "backing quad changed");
    /* Track: dark translucent. */
    for (int k = 8; k < 12; k++) CHECK(v[k].color == HUD_HD_TRACK, "track color %08x", v[k].color);
    /* Fill: green kept, lighter at the top, darker at the bottom, opaque. */
    for (int k = 12; k < 16; k++) {
        uint32_t c = v[k].color;
        int top = sy_of(&v[k]) < 66;
        CHECK(c >> 24 == 0xff, "fill alpha %08x", c);
        CHECK(top ? chan(c, 8) >= 0xe0 && chan(c, 16) > 0xa8 && chan(c, 0) > 0 : chan(c, 8) < 0xe0 && chan(c, 16) < 0xa8,
              "fill gradient vertex %d %08x", k, c);
        CHECK(chan(c, 8) > chan(c, 16) && chan(c, 16) > chan(c, 0), "fill stays green %08x", c);
    }

    /* Height scale 2: twice the height. */
    ni = build(tq, 6, v, idx);
    nv = 24;
    hud_hd_restyle(v, &nv, idx, &ni, &VP, 2, 1, NULL);
    CHECK(near(sy_of(&v[10]), 64.9f + 2 * h), "scale 2 bottom %.2f", sy_of(&v[10]));
    /* A bar taller than the rule keeps its height (never thinner). */
    Quad tall[6];
    memcpy(tall, tq, sizeof tall);
    for (int i = 1; i < 6; i++) tall[i].y1 = tall[i].y0 + 10;
    ni = build(tall, 6, v, idx);
    nv = 24;
    hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 1, NULL);
    CHECK(near(sy_of(&v[10]), 74.9f), "tall bar keeps its height %.2f", sy_of(&v[10]));

    /* The same green bar on the left half: a player-panel bar, untouched. */
    Quad left[6];
    memcpy(left, tq, sizeof left);
    for (int i = 0; i < 6; i++) { left[i].x0 -= 400; left[i].x1 -= 400; }
    ni = build(left, 6, v, idx);
    nv = 24;
    memcpy(keep, v, sizeof v);
    CHECK(hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 1, NULL) == 0 && nv == 24 && ni == 36 && !memcmp(v, keep, sizeof v), "left green bar restyled");

    /* Opacity: strip quads x F, outline x sqrt(F); outline rect reported; 1 changes nothing. */
    HudGlassRect orect[HUD_GLASS_MAX_BARS];
    RVertex ref[64];
    ni = build(tq, 6, v, idx);
    nv = 24;
    hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 1, NULL);
    memcpy(ref, v, sizeof v);
    ni = build(tq, 6, v, idx);
    nv = 24;
    CHECK(hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 1, orect) == 1 && !memcmp(v, ref, sizeof v), "opacity 1 = the opaque look");
    CHECK(near(orect[0].x0, 457.7f) && near(orect[0].y0, 63.9f) && near(orect[0].x1, 570.3f) && near(orect[0].y1, 64.9f + h + 1),
          "outline rect %.2f,%.2f-%.2f,%.2f", orect[0].x0, orect[0].y0, orect[0].x1, orect[0].y1);
    ni = build(tq, 6, v, idx);
    nv = 24;
    hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 0.25f, NULL);
    for (int k = 0; k < 28; k++) {
        uint32_t a = ref[k].color >> 24, want = k >= 24 ? (uint32_t)(a * 0.5f + 0.5f) : k >= 4 ? (uint32_t)(a * 0.25f + 0.5f) : a;
        CHECK(v[k].color >> 24 == want && (v[k].color & 0xFFFFFFu) == (ref[k].color & 0xFFFFFFu), "opacity 0.25 vertex %d %08x (want alpha %02x)",
              k, v[k].color, want);
    }
    CHECK(!memcmp(v, ref, 4 * sizeof *v), "opacity: backing quad changed");
    /* Rage meter with opacity and frost rects: untouched. */
    ni = build(RAGE, 6, v, idx);
    nv = 24;
    memcpy(keep, v, sizeof v);
    CHECK(hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 0.3f, orect) == 0 && nv == 24 && !memcmp(v, keep, sizeof v), "rage meter faded");
    /* Frost op: the outline rect, square, no tint, no rim. */
    RGlass fr = hud_hd_frost((HudGlassRect){457.7f, 63.9f, 570.3f, 70.5f}, 3);
    CHECK(fr.x0 == 457.7f && fr.y0 == 63.9f && fr.x1 == 570.3f && fr.y1 == 70.5f && fr.blur == 3 && fr.corner == 0 && !(fr.tint >> 24) &&
              !(fr.rim >> 24),
          "frost op %.1f,%.1f-%.1f,%.1f blur %.1f corner %.1f tint %08x rim %08x", fr.x0, fr.y0, fr.x1, fr.y1, fr.blur, fr.corner, fr.tint, fr.rim);

    /* Rage meter and target in one batch: only the target changes. */
    Quad both[11];
    memcpy(both, RAGE, sizeof RAGE);
    memcpy(both + 6, tq + 1, 5 * sizeof *tq);
    ni = build(both, 11, v, idx);
    nv = 44;
    memcpy(keep, v, sizeof v);
    n = hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 1, NULL);
    CHECK(n == 1 && nv == 48 && ni == 72 && !memcmp(v, keep, 24 * sizeof *v), "mixed batch: %d bars, rage meter unchanged", n);
}

/* The real target bar draw (Lua mission bar "HURRICANE CAR", mission 7 = level5, recorded at
 * swap 6150 with HUDEnableBar forced on): one batch of 28 quads, the
 * name's glyphs (shadow 80000000 + face b2b2b2) then the right cap, the track, the fill (starting
 * at the left cap, about 1 px left of the track) and the left cap; the caps are opaque here. */
static const Quad TARGET[28] = {
    {457.31f, 132.93f, 466.74f, 151.48f, 0x80000000, 0.9458f, 0.5884f, 0.9858f, 0.6577f},
    {456.10f, 131.37f, 465.53f, 149.93f, 0xffb2b2b2, 0.9458f, 0.5884f, 0.9858f, 0.6577f},
    {468.20f, 132.93f, 477.63f, 151.48f, 0x80000000, 0.4067f, 0.5884f, 0.4468f, 0.6577f},
    {466.99f, 131.37f, 476.41f, 149.93f, 0xffb2b2b2, 0.4067f, 0.5884f, 0.4468f, 0.6577f},
    {479.08f, 132.93f, 488.96f, 151.48f, 0x80000000, 0.5337f, 0.5884f, 0.5757f, 0.6577f},
    {477.87f, 131.37f, 487.74f, 149.93f, 0xffb2b2b2, 0.5337f, 0.5884f, 0.5757f, 0.6577f},
    {490.41f, 132.93f, 500.29f, 151.48f, 0x80000000, 0.5337f, 0.5884f, 0.5757f, 0.6577f},
    {489.20f, 131.37f, 499.08f, 149.93f, 0xffb2b2b2, 0.5337f, 0.5884f, 0.5757f, 0.6577f},
    {501.74f, 132.93f, 505.33f, 151.48f, 0x80000000, 0.9282f, 0.5884f, 0.9429f, 0.6577f},
    {500.53f, 131.37f, 504.12f, 149.93f, 0xffb2b2b2, 0.9282f, 0.5884f, 0.9429f, 0.6577f},
    {506.79f, 132.93f, 516.67f, 151.48f, 0x80000000, 0.1646f, 0.6606f, 0.2065f, 0.7300f},
    {505.58f, 131.37f, 515.45f, 149.93f, 0xffb2b2b2, 0.1646f, 0.6606f, 0.2065f, 0.7300f},
    {518.12f, 132.93f, 528.90f, 151.48f, 0x80000000, 0.2524f, 0.6606f, 0.2983f, 0.7300f},
    {516.91f, 131.37f, 527.68f, 149.93f, 0xffb2b2b2, 0.2524f, 0.6606f, 0.2983f, 0.7300f},
    {530.35f, 132.93f, 539.78f, 151.48f, 0x80000000, 0.7134f, 0.5884f, 0.7534f, 0.6577f},
    {529.14f, 131.37f, 538.57f, 149.93f, 0xffb2b2b2, 0.7134f, 0.5884f, 0.7534f, 0.6577f},
    {541.23f, 132.93f, 549.76f, 151.48f, 0x80000000, 0.0825f, 0.6606f, 0.1187f, 0.7300f},
    {540.02f, 131.37f, 548.55f, 149.93f, 0xffb2b2b2, 0.0825f, 0.6606f, 0.1187f, 0.7300f},
    {556.15f, 132.93f, 566.03f, 151.48f, 0x80000000, 0.1646f, 0.6606f, 0.2065f, 0.7300f},
    {554.94f, 131.37f, 564.82f, 149.93f, 0xffb2b2b2, 0.1646f, 0.6606f, 0.2065f, 0.7300f},
    {567.48f, 132.93f, 578.26f, 151.48f, 0x80000000, 0.2524f, 0.6606f, 0.2983f, 0.7300f},
    {566.27f, 131.37f, 577.05f, 149.93f, 0xffb2b2b2, 0.2524f, 0.6606f, 0.2983f, 0.7300f},
    {579.71f, 132.93f, 589.59f, 151.48f, 0x80000000, 0.5337f, 0.5884f, 0.5757f, 0.6577f},
    {578.50f, 131.37f, 588.38f, 149.93f, 0xffb2b2b2, 0.5337f, 0.5884f, 0.5757f, 0.6577f},
    {588.95f, 149.42f, 590.01f, 159.16f, 0xff404040, 0.6509f, 0.9087f, 0.6538f, 0.9312f},
    {440.54f, 149.42f, 588.57f, 159.16f, 0xff404040, 0.6704f, 0.9087f, 0.6753f, 0.9312f},
    {439.47f, 149.42f, 543.10f, 159.16f, 0xff9d840c, 0.6587f, 0.9087f, 0.6636f, 0.9312f},
    {438.19f, 149.42f, 439.25f, 159.16f, 0xff404040, 0.3325f, 0.9360f, 0.3354f, 0.9585f},
};

/* The recorded target bar: recognized as a target, restyled with the fill found despite its
 * offset, faded per quad class; the glyph quads untouched. */
static void test_real_target(void) {
    static RVertex v[160], keep[160];
    static uint32_t idx[220];
    HudGlassRect bars[HUD_GLASS_MAX_BARS], orect[HUD_GLASS_MAX_BARS];
    uint32_t ni = build(TARGET, 28, v, idx), nv = 112;
    CHECK(hud_glass_find_targets(v, nv, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, NULL) == 1, "real target: found");
    memcpy(keep, v, sizeof v);
    int n = hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 1, orect);
    CHECK(n == 1 && nv == 116 && ni == 174, "real target: restyled (%d, %u, %u)", n, nv, ni);
    if (n != 1) return;
    CHECK(near(orect[0].x0, 437.19f) && near(orect[0].y0, 148.42f) && near(orect[0].x1, 591.01f) && near(orect[0].y1, 160.16f),
          "real target: outline %.2f,%.2f-%.2f,%.2f", orect[0].x0, orect[0].y0, orect[0].x1, orect[0].y1);
    /* Height kept (9.74 px is more than 4.3 % of the width). */
    CHECK(near(sy_of(&v[102]), 159.16f), "real target: height kept %.2f", sy_of(&v[102]));
    /* Glyph quads (0-95) untouched. */
    CHECK(!memcmp(v, keep, 96 * sizeof *v), "real target: glyphs changed");
    /* Fill (quad 26): gradient on its own color. */
    CHECK(v[104].color != keep[104].color && v[106].color != keep[106].color && chan(v[104].color, 8) > chan(keep[104].color, 8) &&
              chan(v[106].color, 8) < chan(keep[106].color, 8),
          "real target: fill gradient %08x / %08x (was %08x)", v[104].color, v[106].color, keep[104].color);
    CHECK(v[100].color == HUD_HD_TRACK, "real target: track %08x", v[100].color);
    /* Opacity 0.6: bar quads x 0.6, outline x sqrt(0.6), glyphs unchanged. */
    ni = build(TARGET, 28, v, idx);
    nv = 112;
    hud_hd_restyle(v, &nv, idx, &ni, &VP, 1, 0.6f, NULL);
    CHECK(v[104].color >> 24 == 0x99 && v[96].color >> 24 == 0x99 && v[108].color >> 24 == 0x99, "real target: bar alpha %02x %02x %02x",
          v[104].color >> 24, v[96].color >> 24, v[108].color >> 24);
    CHECK(v[112].color >> 24 == (uint32_t)(0xE0 * sqrtf(0.6f) + 0.5f), "real target: outline alpha %02x", v[112].color >> 24);
    CHECK(!memcmp(v, keep, 96 * sizeof *v), "real target: glyphs faded");
}

/* ---- T7: glass target bars that keep their color ---- */
static void rgbf(uint32_t c, float *r, float *g, float *b) {
    *r = chan(c, 16) / 255.0f;
    *g = chan(c, 8) / 255.0f;
    *b = chan(c, 0) / 255.0f;
}
/* HSV hue (degrees), saturation and value of an rgb triple. */
static void hsv(float r, float g, float b, float *h, float *s, float *v) {
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) *h = 0;
    else if (mx == r) *h = 60 * fmodf((g - b) / d + 6, 6);
    else if (mx == g) *h = 60 * ((b - r) / d + 2);
    else *h = 60 * ((r - g) / d + 4);
}
/* SRC_ALPHA / INV_SRC_ALPHA of color c over the gray background bg (0..1): HSV of the result. */
static void over(uint32_t c, float bg, float *h, float *s, float *v) {
    float r, g, b, a = (c >> 24) / 255.0f;
    rgbf(c, &r, &g, &b);
    hsv(a * r + (1 - a) * bg, a * g + (1 - a) * bg, a * b + (1 - a) * bg, h, s, v);
}
static float hue_diff(float a, float b) {
    float d = fabsf(a - b);
    return d > 180 ? 360 - d : d;
}

static void test_glass_style(void) {
    /* Vivid: gain 0 is the identity; hue kept, saturation and value raised, gray stays gray. */
    CHECK(hud_hd_vivid(0xff64ff64u, 0) == 0xff64ff64u && hud_hd_vivid(0x809d840cu, 0) == 0x809d840cu, "vivid 0 changed the color");
    const uint32_t cols[4] = {0xff64ff64u, 0xff9d840cu, 0xffaa2b2bu, 0x80405060u};
    for (int i = 0; i < 4; i++) {
        float r, g, b, h0, s0, v0, h1, s1, v1;
        rgbf(cols[i], &r, &g, &b);
        hsv(r, g, b, &h0, &s0, &v0);
        uint32_t o = hud_hd_vivid(cols[i], 0.5f);
        rgbf(o, &r, &g, &b);
        hsv(r, g, b, &h1, &s1, &v1);
        CHECK(o >> 24 == cols[i] >> 24 && hue_diff(h0, h1) < 2 && s1 >= s0 && v1 >= v0 && (s1 > s0 || s0 >= 0.999f) &&
                  (s1 <= fminf(1, s0 * 1.5f) + 0.01f) && v1 <= fminf(1, v0 * 1.5f) + 0.01f,
              "vivid %08x -> %08x: hue %.1f -> %.1f, s %.2f -> %.2f, v %.2f -> %.2f", cols[i], o, h0, h1, s0, s1, v0, v1);
    }
    CHECK(hud_hd_vivid(0xff808080u, 1) == 0xffffffffu || (chan(hud_hd_vivid(0xff808080u, 1), 16) == chan(hud_hd_vivid(0xff808080u, 1), 8) &&
                                                           chan(hud_hd_vivid(0xff808080u, 1), 8) == chan(hud_hd_vivid(0xff808080u, 1), 0)),
          "vivid gray %08x", hud_hd_vivid(0xff808080u, 1));

    /* The real target bar with the glass preset style. */
    static RVertex v[200], keep[200];
    static uint32_t idx[300];
    HudGlassConfig c = HUD_GLASS_DEFAULTS;
    CHECK(hud_glass_arg(&c, "--hud-hd-glass", NULL) == 1, "--hud-hd-glass");
    HudHdStyle st = hud_hd_style(&c);
    CHECK(st.frame && st.track_opacity > 0 && st.track_opacity <= 0.3f && st.fill_opacity >= 0.5f && st.fill_opacity < 1 && st.vivid > 0 &&
              c.hd_frost >= 3,
          "glass preset: frame %d track %.2f fill %.2f vivid %.2f frost %.1f", st.frame, st.track_opacity, st.fill_opacity, st.vivid, c.hd_frost);
    HudGlassRect orect[HUD_GLASS_MAX_BARS];
    uint32_t ni = build(TARGET, 28, v, idx), nv = 112;
    memcpy(keep, v, sizeof v);
    int n = hud_hd_restyle_style(v, &nv, idx, &ni, &VP, &st, orect);
    /* Outline = a frame of 4 thin quads (no filled quad under the translucent bar). */
    CHECK(n == 1 && nv == 112 + 16 && ni == 168 + 24, "glass: restyled (%d, %u, %u)", n, nv, ni);
    if (n != 1) return;
    CHECK(!memcmp(v, keep, 96 * sizeof *v), "glass: glyphs changed");
    CHECK(near(orect[0].x0, 437.19f) && near(orect[0].y0, 148.42f) && near(orect[0].x1, 591.01f) && near(orect[0].y1, 160.16f),
          "glass: outline rect %.2f,%.2f-%.2f,%.2f", orect[0].x0, orect[0].y0, orect[0].x1, orect[0].y1);
    float covered = 0;
    for (int q = 0; q < 4; q++) {
        const RVertex *o = &v[112 + 4 * q];
        float w = sx_of(&o[1]) - sx_of(&o[0]), h = sy_of(&o[2]) - sy_of(&o[0]);
        CHECK((near(w, 1) || near(h, 1)) && w > 0 && h > 0, "glass: frame quad %d is %.2f x %.2f", q, w, h);
        CHECK(o[0].color == HUD_HD_GLASS_OUTLINE, "glass: frame color %08x", o[0].color);
        covered += w * h;
    }
    CHECK(covered < 2 * ((591.01f - 437.19f) + (160.16f - 148.42f)) + 1, "glass: frame covers %.1f px^2", covered);
    for (int k = 0; k < 24; k++) {
        uint32_t want = (uint32_t)idx[k] - 112;
        CHECK(want < 16, "glass: frame indices first (%u)", idx[k]);
    }
    /* Track: very transparent, only over the empty part (from the fill's right edge). */
    CHECK((v[100].color & 0xFFFFFFu) == (HUD_HD_GLASS_TRACK & 0xFFFFFFu) &&
              v[100].color >> 24 == (uint32_t)((HUD_HD_GLASS_TRACK >> 24) * st.track_opacity + 0.5f),
          "glass: track %08x", v[100].color);
    CHECK(near(sx_of(&v[100]), 543.10f) && near(sx_of(&v[101]), 588.57f), "glass: track spans %.2f-%.2f", sx_of(&v[100]), sx_of(&v[101]));
    /* Caps: as transparent as the track. */
    CHECK(v[96].color >> 24 == (uint32_t)(0xff * st.track_opacity + 0.5f) && v[108].color >> 24 == (uint32_t)(0xff * st.track_opacity + 0.5f),
          "glass: caps %08x %08x", v[96].color, v[108].color);
    /* Fill: translucent at fill_opacity, hue of the game's color kept, and over a dark and a
     * bright background its saturation stays well above the T5 look (--hud-hd-opacity 0.6),
     * over the dark one at the opaque original's. */
    const uint32_t a_fill = (uint32_t)(0xff * st.fill_opacity + 0.5f);
    CHECK(v[104].color >> 24 == a_fill && v[106].color >> 24 == a_fill, "glass: fill alpha %02x %02x", v[104].color >> 24, v[106].color >> 24);
    float r, g, b, h0, s0, v0;
    rgbf(TARGET[26].color, &r, &g, &b);
    hsv(r, g, b, &h0, &s0, &v0);
    RVertex t5[200];
    uint32_t t5i[300], t5n = 112, t5ni = build(TARGET, 28, t5, t5i);
    hud_hd_restyle(t5, &t5n, t5i, &t5ni, &VP, 1, 0.6f, NULL);
    for (int bgi = 0; bgi < 2; bgi++) {
        const float bg = bgi ? 0.8f : 0.1f;
        float hg = 0, sg = 0, vg = 0, ht = 0, s5 = 0, v5 = 0;
        for (int k = 0; k < 4; k++) {   /* the fill's 4 vertices (gradient top and bottom) */
            float hh, ss, vv;
            over(v[104 + k].color, bg, &hh, &ss, &vv);
            hg += hh / 4; sg += ss / 4; vg += vv / 4;
            over(t5[104 + k].color, bg, &hh, &ss, &vv);
            ht += hh / 4; s5 += ss / 4; v5 += vv / 4;
        }
        (void)vg; (void)v5; (void)ht;
        /* Over a bright background no translucent color keeps the full saturation (its darkest
         * channel is at least (1 - alpha) x background), so there it only has to beat T5 clearly. */
        CHECK(hue_diff(hg, h0) < 4 && (bgi || sg >= s0 * 0.95f) && sg > s5 + 0.1f,
              "glass over %.1f: hue %.1f (game %.1f), saturation %.2f (game %.2f, T5 %.2f)", bg, hg, h0, sg, s0, s5);
    }

    /* Rage meter untouched with every glass style; in a mixed batch only the target changes. */
    ni = build(RAGE, 6, v, idx);
    nv = 24;
    memcpy(keep, v, sizeof v);
    CHECK(hud_hd_restyle_style(v, &nv, idx, &ni, &VP, &st, orect) == 0 && nv == 24 && ni == 36 && !memcmp(v, keep, sizeof v),
          "glass: rage meter changed");
    Quad both[11];
    memcpy(both, RAGE, sizeof RAGE);
    for (int i = 0; i < 5; i++) {
        both[6 + i] = RAGE[1 + i];
        both[6 + i].x0 += 400;
        both[6 + i].x1 += 400;
    }
    ni = build(both, 11, v, idx);
    nv = 44;
    memcpy(keep, v, sizeof v);
    n = hud_hd_restyle_style(v, &nv, idx, &ni, &VP, &st, NULL);
    CHECK(n == 1 && nv == 60 && ni == 90 && !memcmp(v, keep, 24 * sizeof *v), "glass mixed batch: %d bars, rage meter unchanged", n);

    /* The default style is the T4/T5 look byte for byte. */
    HudGlassConfig d = HUD_GLASS_DEFAULTS;
    d.hd = 1;
    d.hd_opacity = 0.6f;
    HudHdStyle ds = hud_hd_style(&d);
    RVertex a1[200], a2[200];
    uint32_t i1[300], i2[300], n1 = 112, n2 = 112, m1 = build(TARGET, 28, a1, i1), m2 = build(TARGET, 28, a2, i2);
    hud_hd_restyle(a1, &n1, i1, &m1, &VP, 1, 0.6f, NULL);
    hud_hd_restyle_style(a2, &n2, i2, &m2, &VP, &ds, NULL);
    CHECK(!ds.frame && n1 == n2 && m1 == m2 && !memcmp(a1, a2, n1 * sizeof *a1) && !memcmp(i1, i2, m1 * sizeof *i1), "default style differs");

    /* Options: each new one turns --hud-hd and the frame on; values checked; the preset can be
     * overridden by later options. */
    HudGlassConfig e = HUD_GLASS_DEFAULTS, keepc;
    CHECK(e.hd_track_opacity < 0 && e.hd_fill_opacity < 0 && e.hd_vivid == 0 && !e.hd_frame, "new option defaults");
    CHECK(hud_glass_arg(&e, "--hud-hd-track-opacity", "0.1") == 2 && e.hd && e.hd_frame && near(e.hd_track_opacity, 0.1f) && !e.on,
          "--hud-hd-track-opacity");
    CHECK(hud_glass_arg(&e, "--hud-hd-fill-opacity", "0.8") == 2 && near(e.hd_fill_opacity, 0.8f), "--hud-hd-fill-opacity");
    CHECK(hud_glass_arg(&e, "--hud-hd-vivid", "0.5") == 2 && near(e.hd_vivid, 0.5f), "--hud-hd-vivid");
    keepc = e;
    CHECK(hud_glass_arg(&e, "--hud-hd-track-opacity", "1.5") == -1 && hud_glass_arg(&e, "--hud-hd-fill-opacity", "-1") == -1 &&
              hud_glass_arg(&e, "--hud-hd-vivid", "2") == -1 && hud_glass_arg(&e, "--hud-hd-vivid", NULL) == -1 &&
              hud_glass_arg(&e, "--hud-hd-fill-opacity", "x") == -1 && !memcmp(&e, &keepc, sizeof e),
          "bad new option values accepted");
    e = (HudGlassConfig)HUD_GLASS_DEFAULTS;
    hud_glass_arg(&e, "--hud-hd-glass", NULL);
    CHECK(hud_glass_arg(&e, "--hud-hd-fill-opacity", "0.9") == 2 && near(hud_hd_style(&e).fill_opacity, 0.9f), "preset override");
    /* Only track opacity given: the fill follows --hud-hd-opacity. */
    e = (HudGlassConfig)HUD_GLASS_DEFAULTS;
    hud_glass_arg(&e, "--hud-hd-opacity", "0.5");
    hud_glass_arg(&e, "--hud-hd-track-opacity", "0.2");
    st = hud_hd_style(&e);
    CHECK(near(st.fill_opacity, 0.5f) && near(st.track_opacity, 0.2f) && st.frame, "fill follows opacity %.2f", st.fill_opacity);
}

int main(void) {
    RVertex v[64];
    uint32_t idx[96];
    uint8_t member[64];
    HudGlassRect bars[HUD_GLASS_MAX_BARS];

    /* The rage meter: one bar, the strip of quads 1-5, not the backing quad. */
    uint32_t ni = build(RAGE, 6, v, idx);
    memset(member, 7, sizeof member);
    int n = hud_glass_find(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member);
    CHECK(n == 1, "rage meter: %d bars", n);
    if (n == 1)
        CHECK(near(bars[0].x0, 58.7f) && near(bars[0].y0, 64.9f) && near(bars[0].x1, 169.3f) && near(bars[0].y1, 68.4f),
              "rage meter rect %.2f,%.2f-%.2f,%.2f", bars[0].x0, bars[0].y0, bars[0].x1, bars[0].y1);
    for (int i = 0; i < 24; i++)
        CHECK(member[i] == (i < 4 ? 0 : i < 8 || i >= 12 ? HUD_GLASS_PART : HUD_GLASS_TRACK), "member[%d] = %d", i, member[i]);

    /* member may be NULL. */
    CHECK(hud_glass_find(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, NULL) == 1, "NULL member");

    /* The same quads with the track on another atlas part: not a bar. */
    Quad other[6];
    memcpy(other, RAGE, sizeof other);
    other[2].s0 += 0.02f; other[2].s1 += 0.02f;
    ni = build(other, 6, v, idx);
    CHECK(hud_glass_find(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "other part recognized");
    for (int i = 0; i < 24; i++) CHECK(member[i] == 0, "other part member[%d] = %d", i, member[i]);

    /* A track-part quad that is not a thin horizontal strip (tall, or too short): not a bar. */
    memcpy(other, RAGE, sizeof other);
    other[2].y1 = other[2].y0 + 40;
    ni = build(other, 6, v, idx);
    CHECK(hud_glass_find(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "tall track recognized");
    memcpy(other, RAGE, sizeof other);
    other[2].x1 = other[2].x0 + 4;
    ni = build(other, 6, v, idx);
    CHECK(hud_glass_find(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "short track recognized");

    /* A text quad (one quad of the font page): nothing. */
    Quad text = {60.6f, 39.4f, 132.7f, 62.7f, 0xffaa2b2b, 0.001f, 0.001f, 0.5928f, 0.7178f};
    ni = build(&text, 1, v, idx);
    CHECK(hud_glass_find(v, 4, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "text recognized");

    /* Two bars in one batch (rage meter top left, a green target bar top right, plus an unrelated
     * quad of the same page on the target bar's rows but far away): two bars, unrelated quad
     * not marked. */
    Quad two[13];
    memcpy(two, RAGE, sizeof RAGE);
    for (int i = 1; i < 6; i++) {
        two[5 + i] = RAGE[i];
        two[5 + i].x0 += 400; two[5 + i].x1 += 400;
        two[5 + i].y0 += 20; two[5 + i].y1 += 20;
    }
    two[8].color = 0xff64ff64;
    two[11] = (Quad){300, 84.9f, 340, 88.4f, 0xffffffff, 0.6587f, 0.9087f, 0.6636f, 0.9312f};
    two[12] = (Quad){470, 70, 490, 82, 0xffffffff, 0.1f, 0.1f, 0.2f, 0.2f};   /* icon above the strip */
    ni = build(two, 13, v, idx);
    n = hud_glass_find(v, 52, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member);
    CHECK(n == 2, "two bars: %d", n);
    if (n == 2) {
        CHECK(near(bars[0].x0, 58.7f) && near(bars[0].x1, 169.3f), "bar 0 %.2f-%.2f", bars[0].x0, bars[0].x1);
        CHECK(near(bars[1].x0, 458.7f) && near(bars[1].y0, 84.9f) && near(bars[1].x1, 569.3f) && near(bars[1].y1, 88.4f),
              "bar 1 %.2f,%.2f-%.2f,%.2f", bars[1].x0, bars[1].y0, bars[1].x1, bars[1].y1);
    }
    for (int q = 0; q < 13; q++)
        for (int k = 0; k < 4; k++)
            CHECK(member[q * 4 + k] == (q == 0 || q >= 11 ? 0 : q == 2 || q == 7 ? HUD_GLASS_TRACK : HUD_GLASS_PART),
                  "two bars member quad %d = %d", q, member[q * 4 + k]);
    /* max limits the output, the count stays the number written. */
    CHECK(hud_glass_find(v, 52, idx, ni, &VP, bars, 1, member) == 1, "max 1");

    /* Not quads: a fan-like list, an index count that is not whole quads, out-of-range indices,
     * no indices: nothing, no crash. */
    ni = build(RAGE, 6, v, idx);
    uint32_t fan[9] = {0, 1, 2, 0, 2, 3, 0, 3, 4};
    CHECK(hud_glass_find(v, 24, fan, 9, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "fan");
    CHECK(hud_glass_find(v, 24, idx, ni - 3, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "partial quads");
    uint32_t bad[96];
    memcpy(bad, idx, ni * 4);
    bad[14] = 999;
    CHECK(hud_glass_find(v, 24, bad, ni, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "index out of range");
    CHECK(hud_glass_find(v, 24, idx, 0, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "no indices");
    /* A skewed (not axis-aligned) track quad: not a bar. */
    ni = build(RAGE, 6, v, idx);
    v[9].x += 0.2f;
    CHECK(hud_glass_find(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member) == 0, "skewed track");

    /* A viewport offset and size are honored (the same bar in a 1280x960 viewport at 100,50). */
    RViewport vp2 = {100, 50, 1280, 960, 0, 1};
    ni = build(RAGE, 6, v, idx);
    n = hud_glass_find(v, 24, idx, ni, &vp2, bars, HUD_GLASS_MAX_BARS, member);
    CHECK(n == 1 && near(bars[0].x0, 100 + 58.7f * 2) && near(bars[0].y1, 50 + 68.4f * 2), "viewport scale %d %.2f %.2f",
          n, n ? bars[0].x0 : 0, n ? bars[0].y1 : 0);

    /* Fade: alpha of marked vertices scaled, rgb and unmarked vertices unchanged. */
    ni = build(RAGE, 6, v, idx);
    hud_glass_find(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member);
    hud_glass_fade(v, 24, member, 0.5f);
    CHECK(v[0].color == 0x00111111, "backing faded %08x", v[0].color);
    CHECK(v[8].color == 0x40505050, "track %08x", v[8].color);
    CHECK(v[12].color == 0x80ffffff, "fill %08x", v[12].color);
    CHECK(v[4].color == 0x00505050, "cap %08x", v[4].color);
    hud_glass_fade(v, 24, member, 0);
    CHECK(v[12].color == 0x00ffffff, "fill at 0 %08x", v[12].color);
    ni = build(RAGE, 6, v, idx);
    hud_glass_find(v, 24, idx, ni, &VP, bars, HUD_GLASS_MAX_BARS, member);
    hud_glass_fade(v, 24, member, 1.5f);   /* clamped to 1 */
    CHECK(v[12].color == 0xffffffff, "fill above 1 %08x", v[12].color);

    /* Box: margin, scale, outward rounding, clipping. */
    HudGlassRect r = {58.7f, 64.9f, 169.3f, 68.4f};
    HudGlassBox b = hud_glass_box(r, 4, 1, 1, 640, 480);
    CHECK(b.x == 54 && b.y == 60 && b.w == 120 && b.h == 13, "box 1x %d,%d %dx%d", b.x, b.y, b.w, b.h);
    b = hud_glass_box(r, 4, 3, 2.25f, 1920, 1080);
    CHECK(b.x == 164 && b.y == 137 && b.w == 356 && b.h == 26, "box scaled %d,%d %dx%d", b.x, b.y, b.w, b.h);
    b = hud_glass_box((HudGlassRect){-10, -5, 20, 6}, 2, 1, 1, 640, 480);
    CHECK(b.x == 0 && b.y == 0 && b.w == 22 && b.h == 8, "box clipped %d,%d %dx%d", b.x, b.y, b.w, b.h);
    b = hud_glass_box((HudGlassRect){630, 470, 700, 500}, 0, 1, 1, 640, 480);
    CHECK(b.x == 630 && b.y == 470 && b.w == 10 && b.h == 10, "box clipped right %d,%d %dx%d", b.x, b.y, b.w, b.h);
    b = hud_glass_box((HudGlassRect){700, 10, 800, 20}, 0, 1, 1, 640, 480);
    CHECK(b.w == 0 || b.h == 0, "box outside %d,%d %dx%d", b.x, b.y, b.w, b.h);

    /* Options. */
    HudGlassConfig c = HUD_GLASS_DEFAULTS, d = HUD_GLASS_DEFAULTS;
    CHECK(!c.on, "default on");
    CHECK(hud_glass_arg(&c, "--frames", "10") == 0 && !memcmp(&c, &d, sizeof c), "other option");
    CHECK(hud_glass_arg(&c, "--hud-glass", "--frames") == 1 && c.on, "--hud-glass");
    c = d;
    CHECK(hud_glass_arg(&c, "--hud-glass-blur", "6.5") == 2 && c.on && c.blur == 6.5f, "blur %.2f", c.blur);
    CHECK(hud_glass_arg(&c, "--hud-glass-opacity", "0.2") == 2 && near(c.opacity, 0.2f), "opacity %.2f", c.opacity);
    CHECK(hud_glass_arg(&c, "--hud-glass-tint", "40a0c0ff") == 2 && c.tint == 0x40a0c0ffu, "tint %08x", c.tint);
    CHECK(hud_glass_arg(&c, "--hud-glass-tint", "0x20FFFFFF") == 2 && c.tint == 0x20ffffffu, "tint 0x %08x", c.tint);
    HudGlassConfig before = c;
    const char *badopt[][2] = {{"--hud-glass-blur", "0"}, {"--hud-glass-blur", "33"}, {"--hud-glass-blur", "4px"}, {"--hud-glass-blur", NULL},
                            {"--hud-glass-opacity", "-0.1"}, {"--hud-glass-opacity", "1.5"}, {"--hud-glass-opacity", "nan"},
                            {"--hud-glass-tint", "fff"}, {"--hud-glass-tint", "40a0c0fg"}, {"--hud-glass-tint", "140a0c0ff"},
                            {"--hud-glass-tint", ""}, {"--hud-glass-blur", ""}};
    for (unsigned i = 0; i < sizeof badopt / sizeof badopt[0]; i++)
        CHECK(hud_glass_arg(&c, badopt[i][0], badopt[i][1]) == -1 && !memcmp(&c, &before, sizeof c), "bad %s %s accepted", badopt[i][0],
              badopt[i][1] ? badopt[i][1] : "(null)");
    CHECK(hud_glass_arg(&c, "--hud-glass-foo", "1") == 0, "unknown --hud-glass-foo");
    c = d;
    CHECK(!c.hd && c.hd_height == 1.0f, "hd defaults");
    CHECK(hud_glass_arg(&c, "--hud-hd", NULL) == 1 && c.hd && !c.on, "--hud-hd");
    c = d;
    CHECK(hud_glass_arg(&c, "--hud-hd-height", "1.5") == 2 && c.hd && c.hd_height == 1.5f && !c.on, "--hud-hd-height");
    before = c;
    c = d;
    CHECK(hud_glass_arg(&c, "--hud-hd-opacity", "0.6") == 2 && c.hd && near(c.hd_opacity, 0.6f) && !c.on, "--hud-hd-opacity");
    CHECK(hud_glass_arg(&c, "--hud-hd-frost", "3") == 2 && c.hd && c.hd_frost == 3 && !c.on, "--hud-hd-frost");
    CHECK(hud_glass_arg(&c, "--hud-hd-frost", "0") == 2 && c.hd_frost == 0, "--hud-hd-frost 0");
    before = c;
    CHECK(hud_glass_arg(&c, "--hud-hd-opacity", "1.1") == -1 && hud_glass_arg(&c, "--hud-hd-opacity", "-0.1") == -1 &&
              hud_glass_arg(&c, "--hud-hd-opacity", "x") == -1 && hud_glass_arg(&c, "--hud-hd-frost", "33") == -1 &&
              hud_glass_arg(&c, "--hud-hd-frost", "-1") == -1 && hud_glass_arg(&c, "--hud-hd-frost", NULL) == -1 &&
              !memcmp(&c, &before, sizeof c),
          "bad --hud-hd-opacity / --hud-hd-frost");
    c = d;
    CHECK(d.hd_opacity == 1.0f && d.hd_frost == 0, "hd opacity/frost defaults");
    c.hd_height = 1.5f;
    c.hd = 1;
    before = c;
    CHECK(hud_glass_arg(&c, "--hud-hd-height", "0.4") == -1 && hud_glass_arg(&c, "--hud-hd-height", "5") == -1 &&
              hud_glass_arg(&c, "--hud-hd-height", NULL) == -1 && !memcmp(&c, &before, sizeof c), "bad --hud-hd-height");

    /* Panel: margin, pill corner, parameters. */
    c = d;
    RGlass g = hud_glass_panel(&c, (HudGlassRect){58.7f, 64.9f, 169.3f, 68.4f});
    CHECK(near(g.x0, 53.7f) && near(g.y0, 59.9f) && near(g.x1, 174.3f) && near(g.y1, 73.4f), "panel %.2f,%.2f-%.2f,%.2f", g.x0, g.y0, g.x1,
          g.y1);
    CHECK(near(g.corner, 6.75f) && g.blur == c.blur && g.tint == c.tint && g.rim == c.rim, "panel params %.2f", g.corner);

    test_hd();
    test_real_target();
    test_glass_style();

    if (fails) printf("%d FAILED\n", fails);
    else printf("ALL OK\n");
    return fails != 0;
}
