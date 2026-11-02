/* --rain-hd: the options, the rule that recognises the title's rain layers in
 * its draws and reads their parameters, the scroll they accumulate from frame to frame, the fit of
 * the scene's depth projection and the procedural rain slices built from all of it
 * (renderer_rain). */
#ifndef RAIN_H
#define RAIN_H
#include <stddef.h>
#include <stdint.h>
#include "renderer.h"

#define RAIN_DENSITY_DEFAULT 1.0f
#define RAIN_DENSITY_MIN 0.25f
#define RAIN_DENSITY_MAX 4.0f
typedef struct {
    int on;          /* --rain-hd / --no-rain-hd */
    float density;   /* --rain-hd-density 0.25-4: streaks (and splashes) per area, times the title's own density */
    int splash;      /* --rain-hd-splash / --no-rain-hd-splash: splashes and ripples on the ground (with --rain-hd) */
} RainConfig;
#define RAIN_DEFAULTS {0, RAIN_DENSITY_DEFAULT, 1}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a rain option, -1 for a missing or out-of-range value (c unchanged). */
int rain_arg(RainConfig *c, const char *opt, const char *val);

/* The title's rain (Lua StartRain; OE_Rain queues it, the particle renderer draws it) is two
 * full-screen layers, each one quad with a scrolling streak texture. A layer draw: fixed-function
 * FVF XYZ | DIFFUSE | TEX1 (0x142), a triangle list of 4 vertices and 6 indices, depth test off,
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending, every vertex at clip z 0.5 and w 1 and outside the
 * screen (the quad is larger than the screen and slanted), one vertex colour, and two edges along
 * the texture axes (u across, v down). Code addresses are not used. */
#define RAIN_LAYER_FVF 0x142u
typedef struct {
    uint32_t fvf, prim, nv, ni;   /* fixed-function FVF (0 for a vertex program); D3DPRIMITIVETYPE (5 = list) */
    uint32_t depth_test, blend_enable, blend_src, blend_dst;   /* RState enums */
    const RVertex *v;             /* the draw's nv vertices (clip space) */
    float aspect;                 /* viewport width / height */
} RainDraw;
typedef struct {
    float u0, v0;          /* texture coordinates at the first vertex: the layer's scroll position */
    float u_len, v_len;    /* screen heights per texture unit across (u) and down (v) */
    float urep, vrep;      /* texture repeats across the quad (the title's density) */
    float tilt;            /* radians: the v (fall) direction from straight down, positive = towards the right */
    uint32_t color;        /* ARGB8888 vertex colour (StartRain's colour and alpha, faded by the title) */
} RainLayer;
/* Returns 1 and the layer's parameters when the draw is a rain layer, else 0 (out unchanged). */
int rain_layer(const RainDraw *d, RainLayer *out);

/* Scroll: how far the title moved each layer, in screen heights, summed over the frames it has
 * been drawn in a row (fall: down its v direction; drift: across, positive = towards the right).
 * Texture coordinates wrap, so each frame's step is taken as the nearest one (under half a
 * repeat). A layer index (its order among the frame's layer draws) that was not drawn in a frame
 * starts over. */
#define RAIN_MAX_LAYERS 4
typedef struct {
    float u0[RAIN_MAX_LAYERS], v0[RAIN_MAX_LAYERS];
    double fall[RAIN_MAX_LAYERS], drift[RAIN_MAX_LAYERS];
    uint8_t valid[RAIN_MAX_LAYERS], seen[RAIN_MAX_LAYERS];
} RainScroll;
/* Layer k (0..RAIN_MAX_LAYERS-1) of the current frame; out = its fall and drift so far. */
void rain_scroll_layer(RainScroll *s, int k, const RainLayer *l, float out[2]);
/* At the end of every frame. */
void rain_scroll_frame(RainScroll *s);

/* Depth projection of a perspective draw: clip z = A w - B (A, B > 0), so view depth d is at NDC
 * depth A - B / d. Fits it to n vertices (zw: the first vertex's z with its w right after it, stride
 * floats from one vertex to the next). Returns 0 and ab = {A, B} when the vertices span w (at least
 * 1e-3 of its size), every w > 0 and every vertex lies on the fit; -1 otherwise (ab unchanged). */
int rain_depth_fit(const float *zw, size_t stride, uint32_t n, float ab[2]);
/* The frame's projection from per-draw fits (a, b: n values each, reordered): the median of each;
 * without fits the default below. */
#define RAIN_DEPTH_A_DEFAULT 1.0002f
#define RAIN_DEPTH_B_DEFAULT 0.0102f
void rain_depth_pick(float *a, float *b, uint32_t n, float ab[2]);

/* The procedural rain for the frame's layers (n of them, scroll[i] = rain_scroll_layer's out) and
 * depth projection ab: RAIN_SLICES slices from RAIN_NEAR to RAIN_FAR view units, far first. Sizes
 * and speeds follow the distance (a slice twice as far has half the cell size, streak length and
 * width and moves half as fast on screen), the scroll is the layers' mean fall and drift scaled
 * to each slice's distance (RAIN_REF_DIST moves exactly as the title's layers), tilt and colour
 * come from the first layer, opacity (streaks and veil) from the layers' strongest alpha and the
 * density (cells per area) from the layers' repeats times c->density. Each slice's streaks spread
 * in distance up to the next farther slice (RRainSlice.spread, the depth projection in
 * out->depth_a/b), so no streak sits on a flat layer. A streak mostly brightens the scene behind
 * it (RRainSlice.lit) and adds only a little flat colour (alpha): near streaks long, wide and
 * faint, far ones short, thin and dim. The nearest slice is lit only (no flat colour, no veil):
 * nothing hides it, so a flat pass there would shade the whole screen for little. Returns 0 (out->slices = 0) when n is 0. */
#define RAIN_SLICES 4
#define RAIN_NEAR 1.5f
#define RAIN_FAR 24.0f
#define RAIN_REF_DIST 4.0f
/* Depth of the scene under the rain: the title draws most of its world blended (SRC_ALPHA) with
 * depth writes and an alpha test that keeps nearly every texel (GREATER 1), so the see-through
 * parts of fences, wire and crates write depth like solid ones and would hide the rain behind them
 * in flat panels. In a frame with --rain-hd rain, such a draw before the rain is drawn with its
 * alpha test raised to RAIN_SOFT_REF: texels under an eighth opaque neither write depth nor add
 * their (at most an eighth of a) colour; one draw, so it costs nothing. Returns 1 and that alpha
 * test (D3DCMPFUNC numbering, ref 0-255) for a depth-tested, depth-writing draw blended by its
 * alpha (SRC_ALPHA) whose alpha test is off or GREATER / GREATEREQUAL below RAIN_SOFT_REF; 0
 * otherwise (draw it as recorded). */
#define RAIN_SOFT_REF 0x20u
typedef struct { uint32_t depth_func, depth_ref; } RainSoftDepth;
int rain_soft_depth(int depth_test, uint32_t depth_write, uint32_t blend_enable, uint32_t blend_src, uint32_t alpha_func, uint32_t alpha_ref, RainSoftDepth *out);

int rain_build(const RainConfig *c, const RainLayer *l, const float (*scroll)[2], int n, const float ab[2], RRain *out);

/* Splashes and ripples on the ground (renderer_rain_splash) for a frame with the title's rain
 * layers l (n of them): drops land on a grid of world cells (one chance per cell and period, the
 * rate RAIN_SPLASH_RATE impacts per square world unit and second times the same density as the
 * streaks; above one chance per cell the cells shrink), each a short crown of droplets and, on flat
 * ground, a ring. The camera comes from the frame's view and projection matrices (D3D row-vector
 * matrices, 16 floats: view = world x view, clip = view x proj; perspective with proj[11] = +-1 and
 * proj[15] = 0, an orthonormal rotation); world up is +z (the title's world). The time is the
 * title's clock (clock_ms, wrapped every RAIN_SPLASH_CLOCK_WRAP ms), so splashes run at the same
 * speed at 30 and 60 frames per second and stop when the title's clock does. Colour and strength
 * follow the layers like the streaks (rain_build). ab: the frame's depth projection. Returns 1 and
 * out, or 0 (out unchanged) when splashes are off (c->splash), without layers or without a usable
 * camera. */
#define RAIN_SPLASH_RATE 30.0f
#define RAIN_SPLASH_CELL 0.5f
#define RAIN_SPLASH_PERIOD 0.6f
#define RAIN_SPLASH_FADE 22.0f
#define RAIN_SPLASH_CLOCK_WRAP (1u << 24)
int rain_splash_build(const RainConfig *c, const RainLayer *l, int n, const float view[16], const float proj[16], uint32_t clock_ms,
                      const float ab[2], RRainSplash *out);
#endif
