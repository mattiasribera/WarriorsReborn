/* --dynres: the render-size controller (see dynres.h). */
#include "dynres.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

DynresOptions dynres_options = {0, 0, DYNRES_MIN_RATIO_DEFAULT, 0};

static int parse_num(const char *s, float lo, float hi, float *out) {
    if (!s || !*s) return -1;
    char *end = NULL;
    const double v = strtod(s, &end);
    if (end == s || *end || !isfinite(v) || v < lo || v > hi) return -1;
    *out = (float)v;
    return 0;
}

int dynres_parse_target(const char *s, float *ms) {
    if (s && !strcmp(s, "auto")) {
        *ms = 0;
        return 0;
    }
    return parse_num(s, DYNRES_TARGET_MIN, DYNRES_TARGET_MAX, ms);
}

int dynres_parse_min(const char *s, float *ratio) { return parse_num(s, 1.1f, 3.0f, ratio); }

float dynres_target(float option_ms) { return option_ms > 0 ? option_ms : DYNRES_AUTO_MS; }

float dynres_target_for(float option_ms, int native) { return option_ms > 0 ? option_ms : native ? DYNRES_AUTO_NATIVE_MS : DYNRES_AUTO_MS; }

float dynres_floor(float min_ratio, int min_set, int native) {
    if (native && !min_set) return 1.0f / DYNRES_NATIVE_MIN_RATIO;
    return min_ratio > 1 ? 1.0f / min_ratio : 1.0f / DYNRES_MIN_RATIO_DEFAULT;
}

void dynres_init(Dynres *d, float target_ms, float scale_min, float scale_max) {
    memset(d, 0, sizeof *d);
    d->target_ms = target_ms > 0 ? target_ms : DYNRES_AUTO_MS;
    d->scale_max = scale_max > 0 && scale_max <= 1 ? scale_max : 1;
    d->scale_min = scale_min > 0 && scale_min <= d->scale_max ? scale_min : d->scale_max;
    d->scale = d->scale_max;
    d->ema_ms = -1;
    d->up_frames = DYNRES_UP_FRAMES;
    d->since_up = 1 << 20;
}

static int set_scale(Dynres *d, float s) {
    if (s < d->scale_min) s = d->scale_min;
    if (s > d->scale_max) s = d->scale_max;
    const int bound = s == d->scale_min || s == d->scale_max;
    if (s == d->scale || (fabsf(s - d->scale) < DYNRES_MIN_CHANGE && !bound)) return 0;
    /* The filtered time follows the predicted cost of the new size (cost ~ scale^2). */
    const float r = s / d->scale;
    if (s < d->scale && d->since_up < DYNRES_BACKOFF_FRAMES) {   /* a rise undone at once: wait longer next time */
        d->up_frames = d->up_frames * 2 > DYNRES_BACKOFF_MAX * DYNRES_UP_FRAMES ? DYNRES_BACKOFF_MAX * DYNRES_UP_FRAMES : d->up_frames * 2;
        d->calm = 0;
    }
    if (s > d->scale) d->since_up = 0;
    d->ema_ms *= r * r;
    d->scale = s;
    d->settle = DYNRES_SETTLE;
    d->above = d->below = 0;
    d->changes++;
    return 1;
}

int dynres_update(Dynres *d, float gpu_ms) {
    if (!(gpu_ms > 0) || !isfinite(gpu_ms)) return 0;
    if (d->since_up < (1 << 20)) d->since_up++;
    if (++d->calm >= DYNRES_CALM_FRAMES) {
        d->up_frames = DYNRES_UP_FRAMES;
        d->calm = 0;
    }
    if (d->settle > 0) {   /* the timestamps still describe the size before the change */
        d->settle--;
        return 0;
    }
    /* Fast towards a heavier load, slow towards a lighter one; a single spike moves it half way. */
    if (d->ema_ms < 0) d->ema_ms = gpu_ms;
    else d->ema_ms += (gpu_ms > d->ema_ms ? 0.35f : 0.1f) * (gpu_ms - d->ema_ms);
    const float t = d->target_ms;
    if (d->ema_ms > t && gpu_ms > t) {
        d->below = 0;
        if (++d->above < 2) return 0;   /* two frames in a row over the target */
        float s = d->scale * sqrtf(DYNRES_AIM * t / d->ema_ms);
        if (d->ema_ms < DYNRES_BIG_OVER * t && s < DYNRES_DROP_STEP * d->scale) s = DYNRES_DROP_STEP * d->scale;
        return set_scale(d, s);
    }
    d->above = 0;
    if (d->ema_ms < DYNRES_UP_FRACTION * t && d->scale < d->scale_max) {
        if (++d->below < d->up_frames) return 0;
        float s = d->scale * sqrtf(DYNRES_AIM * t / d->ema_ms);
        if (s > d->scale + DYNRES_UP_STEP) s = d->scale + DYNRES_UP_STEP;
        return set_scale(d, s);
    }
    d->below = 0;
    return 0;
}

void dynres_size(float scale, int out_w, int out_h, int max_w, int max_h, int *rw, int *rh) {
    int w = (int)floor(out_w * (double)scale + 0.5), h = (int)floor(out_h * (double)scale + 0.5);
    if (w > max_w) w = max_w;
    if (h > max_h) h = max_h;
    *rw = w < 1 ? 1 : w;
    *rh = h < 1 ? 1 : h;
}
