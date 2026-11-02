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
    float density;   /* --rain-hd-density 0.25-4: streaks per area, times the title's own density */
} RainConfig;
#define RAIN_DEFAULTS {0, RAIN_DENSITY_DEFAULT}

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
 * density (cells per area) from the layers' repeats times c->density. Returns 0 (out->slices = 0) when n is 0. */
#define RAIN_SLICES 5
#define RAIN_NEAR 1.5f
#define RAIN_FAR 24.0f
#define RAIN_REF_DIST 4.0f
int rain_build(const RainConfig *c, const RainLayer *l, const float (*scroll)[2], int n, const float ab[2], RRain *out);
#endif
