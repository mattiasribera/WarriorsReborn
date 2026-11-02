/* Game-independent test of the --rain-hd options, the rain layer rule, the scroll, the depth fit
 * and the slice builder (src/rain.c). The layer quads are the ones measured in level51 chapter3. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/rain.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Layer 1 of level51 drizzle6 at one frame: corners (clip x, y), texture u, v. */
static void layer_quad(RVertex v[4], float u0, float v0, float urep, float vrep, uint32_t color) {
    static const float xy[4][2] = {{-0.919f, 1.499f}, {1.189f, 1.158f}, {0.919f, -1.499f}, {-1.189f, -1.158f}};
    const float uv[4][2] = {{u0, v0}, {u0 + urep, v0}, {u0 + urep, v0 + vrep}, {u0, v0 + vrep}};
    memset(v, 0, 4 * sizeof *v);
    for (int i = 0; i < 4; i++) {
        v[i].x = xy[i][0];
        v[i].y = xy[i][1];
        v[i].z = 0.5f;
        v[i].w = 1.0f;
        v[i].color = color;
        v[i].tex[0][0] = uv[i][0];
        v[i].tex[0][1] = uv[i][1];
        v[i].tex[0][3] = 1;
    }
}
static RainDraw layer_draw(const RVertex *v) {
    RainDraw d = {RAIN_LAYER_FVF, 5, 4, 6, 0, 1, 0x302, 0x303, v, 4.0f / 3};
    return d;
}

static void test_options(void) {
    RainConfig c = RAIN_DEFAULTS;
    CHECK(!c.on && c.density == RAIN_DENSITY_DEFAULT, "defaults: off, density %.2f", c.density);
    CHECK(rain_arg(&c, "--bloom", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(rain_arg(&c, "--rain-hd", NULL) == 1 && c.on, "--rain-hd turns it on");
    CHECK(rain_arg(&c, "--no-rain-hd", NULL) == 1 && !c.on, "--no-rain-hd turns it off (the last one wins)");
    CHECK(rain_arg(&c, "--rain-hd-density", "2.5") == 2 && c.density == 2.5f && !c.on, "--rain-hd-density sets the density only");
    CHECK(rain_arg(&c, "--rain-hd-density", "0.25") == 2 && rain_arg(&c, "--rain-hd-density", "4") == 2 && c.density == 4.0f,
          "density range 0.25-4");
    const char *bad[] = {"0", "0.2", "4.5", "-1", "x", "1x", "", "nan", NULL};
    for (int i = 0; i < 9; i++) {
        RainConfig d = RAIN_DEFAULTS;
        CHECK(rain_arg(&d, "--rain-hd-density", bad[i]) == -1 && d.density == RAIN_DENSITY_DEFAULT, "--rain-hd-density '%s' is rejected",
              bad[i] ? bad[i] : "(missing)");
    }
}

static void test_layer(void) {
    RVertex v[4];
    RainLayer l;
    memset(&l, 0, sizeof l);
    layer_quad(v, 0.6f, -0.88f, 7, 1, 0x43bebebe);
    RainDraw d = layer_draw(v);
    CHECK(rain_layer(&d, &l) == 1, "the measured layer quad is a rain layer");
    CHECK(fabsf(l.urep - 7) < 1e-4f && fabsf(l.vrep - 1) < 1e-4f, "repeats %.3f x %.3f", l.urep, l.vrep);
    CHECK(fabsf(l.u0 - 0.6f) < 1e-6f && fabsf(l.v0 + 0.88f) < 1e-6f, "scroll position %.3f, %.3f", l.u0, l.v0);
    /* v edge: (-0.919, 1.499) -> (-1.189, -1.158): (-0.27 / 2 x 4/3, 2.657 / 2) screen heights. */
    const float ex = -0.27f / 2 * 4 / 3, ey = 2.657f / 2;
    CHECK(fabsf(l.v_len - sqrtf(ex * ex + ey * ey)) < 1e-3f, "v_len %.4f screen heights per repeat", l.v_len);
    CHECK(fabsf(l.tilt - atan2f(ex, ey)) < 1e-3f && l.tilt < 0, "tilt %.4f rad (falls towards the left)", l.tilt);
    const float fx = 2.108f / 2 * 4 / 3, fy = 0.341f / 2;
    CHECK(fabsf(l.u_len - sqrtf(fx * fx + fy * fy) / 7) < 1e-3f, "u_len %.4f", l.u_len);
    CHECK(l.color == 0x43bebebe, "colour %08x", l.color);

    /* Vertex order does not matter (the quad's edges are found by their texture axes). */
    RVertex r[4] = {v[2], v[3], v[0], v[1]};
    RainLayer m;
    d = layer_draw(r);
    CHECK(rain_layer(&d, &m) == 1 && fabsf(m.tilt - l.tilt) < 1e-4f && fabsf(m.v_len - l.v_len) < 1e-4f && fabsf(m.urep - 7) < 1e-4f,
          "rotated vertex order: same layer (tilt %.4f)", m.tilt);

    /* Not a layer. */
    RainLayer keep = l;
    struct { const char *what; RainDraw d; } no[8];
    for (int i = 0; i < 8; i++) no[i].d = layer_draw(v);
    no[0].what = "depth test on", no[0].d.depth_test = 1;
    no[1].what = "FVF 0x152", no[1].d.fvf = 0x152;
    no[2].what = "a vertex program", no[2].d.fvf = 0;
    no[3].what = "additive", no[3].d.blend_dst = 1;
    no[4].what = "no blending", no[4].d.blend_enable = 0;
    no[5].what = "8 vertices", no[5].d.nv = 8;
    no[6].what = "triangle strip", no[6].d.prim = 6;
    no[7].what = "12 indices", no[7].d.ni = 12;
    for (int i = 0; i < 8; i++) CHECK(rain_layer(&no[i].d, &l) == 0 && !memcmp(&l, &keep, sizeof l), "%s: not a layer", no[i].what);
    RVertex h[4];
    memcpy(h, v, sizeof h);
    for (int i = 0; i < 4; i++) h[i].x *= 0.3f, h[i].y *= 0.3f;   /* a HUD quad inside the screen */
    d = layer_draw(h);
    CHECK(rain_layer(&d, &l) == 0, "a quad inside the screen is not a layer");
    memcpy(h, v, sizeof h);
    for (int i = 0; i < 4; i++) h[i].z = 0.4565f, h[i].w = 0.92f;
    d = layer_draw(h);
    CHECK(rain_layer(&d, &l) == 0, "z 0.4565 / w 0.92 (the HUD's depth) is not a layer");
    memcpy(h, v, sizeof h);
    h[2].color = 0xffffffff;
    d = layer_draw(h);
    CHECK(rain_layer(&d, &l) == 0, "two vertex colours: not a layer");
    memcpy(h, v, sizeof h);
    h[1].tex[0][1] += 0.5f;   /* no edge along u */
    d = layer_draw(h);
    CHECK(rain_layer(&d, &l) == 0, "texture not along the quad's edges: not a layer");
}

static void test_scroll(void) {
    RainScroll s;
    memset(&s, 0, sizeof s);
    RVertex v[4];
    RainLayer l;
    float out[2];
    const float vv[4] = {-0.880f, -0.952f, -0.024f, -0.096f};   /* measured: -0.072 per frame, wrapping */
    float fall_step = 0;
    for (int f = 0; f < 4; f++) {
        layer_quad(v, 0.6f + 0.015f * f, vv[f], 7, 1, 0x43bebebe);
        RainDraw d = layer_draw(v);
        rain_layer(&d, &l);
        rain_scroll_layer(&s, 0, &l, out);
        rain_scroll_frame(&s);
        fall_step = 0.072f * l.v_len;
        CHECK(fabsf(out[0] - f * fall_step) < 1e-3f, "frame %d: fall %.4f (want %.4f)", f, out[0], f * fall_step);
        CHECK(fabsf(out[1] + f * 0.015f * l.u_len) < 1e-4f, "frame %d: drift %.5f (u rising: towards the left)", f, out[1]);
    }
    /* A frame without the layer: it starts over. */
    rain_scroll_frame(&s);
    layer_quad(v, 0.6f, -0.5f, 7, 1, 0x43bebebe);
    RainDraw d = layer_draw(v);
    rain_layer(&d, &l);
    rain_scroll_layer(&s, 0, &l, out);
    CHECK(out[0] == 0 && out[1] == 0, "after a frame without rain: fall and drift start at 0 (%.3f, %.3f)", out[0], out[1]);
    /* Layers are independent. */
    rain_scroll_frame(&s);
    layer_quad(v, 0.4f, -0.4f, 8, 4, 0x52bebebe);
    d = layer_draw(v);
    rain_layer(&d, &l);
    rain_scroll_layer(&s, 1, &l, out);
    CHECK(out[0] == 0, "a new layer index starts at 0");
    rain_scroll_frame(&s);
    layer_quad(v, 0.41f, -0.61f, 8, 4, 0x52bebebe);
    d = layer_draw(v);
    rain_layer(&d, &l);
    rain_scroll_layer(&s, 1, &l, out);
    CHECK(fabsf(out[0] - 0.21f * l.v_len) < 1e-3f, "layer 2 falls 0.21 x v_len (%.4f)", out[0]);
    rain_scroll_layer(&s, RAIN_MAX_LAYERS, &l, out);   /* out of range: ignored */
    CHECK(out[0] == 0 && out[1] == 0, "a layer index beyond RAIN_MAX_LAYERS gives 0");
}

static void test_depth(void) {
    const float A = 1.0002f, B = 0.0102f;
    float zw[6][2];
    const float w[6] = {1.014f, 5.2f, 31.5f, 12.f, 80.f, 3.3f};
    for (int i = 0; i < 6; i++) zw[i][0] = A * w[i] - B, zw[i][1] = w[i];
    float ab[2] = {0, 0};
    CHECK(rain_depth_fit(&zw[0][0], 2, 6, ab) == 0 && fabsf(ab[0] - A) < 1e-4f && fabsf(ab[1] - B) < 1e-4f, "fit A %.5f B %.5f", ab[0], ab[1]);
    float keep[2] = {7, 7};
    float flat[3][2] = {{0.5f, 1}, {0.5f, 1}, {0.5f, 1}};
    CHECK(rain_depth_fit(&flat[0][0], 2, 3, keep) == -1 && keep[0] == 7, "w = 1 everywhere (screen-space): no fit");
    float behind[2][2] = {{A * -2 - B, -2}, {A * 5 - B, 5}};
    CHECK(rain_depth_fit(&behind[0][0], 2, 2, keep) == -1, "a vertex behind the camera: no fit");
    zw[3][0] += 0.05f;
    CHECK(rain_depth_fit(&zw[0][0], 2, 6, keep) == -1 && keep[0] == 7, "a vertex off the projection: no fit");
    float ortho[2][2] = {{0.2f, 1}, {0.8f, 1.0001f}};
    CHECK(rain_depth_fit(&ortho[0][0], 2, 2, keep) == -1, "w span under 1e-3: no fit");
    float rev[2][2] = {{0.9f, 1}, {0.1f, 9}};
    CHECK(rain_depth_fit(&rev[0][0], 2, 2, keep) == -1, "A <= 0: no fit");

    float a[5] = {1.0f, 1.0002f, 3.0f, 1.0001f, 1.0003f}, b[5] = {0.01f, 0.0102f, 0.5f, 0.0101f, 0.0103f};
    rain_depth_pick(a, b, 5, ab);
    CHECK(ab[0] == 1.0002f && ab[1] == 0.0102f, "median of the fits (%.5f, %.5f)", ab[0], ab[1]);
    rain_depth_pick(a, b, 0, ab);
    CHECK(ab[0] == RAIN_DEPTH_A_DEFAULT && ab[1] == RAIN_DEPTH_B_DEFAULT, "no fits: the default projection");
}

static void test_build(void) {
    RainConfig c = RAIN_DEFAULTS;
    c.on = 1;
    RVertex v[4];
    RainLayer l[2];
    layer_quad(v, 0.6f, -0.88f, 7, 1, 0x43bebebe);
    RainDraw d = layer_draw(v);
    rain_layer(&d, &l[0]);
    layer_quad(v, 0.4f, -0.4f, 8, 4, 0x52bebebe);
    rain_layer(&d, &l[1]);
    const float scroll[2][2] = {{10.0f, -0.5f}, {8.0f, -0.3f}};
    const float ab[2] = {1.0002f, 0.0102f};
    RRain r;
    memset(&r, 0xAB, sizeof r);
    CHECK(rain_build(&c, l, scroll, 0, ab, &r) == 0 && r.slices == 0, "no layers: no slices");
    CHECK(rain_build(&c, l, scroll, 2, ab, &r) == RAIN_SLICES && r.slices == RAIN_SLICES && RAIN_SLICES <= RENDERER_RAIN_SLICES,
          "%u slices", r.slices);
    CHECK((r.color & 0xFFFFFF) == 0xbebebe && fabsf(r.tilt - l[0].tilt) < 1e-6f, "colour %08x and tilt from the first layer", r.color);
    float prev_depth = 2;
    for (uint32_t i = 0; i < r.slices; i++) {
        const RRainSlice *q = &r.slice[i];
        const float dist = RAIN_FAR * powf(RAIN_NEAR / RAIN_FAR, (float)i / (RAIN_SLICES - 1));
        CHECK(fabsf(q->depth - (ab[0] - ab[1] / dist)) < 1e-5f, "slice %u at distance %.2f: depth %.6f", i, dist, q->depth);
        CHECK(q->depth > 0 && q->depth < 1 && q->depth < prev_depth, "slice %u: far first, depth %.6f in (0, 1)", i, q->depth);
        prev_depth = q->depth;
        const int nearest = i == RAIN_SLICES - 1;   /* lit only: a flat pass there would shade the whole screen */
        CHECK((nearest ? q->alpha == 0 : q->alpha > 0) && q->alpha <= 1 && q->fill > 0 && q->fill <= 1 && q->length > 0 && q->length < 1,
              "slice %u: alpha %.3f fill %.3f length %.3f", i, q->alpha, q->fill, q->length);
        CHECK(nearest ? q->haze == 0 : q->haze > 0 && q->haze < 0.05f, "slice %u: a light veil (%.4f), none in the nearest", i, q->haze);
        CHECK(q->scroll_y >= 0 && q->scroll_y < q->cell_h * RENDERER_RAIN_PERIOD && fabsf(q->scroll_x) < q->cell_w * RENDERER_RAIN_PERIOD,
              "slice %u: scroll wrapped by the period", i);
    }
    const RRainSlice *far = &r.slice[0], *near = &r.slice[RAIN_SLICES - 1];
    const float ratio = RAIN_FAR / RAIN_NEAR;
    CHECK(fabsf(near->cell_w / far->cell_w - ratio) < 1e-2f * ratio && fabsf(near->width / far->width - ratio) < 1e-2f * ratio &&
              fabsf(near->cell_h * near->length / (far->cell_h * far->length) - ratio) < 1e-2f * ratio,
          "near slice: cells, width and streak length %.1fx the far one's", ratio);
    /* Scroll: the mean fall (9) at RAIN_REF_DIST, scaled by RAIN_REF_DIST / distance (before wrapping). */
    const float want_near = 9.0f * RAIN_REF_DIST / RAIN_NEAR, want_far = 9.0f * RAIN_REF_DIST / RAIN_FAR;
    CHECK(fabsf(near->scroll_y - fmodf(want_near, near->cell_h * RENDERER_RAIN_PERIOD)) < 1e-3f &&
              fabsf(far->scroll_y - fmodf(want_far, far->cell_h * RENDERER_RAIN_PERIOD)) < 1e-3f,
          "scroll: near %.3f far %.3f (want %.3f, %.3f)", near->scroll_y, far->scroll_y, want_near, want_far);
    CHECK(near->scroll_x < 0 && fabsf(near->scroll_x / far->scroll_x - ratio) < 1e-2f * ratio, "drift scaled like the fall (%.4f, %.4f)",
          near->scroll_x, far->scroll_x);

    /* Density: twice the option, half the cell area. */
    RRain r2;
    c.density = 2;
    rain_build(&c, l, scroll, 2, ab, &r2);
    CHECK(fabsf(r2.slice[2].cell_w * r2.slice[2].cell_h / (r.slice[2].cell_w * r.slice[2].cell_h) - 0.5f) < 1e-3f, "density 2: half the cell area");
    CHECK(r2.slice[2].width == r.slice[2].width && r2.slice[2].cell_h * r2.slice[2].length == r.slice[2].cell_h * r.slice[2].length,
          "density keeps the streak size");
    /* The title's density (repeats) counts too: fewer repeats, larger cells. */
    c.density = 1;
    RainLayer thin[2] = {l[0], l[1]};
    thin[0].urep = 3;
    rain_build(&c, thin, scroll, 2, ab, &r2);
    CHECK(r2.slice[2].cell_w * r2.slice[2].cell_h > r.slice[2].cell_w * r.slice[2].cell_h, "fewer title repeats: larger cells");
    /* Opacity follows the title's alpha (its fade in and out). */
    RainLayer faint[2] = {l[0], l[1]};
    faint[0].color = 0x10bebebe, faint[1].color = 0x14bebebe;
    rain_build(&c, faint, scroll, 2, ab, &r2);
    CHECK(r2.slice[2].alpha < r.slice[2].alpha * 0.5f && r2.slice[2].haze < r.slice[2].haze * 0.5f,
          "faint title layers: fainter streaks and veil (%.3f vs %.3f)", r2.slice[2].alpha, r.slice[2].alpha);
    faint[0].color = faint[1].color = 0x00bebebe;
    CHECK(rain_build(&c, faint, scroll, 2, ab, &r2) == 0 && r2.slices == 0, "alpha 0 (faded out): no slices");
    /* Without a depth fit: the default projection. */
    rain_build(&c, l, scroll, 2, NULL, &r2);
    CHECK(fabsf(r2.slice[0].depth - (RAIN_DEPTH_A_DEFAULT - RAIN_DEPTH_B_DEFAULT / RAIN_FAR)) < 1e-5f, "ab NULL: default projection");
    CHECK(r2.depth_a == RAIN_DEPTH_A_DEFAULT && r2.depth_b == RAIN_DEPTH_B_DEFAULT, "ab NULL: the default projection is passed on");
}

/* The look: every streak has its own distance inside its slice's range (no flat layers), near
 * streaks are long, wide and faint, far ones short, thin and dim, and the streaks mostly brighten
 * the scene behind them (lit: tied to its lighting) rather than paint a flat colour (alpha). */
static void test_look(void) {
    RainConfig c = RAIN_DEFAULTS;
    c.on = 1;
    RVertex v[4];
    RainLayer l[2];
    layer_quad(v, 0.6f, -0.88f, 7, 1, 0x43bebebe);
    RainDraw d = layer_draw(v);
    rain_layer(&d, &l[0]);
    layer_quad(v, 0.4f, -0.4f, 8, 4, 0x52bebebe);
    rain_layer(&d, &l[1]);
    const float scroll[2][2] = {{10.0f, -0.5f}, {8.0f, -0.3f}};
    const float ab[2] = {1.00111f, 0.10011f};
    RRain r;
    memset(&r, 0, sizeof r);
    rain_build(&c, l, scroll, 2, ab, &r);
    CHECK(r.depth_a == ab[0] && r.depth_b == ab[1], "the depth projection is passed on (%.5f, %.5f)", r.depth_a, r.depth_b);
    const float step = powf(RAIN_FAR / RAIN_NEAR, 1.0f / (RAIN_SLICES - 1));   /* distance ratio of neighbouring slices */
    for (uint32_t i = 0; i < r.slices; i++) {
        const RRainSlice *q = &r.slice[i];
        const float dist = RAIN_FAR * powf(RAIN_NEAR / RAIN_FAR, (float)i / (RAIN_SLICES - 1));
        CHECK(fabsf(q->dist - dist) < 1e-3f * dist, "slice %u: distance %.3f (want %.3f)", i, q->dist, dist);
        CHECK(fabsf(q->spread - step) < 1e-3f * step, "slice %u: streaks spread up to the next farther slice (%.3f, want %.3f)", i, q->spread, step);
        CHECK(q->lit > 0 && q->lit <= 2, "slice %u: lit %.3f", i, q->lit);
        CHECK(q->alpha < q->lit * 0.25f, "slice %u: the flat colour (%.3f) is a small part of the look (lit %.3f)", i, q->alpha, q->lit);
    }
    const RRainSlice *far = &r.slice[0], *near = &r.slice[RAIN_SLICES - 1];
    CHECK(near->lit < far->lit && near->alpha < far->alpha, "near streaks are fainter per pixel (lit %.3f vs %.3f)", near->lit, far->lit);
    CHECK(near->width > far->width * 8 && near->cell_h * near->length > far->cell_h * far->length * 8, "near streaks are wider and longer");
    /* The title's fade fades the lit part too. */
    RainLayer faint[2] = {l[0], l[1]};
    faint[0].color = 0x10bebebe, faint[1].color = 0x14bebebe;
    RRain r2;
    rain_build(&c, faint, scroll, 2, ab, &r2);
    CHECK(r2.slice[2].lit < r.slice[2].lit * 0.5f, "faint title layers: less lit (%.3f vs %.3f)", r2.slice[2].lit, r.slice[2].lit);
}

/* The see-through parts of the title's blended, depth-writing world do not hide the rain. */
static void test_soft_depth(void) {
    RainSoftDepth sd = {0, 0};
    CHECK(rain_soft_depth(1, 1, 1, 0x302, 5, 1, &sd) == 1 && sd.depth_func == 5 && sd.depth_ref == RAIN_SOFT_REF,
          "the title's world draw (blended, depth write, GREATER 1): depth where mostly opaque (%u %u)", sd.depth_func, sd.depth_ref);
    CHECK(rain_soft_depth(1, 1, 1, 0x302, 0, 0, &sd) == 1 && sd.depth_func == 5 && sd.depth_ref == RAIN_SOFT_REF, "alpha test off: the same");
    CHECK(rain_soft_depth(1, 1, 1, 0x302, 7, 0x10, &sd) == 1 && sd.depth_func == 7 && sd.depth_ref == RAIN_SOFT_REF, "GREATEREQUAL 0x10: raised");
    sd.depth_func = 99;
    CHECK(rain_soft_depth(1, 1, 1, 0x302, 5, RAIN_SOFT_REF, &sd) == 0 && sd.depth_func == 99, "an alpha test already at RAIN_SOFT_REF: as recorded");
    CHECK(rain_soft_depth(1, 1, 0, 0x302, 5, 1, &sd) == 0, "not blended (solid): as recorded");
    CHECK(rain_soft_depth(1, 0, 1, 0x302, 5, 1, &sd) == 0, "no depth write: as recorded");
    CHECK(rain_soft_depth(0, 1, 1, 0x302, 5, 1, &sd) == 0, "no depth test: as recorded");
    CHECK(rain_soft_depth(1, 1, 1, 1, 5, 1, &sd) == 0, "additive (ONE): alpha is not coverage, as recorded");
    CHECK(rain_soft_depth(1, 1, 1, 0x302, 2, 0x80, &sd) == 0, "another alpha comparison (LESS): as recorded");
}

/* The camera of level51 chapter3 frame 5500 (SetTransform VIEW and PROJECTION, row vectors). */
static const float cam_view[16] = {0.5153f, -0.1984f, 0.8337f, 0, 0.8570f, 0.1193f, -0.5013f, 0, 0, 0.9728f, 0.2315f, 0, -322.875f, -23.382f, 66.656f, 1};
static const float cam_proj[16] = {1.3971f, 0, 0, 0, 0, 1.9955f, 0, 0, 0, 0, -1.0007f, -1, 0, 0, -0.1001f, 0};

static void splash_layers(RainLayer l[2], uint32_t c1, uint32_t c2) {
    RVertex v[4];
    layer_quad(v, 0.6f, -0.88f, 7, 1, c1);
    RainDraw d = layer_draw(v);
    rain_layer(&d, &l[0]);
    layer_quad(v, 0.4f, -0.4f, 8, 4, c2);
    rain_layer(&d, &l[1]);
}
/* Impacts per square world unit and second of a splash grid. */
static float splash_rate(const RRainSplash *p) { return p->chance / (p->period * p->cell * p->cell); }

static void test_splash(void) {
    RainConfig c = RAIN_DEFAULTS;
    CHECK(c.splash == 1, "splashes on by default (with --rain-hd)");
    CHECK(rain_arg(&c, "--no-rain-hd-splash", NULL) == 1 && !c.splash && !c.on, "--no-rain-hd-splash turns them off only");
    CHECK(rain_arg(&c, "--rain-hd-splash", NULL) == 1 && c.splash, "--rain-hd-splash turns them on");
    c.on = 1;
    RainLayer l[2];
    splash_layers(l, 0x43bebebe, 0x52bebebe);
    const float ab[2] = {1.0007f, 0.1001f};
    RRainSplash p;
    memset(&p, 0, sizeof p);
    CHECK(rain_splash_build(&c, l, 2, cam_view, cam_proj, 5000, ab, &p) == 1, "built for the title's camera");
    CHECK(p.depth_a == ab[0] && p.depth_b == ab[1] && fabsf(p.px - 1.3971f) < 1e-6f && fabsf(p.py - 1.9955f) < 1e-6f,
          "depth projection and x/y scale (%.4f %.4f)", p.px, p.py);
    CHECK(p.up[0] == 0 && p.up[1] == 0 && p.up[2] == 1, "world up is +z");
    /* A world point seen by the camera is rebuilt from its view x, y and distance. */
    const float wp[3] = {-310.0f, 20.0f, -60.0f};
    float vp[3];
    for (int k = 0; k < 3; k++) vp[k] = wp[0] * cam_view[k] + wp[1] * cam_view[4 + k] + wp[2] * cam_view[8 + k] + cam_view[12 + k];
    const float d = -vp[2];   /* right-handed view: the camera looks down -z */
    float err = 0;
    for (int k = 0; k < 3; k++) err = fmaxf(err, fabsf(p.eye[k] + vp[0] * p.right[k] + vp[1] * p.up_v[k] - d * p.back[k] - wp[k]));
    CHECK(d > 0 && err < 1e-4f * d + 2e-3f, "a world point at view distance %.2f is rebuilt from the camera (error %.5f; the matrix has 4 decimals)", d, err);
    CHECK(fabsf(p.time - 5.0f) < 1e-6f, "time: the title's clock in seconds (%.4f)", p.time);
    RRainSplash q;
    rain_splash_build(&c, l, 2, cam_view, cam_proj, RAIN_SPLASH_CLOCK_WRAP + 5000, ab, &q);
    CHECK(fabsf(q.time - 5.0f) < 1e-6f, "the clock wraps every RAIN_SPLASH_CLOCK_WRAP ms (%.4f)", q.time);
    CHECK((p.color & 0xFFFFFF) == 0xbebebe, "colour of the title's rain (%08x)", p.color);
    CHECK(p.cell > 0 && p.period > 0.5f && p.chance > 0 && p.chance <= 1 && p.fade > 0, "grid %.3f units, %.2f s, chance %.2f, fade %.1f",
          p.cell, p.period, p.chance, p.fade);
    CHECK(p.splash > 0 && p.splash <= 4 && p.ripple > 0 && p.ripple <= 4 && p.flat > 0 && p.flat <= 1, "strengths %.2f %.2f, flat %.2f", p.splash,
          p.ripple, p.flat);
    const float base = splash_rate(&p);
    CHECK(base > 0.25f * RAIN_SPLASH_RATE && base < 4 * RAIN_SPLASH_RATE, "rate %.2f per unit^2 and s", base);
    RainConfig c2 = c;
    c2.density = 2;
    rain_splash_build(&c2, l, 2, cam_view, cam_proj, 5000, ab, &q);
    CHECK(fabsf(splash_rate(&q) / base - 2) < 1e-3f, "--rain-hd-density 2: twice the impacts (%.3f)", splash_rate(&q) / base);
    c2.density = 4;
    rain_splash_build(&c2, l, 2, cam_view, cam_proj, 5000, ab, &q);
    CHECK(fabsf(splash_rate(&q) / base - 4) < 1e-3f && q.chance <= 1 && q.cell <= p.cell, "density 4: four times, chance %.2f, cell %.3f",
          q.chance, q.cell);
    RainLayer faint[2];
    splash_layers(faint, 0x10bebebe, 0x12bebebe);
    rain_splash_build(&c, faint, 2, cam_view, cam_proj, 5000, ab, &q);
    CHECK(q.splash < p.splash && q.ripple < p.ripple, "faint title rain: fainter splashes (%.2f vs %.2f)", q.splash, p.splash);
    /* A left-handed camera (proj[11] = +1) looks down +z. */
    float lh[16];
    memcpy(lh, cam_proj, sizeof lh);
    lh[10] = -lh[10];
    lh[11] = 1;
    rain_splash_build(&c, l, 2, cam_view, lh, 5000, ab, &q);
    CHECK(q.back[0] == -p.back[0] && q.back[1] == -p.back[1] && q.back[2] == -p.back[2], "left-handed projection: back is -z");
    /* Not built. */
    memset(&q, 0x5A, sizeof q);
    const RRainSplash keep = q;
    RainConfig off = c;
    off.splash = 0;
    CHECK(rain_splash_build(&off, l, 2, cam_view, cam_proj, 5000, ab, &q) == 0, "--no-rain-hd-splash: none");
    CHECK(rain_splash_build(&c, l, 0, cam_view, cam_proj, 5000, ab, &q) == 0, "no layers: none");
    float ortho[16];
    memcpy(ortho, cam_proj, sizeof ortho);
    ortho[11] = 0;
    ortho[15] = 1;
    CHECK(rain_splash_build(&c, l, 2, cam_view, ortho, 5000, ab, &q) == 0, "orthographic projection (overlays): none");
    float skew[16];
    memcpy(skew, cam_view, sizeof skew);
    skew[0] = 2;
    CHECK(rain_splash_build(&c, l, 2, skew, cam_proj, 5000, ab, &q) == 0, "a view that is not a rotation: none");
    CHECK(!memcmp(&q, &keep, sizeof q), "out unchanged when not built");
}

int main(void) {
    test_options();
    test_splash();
    test_layer();
    test_scroll();
    test_depth();
    test_build();
    test_look();
    test_soft_depth();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
