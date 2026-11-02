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
#define STREAK_WIDTH (VIEW_K * 0.006f)
#define CELL_W (VIEW_K * 0.3f)
#define LEN_FRAC 0.55f
#define CELL_H (STREAK_LEN / LEN_FRAC)
#define RAIN_FALL_PERIOD ((double)CELL_H * RENDERER_RAIN_PERIOD / RAIN_REF_DIST)
/* The title's layers at full rain (level51 drizzle6: 7 x 1 and 8 x 4 repeats, alpha about 0.35 after
 * its fade) give density and opacity 1. */
#define TITLE_REPEATS 39.0f
#define TITLE_ALPHA 0.35f
/* Opacity of a streak in the far and the near slice (near streaks are wide and soft). */
#define ALPHA_FAR 0.55f
#define ALPHA_NEAR 0.22f
#define FILL 0.75f
/* Veil per slice (the rain between slices): five slices over open sky add about 3%, what the title's
 * layers add to a night scene on average. */
#define HAZE 0.007f

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

int rain_build(const RainConfig *c, const RainLayer *l, const float (*scroll)[2], int n, const float ab[2], RRain *out) {
    out->slices = 0;
    if (n <= 0) return 0;
    float alpha = 0, repeats = 0, fall = 0, drift = 0;
    for (int i = 0; i < n; i++) {
        const float a = (float)(l[i].color >> 24) / 255.0f;
        if (a > alpha) alpha = a;
        repeats += l[i].urep * l[i].vrep;
        fall += scroll[i][0];
        drift += scroll[i][1];
    }
    if (alpha <= 0) return 0;
    fall /= (float)n;
    drift /= (float)n;
    float intensity = alpha / TITLE_ALPHA, density = repeats / TITLE_REPEATS;
    intensity = intensity > 1.5f ? 1.5f : intensity;
    density = (density < 0.25f ? 0.25f : density > 2 ? 2 : density) * (c->density > 0 ? c->density : 1);
    const float A = ab ? ab[0] : RAIN_DEPTH_A_DEFAULT, B = ab ? ab[1] : RAIN_DEPTH_B_DEFAULT;
    out->tilt = l[0].tilt;
    out->color = l[0].color | 0xFF000000u;
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
        q->alpha = a > 1 ? 1 : a;
        q->fill = FILL;
        q->haze = HAZE * intensity;
        const float scale = RAIN_REF_DIST / dist, py = q->cell_h * RENDERER_RAIN_PERIOD, px = q->cell_w * RENDERER_RAIN_PERIOD;
        q->scroll_y = fmodf(fall * scale, py);
        if (q->scroll_y < 0) q->scroll_y += py;
        q->scroll_x = fmodf(drift * scale, px);
    }
    out->slices = RAIN_SLICES;
    return RAIN_SLICES;
}
