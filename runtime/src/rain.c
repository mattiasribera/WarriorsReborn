/* --rain-hd options, rain layer rule, scroll, depth fit and slices (rain.h). */
#include "rain.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Streak model, in screen heights at view distance 1 (a slice at distance d uses them / d): the
 * screen height a world unit covers there (about a 60 degree vertical field of view), a streak's
 * length (fall during one exposure, with motion blur) and width, and the cell spacing across. A
 * streak fills LEN_FRAC of its cell's height, so cell_h x d is the same for every slice and the
 * fall can wrap by one period for all of them (RAIN_FALL_PERIOD). */
#define VIEW_K 0.9f
#define STREAK_LEN (VIEW_K * 0.45f)
#define STREAK_WIDTH (VIEW_K * 0.009f)
#define CELL_W (VIEW_K * 0.3f)
#define LEN_FRAC 0.55f
#define CELL_H (STREAK_LEN / LEN_FRAC)
#define RAIN_FALL_PERIOD ((double)CELL_H * RENDERER_RAIN_PERIOD / RAIN_REF_DIST)
/* The title's layers at full rain (level51 drizzle6: 7 x 1 and 8 x 4 repeats, alpha about 0.35 after
 * its fade) give density and opacity 1. */
#define TITLE_REPEATS 39.0f
#define TITLE_ALPHA 0.35f
/* A streak brightens what lies behind it by up to LIT x the title's colour (the scene's light: bright
 * near lamps, faint in the dark) and adds a little flat colour of opacity ALPHA (so it does not vanish
 * in the dark), in the far and the near slice (near streaks are wide and soft, far ones dimmed
 * further by their sub-pixel width). */
#define LIT_FAR 2.0f
#define LIT_NEAR 1.0f
#define ALPHA_FAR 0.22f
#define ALPHA_NEAR 0.10f
#define FILL 0.75f
/* Veil per slice but the nearest (the rain between slices): the three over open sky add about 3%,
 * what the title's layers add to a night scene on average. */
#define HAZE 0.0115f

static int parse_float(const char *v, float lo, float hi, float *out) {
    char *end;
    if (!v || !*v) return -1;
    float f = (float)strtod(v, &end);
    if (*end || !(f >= lo && f <= hi)) return -1;   /* NaN fails the range check */
    *out = f;
    return 0;
}

int rain_arg(RainConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--rain-hd") || !strcmp(opt, "--no-rain-hd")) {
        c->on = opt[2] == 'r';
        return 1;
    }
    if (!strcmp(opt, "--rain-hd-splash") || !strcmp(opt, "--no-rain-hd-splash")) {
        c->splash = opt[2] == 'r';
        return 1;
    }
    if (!strcmp(opt, "--rain-hd-density")) return parse_float(val, RAIN_DENSITY_MIN, RAIN_DENSITY_MAX, &c->density) ? -1 : 2;
    return 0;
}

static int near_eq(float a, float b, float eps) { return fabsf(a - b) <= eps; }

/* Screen-height vector from vertex p to q (y down) and its length. */
static float edge(const RVertex *p, const RVertex *q, float aspect, float e[2]) {
    e[0] = (q->x - p->x) * 0.5f * aspect;
    e[1] = -(q->y - p->y) * 0.5f;
    return sqrtf(e[0] * e[0] + e[1] * e[1]);
}

int rain_layer(const RainDraw *d, RainLayer *out) {
    if (d->fvf != RAIN_LAYER_FVF || d->prim != 5 || d->nv != 4 || d->ni != 6 || d->depth_test || !d->blend_enable ||
        d->blend_src != 0x302 || d->blend_dst != 0x303 || !d->v)
        return 0;
    const RVertex *v = d->v;
    for (int i = 0; i < 4; i++)
        if (!near_eq(v[i].z, 0.5f, 1e-3f) || !near_eq(v[i].w, 1.0f, 1e-3f) || (fabsf(v[i].x) <= 1 && fabsf(v[i].y) <= 1) ||
            v[i].color != v[0].color)
            return 0;
    /* The neighbours of vertex 0 along v (same u) and along u (same v). */
    const RVertex *a = &v[0], *b = NULL, *c = NULL;
    for (int i = 1; i < 4; i++) {
        const float du = v[i].tex[0][0] - a->tex[0][0], dv = v[i].tex[0][1] - a->tex[0][1];
        if (fabsf(du) < 1e-4f && fabsf(dv) > 1e-3f) b = &v[i];
        if (fabsf(dv) < 1e-4f && fabsf(du) > 1e-3f) c = &v[i];
    }
    if (!b || !c) return 0;
    const float aspect = d->aspect > 0 ? d->aspect : 4.0f / 3;
    float ev[2], eu[2];
    const float lv = edge(a, b, aspect, ev), lu = edge(a, c, aspect, eu);
    if (lv <= 0 || lu <= 0) return 0;
    const float dv = b->tex[0][1] - a->tex[0][1], du = c->tex[0][0] - a->tex[0][0];
    out->u0 = a->tex[0][0];
    out->v0 = a->tex[0][1];
    out->vrep = fabsf(dv);
    out->urep = fabsf(du);
    out->v_len = lv / out->vrep;
    out->u_len = lu / out->urep;
    const float s = dv > 0 ? 1.0f : -1.0f;   /* towards increasing v: down */
    out->tilt = atan2f(s * ev[0], s * ev[1]);
    out->color = a->color;
    return 1;
}

void rain_scroll_layer(RainScroll *s, int k, const RainLayer *l, float out[2]) {
    out[0] = out[1] = 0;
    if (k < 0 || k >= RAIN_MAX_LAYERS) return;
    if (s->valid[k]) {
        float dv = l->v0 - s->v0[k], du = l->u0 - s->u0[k];
        dv -= floorf(dv + 0.5f);   /* the nearest step: texture coordinates wrap */
        du -= floorf(du + 0.5f);
        s->fall[k] = fmod(s->fall[k] - dv * l->v_len, RAIN_FALL_PERIOD);   /* v falling at a point: the texture moves down */
        if (s->fall[k] < 0) s->fall[k] += RAIN_FALL_PERIOD;
        s->drift[k] -= du * l->u_len;
    } else {
        s->fall[k] = s->drift[k] = 0;
        s->valid[k] = 1;
    }
    s->u0[k] = l->u0;
    s->v0[k] = l->v0;
    s->seen[k] = 1;
    out[0] = (float)s->fall[k];
    out[1] = (float)s->drift[k];
}

void rain_scroll_frame(RainScroll *s) {
    for (int k = 0; k < RAIN_MAX_LAYERS; k++) {
        s->valid[k] = s->valid[k] && s->seen[k];
        s->seen[k] = 0;
    }
}

int rain_depth_fit(const float *zw, size_t stride, uint32_t n, float ab[2]) {
    if (n < 2) return -1;
    uint32_t lo = 0, hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        const float w = zw[i * stride + 1];
        if (!(w > 0)) return -1;
        if (w < zw[lo * stride + 1]) lo = i;
        if (w > zw[hi * stride + 1]) hi = i;
    }
    const double w0 = zw[lo * stride + 1], w1 = zw[hi * stride + 1], z0 = zw[lo * stride], z1 = zw[hi * stride];
    if (w1 - w0 <= 1e-3 * w1) return -1;
    const double A = (z1 - z0) / (w1 - w0), B = A * w0 - z0;
    if (!(A > 0) || !(B > 0)) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const double w = zw[i * stride + 1];
        if (fabs(A * w - B - zw[i * stride]) > 1e-4 * w + 1e-5) return -1;
    }
    ab[0] = (float)A;
    ab[1] = (float)B;
    return 0;
}

static int cmp_float(const void *x, const void *y) {
    const float a = *(const float *)x, b = *(const float *)y;
    return (a > b) - (a < b);
}

void rain_depth_pick(float *a, float *b, uint32_t n, float ab[2]) {
    if (!n) {
        ab[0] = RAIN_DEPTH_A_DEFAULT;
        ab[1] = RAIN_DEPTH_B_DEFAULT;
        return;
    }
    qsort(a, n, sizeof *a, cmp_float);
    qsort(b, n, sizeof *b, cmp_float);
    ab[0] = a[n / 2];
    ab[1] = b[n / 2];
}

/* The layers' intensity (strongest alpha against the title's full rain, at most 1.5) and density
 * (repeats against the full rain, 0.25-2, times --rain-hd-density). Returns 0 when no layer shows. */
static int rain_strength(const RainConfig *c, const RainLayer *l, int n, float *intensity, float *density) {
    float alpha = 0, repeats = 0;
    for (int i = 0; i < n; i++) {
        const float a = (float)(l[i].color >> 24) / 255.0f;
        if (a > alpha) alpha = a;
        repeats += l[i].urep * l[i].vrep;
    }
    if (alpha <= 0) return 0;
    const float in = alpha / TITLE_ALPHA, de = repeats / TITLE_REPEATS;
    *intensity = in > 1.5f ? 1.5f : in;
    *density = (de < 0.25f ? 0.25f : de > 2 ? 2 : de) * (c->density > 0 ? c->density : 1);
    return 1;
}

int rain_build(const RainConfig *c, const RainLayer *l, const float (*scroll)[2], int n, const float ab[2], RRain *out) {
    out->slices = 0;
    float intensity, density, fall = 0, drift = 0;
    if (n <= 0 || !rain_strength(c, l, n, &intensity, &density)) return 0;
    for (int i = 0; i < n; i++) {
        fall += scroll[i][0];
        drift += scroll[i][1];
    }
    fall /= (float)n;
    drift /= (float)n;
    const float A = ab ? ab[0] : RAIN_DEPTH_A_DEFAULT, B = ab ? ab[1] : RAIN_DEPTH_B_DEFAULT;
    out->tilt = l[0].tilt;
    out->color = l[0].color | 0xFF000000u;
    out->depth_a = A;
    out->depth_b = B;
    const float spread = powf(RAIN_FAR / RAIN_NEAR, 1.0f / (RAIN_SLICES - 1));
    for (int i = 0; i < RAIN_SLICES; i++) {
        const float t = (float)i / (RAIN_SLICES - 1);   /* 0 far .. 1 near */
        const float dist = RAIN_FAR * powf(RAIN_NEAR / RAIN_FAR, t);
        RRainSlice *q = &out->slice[i];
        const float depth = A - B / dist;
        q->depth = depth < 0 ? 0 : depth > 1 ? 1 : depth;
        q->cell_w = CELL_W / dist / density;
        q->cell_h = CELL_H / dist;
        q->length = LEN_FRAC;
        q->width = STREAK_WIDTH / dist;
        const float a = (ALPHA_FAR + (ALPHA_NEAR - ALPHA_FAR) * t) * intensity;
        const int nearest = i == RAIN_SLICES - 1;
        q->alpha = nearest ? 0 : a > 1 ? 1 : a;
        const float lit = (LIT_FAR + (LIT_NEAR - LIT_FAR) * t) * intensity;
        q->lit = lit > 2 ? 2 : lit;
        q->dist = dist;
        q->spread = spread;
        q->fill = FILL;
        q->haze = nearest ? 0 : HAZE * intensity;
        const float scale = RAIN_REF_DIST / dist, py = q->cell_h * RENDERER_RAIN_PERIOD, px = q->cell_w * RENDERER_RAIN_PERIOD;
        q->scroll_y = fmodf(fall * scale, py);
        if (q->scroll_y < 0) q->scroll_y += py;
        q->scroll_x = fmodf(drift * scale, px);
    }
    out->slices = RAIN_SLICES;
    return RAIN_SLICES;
}

int rain_soft_depth(int depth_test, uint32_t depth_write, uint32_t blend_enable, uint32_t blend_src, uint32_t alpha_func, uint32_t alpha_ref,
                    RainSoftDepth *out) {
    if (!depth_test || !depth_write || !blend_enable || blend_src != 0x302) return 0;   /* SRC_ALPHA: alpha is coverage */
    if (alpha_func && alpha_func != 5 && alpha_func != 7) return 0;                  /* off, GREATER, GREATEREQUAL */
    if (alpha_func && alpha_ref >= RAIN_SOFT_REF) return 0;
    out->depth_func = alpha_func ? alpha_func : 5;
    out->depth_ref = RAIN_SOFT_REF;
    return 1;
}

/* Splash brightening of droplets and rings at the title's full rain (the streaks' far lit is 2). */
#define SPLASH_LIT 3.0f
#define RIPPLE_LIT 2.0f
#define SPLASH_FLAT 0.12f

int rain_splash_build(const RainConfig *c, const RainLayer *l, int n, const float view[16], const float proj[16], uint32_t clock_ms,
                      const float ab[2], RRainSplash *out) {
    float intensity, density;
    if (!c->splash || n <= 0 || !rain_strength(c, l, n, &intensity, &density)) return 0;
    /* Perspective (w = +-view z) with positive x/y scales. */
    if (!(fabsf(proj[11]) == 1 && proj[15] == 0 && proj[3] == 0 && proj[7] == 0 && proj[0] > 0 && proj[5] > 0)) return 0;
    /* The view's rotation rows (world axis i in view space) orthonormal. */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            const float dot = view[4 * i] * view[4 * j] + view[4 * i + 1] * view[4 * j + 1] + view[4 * i + 2] * view[4 * j + 2];
            if (!(fabsf(dot - (i == j)) < 1e-2f)) return 0;
        }
    RRainSplash p;
    memset(&p, 0, sizeof p);
    p.depth_a = ab ? ab[0] : RAIN_DEPTH_A_DEFAULT;
    p.depth_b = ab ? ab[1] : RAIN_DEPTH_B_DEFAULT;
    p.px = proj[0];
    p.py = proj[5];
    /* View = world R + t: view axis k in the world is column k of R, the eye -t R^T. The camera
     * looks down -proj[11] z, so back (towards the viewer) is proj[11] x ... negated. */
    const float back = -proj[11];
    for (int j = 0; j < 3; j++) {
        p.right[j] = view[4 * j];
        p.up_v[j] = view[4 * j + 1];
        p.back[j] = back * view[4 * j + 2];
        p.eye[j] = -(view[12] * view[4 * j] + view[13] * view[4 * j + 1] + view[14] * view[4 * j + 2]);
    }
    p.up[2] = 1;
    p.time = (float)(clock_ms % RAIN_SPLASH_CLOCK_WRAP) / 1000.0f;
    /* RAIN_SPLASH_RATE x density impacts per unit^2 and second: one chance per cell and period. */
    const float rate = RAIN_SPLASH_RATE * density;
    p.period = RAIN_SPLASH_PERIOD;
    p.cell = RAIN_SPLASH_CELL;
    p.chance = rate * p.period * p.cell * p.cell;
    if (p.chance > 1) {
        p.cell = sqrtf(1.0f / (rate * p.period));
        p.chance = 1;
    }
    const float sp = SPLASH_LIT * intensity, rp = RIPPLE_LIT * intensity;
    p.splash = sp > 4 ? 4 : sp;
    p.ripple = rp > 4 ? 4 : rp;
    p.flat = SPLASH_FLAT;
    p.fade = RAIN_SPLASH_FADE;
    p.color = l[0].color | 0xFF000000u;
    *out = p;
    return 1;
}
