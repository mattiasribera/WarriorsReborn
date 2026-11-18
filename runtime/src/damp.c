/* --damp-streets options, damp ground, night test and fade (damp.h). */
#include "damp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int damp_arg(DampConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--damp-streets") || !strcmp(opt, "--no-damp-streets")) {
        c->on = opt[2] == 'd';
        return 1;
    }
    if (!strcmp(opt, "--damp-streets-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= DAMP_STRENGTH_MIN && f <= DAMP_STRENGTH_MAX)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    return 0;
}

int damp_night(uint32_t zenith_argb) {
    const float r = (float)(zenith_argb >> 16 & 0xFFu), g = (float)(zenith_argb >> 8 & 0xFFu), b = (float)(zenith_argb & 0xFFu);
    return 0.299f * r + 0.587f * g + 0.114f * b < DAMP_NIGHT_LUMA;
}

int damp_wet_build(const DampConfig *c, const float view[16], const float proj[16], const float ab[2], RRainWet *out) {
    /* A perspective projection (as rain.c usable_camera). */
    if (!c->on || !(proj[15] == 0 && fabsf(proj[11]) == 1) || !(proj[0] > 0) || !(proj[5] > 0)) return 0;
    RRainWet p;
    memset(&p, 0, sizeof p);
    p.depth_a = ab ? ab[0] : 1.0002f;   /* rain.h RAIN_DEPTH_A/B_DEFAULT */
    p.depth_b = ab ? ab[1] : 0.0102f;
    p.px = proj[0];
    p.py = proj[5];
    /* World z in view space, z towards the viewer (as rain_wet_build), and the camera in the world
     * (as rain_puddle_build). */
    const float back = -proj[11];
    p.up_view[0] = view[8];
    p.up_view[1] = view[9];
    p.up_view[2] = back * view[10];
    for (int j = 0; j < 3; j++) {
        p.right[j] = view[4 * j];
        p.up_v[j] = view[4 * j + 1];
        p.back[j] = back * view[4 * j + 2];
        p.eye[j] = -(view[12] * view[4 * j] + view[13] * view[4 * j + 1] + view[14] * view[4 * j + 2]);
    }
    const float k = c->strength;
    p.reflect = DAMP_REFLECT * k;
    const float d = DAMP_DARKEN * k;
    p.darken = d > DAMP_DARKEN_MAX ? DAMP_DARKEN_MAX : d;
    p.reach = DAMP_REACH;
    p.puddles = DAMP_PUDDLES;
    p.damp = k;
    *out = p;
    return 1;
}

void damp_streaks_apply(RRainWet *w) {
    w->streaks *= DAMP_STREAKS;
    w->streak_reach = DAMP_STREAK_REACH;
    w->fallback = 0;
}

float damp_fade_step(DampFade *f, int want, int has_ms, uint32_t ms) {
    float dt = DAMP_FADE_FRAME_MS;
    if (has_ms && f->has_ms) dt = ms - f->last_ms > (uint32_t)DAMP_FADE_MS ? DAMP_FADE_MS : (float)(ms - f->last_ms);
    else if (has_ms) dt = 0;   /* the first clock reading */
    if (has_ms) f->last_ms = ms;
    f->has_ms = has_ms;
    f->level += (want ? dt : -dt) / DAMP_FADE_MS;
    if (f->level > 1) f->level = 1;
    if (f->level < 0) f->level = 0;
    return f->level;
}
