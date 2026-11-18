/* --auto-exposure options and the adaptation / roll-off math (auto_exposure.h). Keep
 * auto_exposure_step in step with shaders/ae_adapt.frag and auto_exposure_rolloff with
 * shaders/ae_common.glsl. */
#include "auto_exposure.h"
#include "filmic.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int auto_exposure_arg(AutoExposureConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--auto-exposure") || !strcmp(opt, "--no-auto-exposure")) {
        c->on = opt[2] == 'a';
        return 1;
    }
    if (!strcmp(opt, "--auto-exposure-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= 0 && f <= 2)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    return 0;
}

/* Calibration: the key is the meter's value on the title's lit
 * night streets (headless --stats sweeps: level51's rainy yard about -8.9, level82's warehouse
 * street -7.4 to -9.7; light mask excluded), so those keep Rockstar's grade; per unit of strength
 * 0.4 of the deviation is corrected and the gain stays within -0.4 to +0.5 stop, so a dark corner
 * (about -11.7) gets at most half a stop and stays dark. */
#define AE_KEY (-8.8f)
#define AE_ADAPT 0.4f
#define AE_EV_MIN (-0.4f)
#define AE_EV_MAX 0.5f

void auto_exposure_params(float strength, RAutoExposure *p) {
    const float s = strength < 0 ? 0 : strength > 2 ? 2 : strength;
    memset(p, 0, sizeof *p);
    p->key = AE_KEY;
    p->adapt = AE_ADAPT * s;
    p->ev_min = AE_EV_MIN * s;
    p->ev_max = AE_EV_MAX * s;
    p->tau_up = 0.6f;     /* dark to bright: the over-exposure shows, then settles */
    p->tau_down = 2.5f;   /* bright to dark: the eyes get used to the dark slowly */
    p->rate_up = 1.5f;    /* a 125 ms lightning flash moves the level at most 0.19 stop */
    p->knee = 0.5f;
    p->dither = 1;
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

void auto_exposure_step(const RAutoExposure *p, AutoExposureState *s, float meter, int meter_valid, float dt, int snap) {
    const float m = meter_valid ? clampf(meter, AUTO_EXPOSURE_LOG_MIN, AUTO_EXPOSURE_LOG_MAX) : s->adapted;
    if (snap) {
        s->adapted = m;
    } else if (dt > 0) {
        const float d = m - s->adapted;
        float step = d * (1 - expf(-dt / (d > 0 ? p->tau_up : p->tau_down)));
        if (step > p->rate_up * dt) step = p->rate_up * dt;
        s->adapted += step;
    }
    /* No wind-up: the level stays where the gain still moves, so after a dark (or bright) moment the
     * gain starts back at once instead of after the level crossed the dead range. */
    if (p->adapt > 0) s->adapted = clampf(s->adapted, p->key - p->ev_max / p->adapt, p->key - p->ev_min / p->adapt);
    s->cut = snap ? 1.0f : 0.0f;
    s->meter = m;
    s->ev = clampf(p->adapt * (p->key - s->adapted), p->ev_min, p->ev_max);
}

float auto_exposure_rolloff(float t, float tmax, float knee) {
    if (tmax <= 1 || t <= knee) return t;
    const float T = (tmax - knee) / (1 - knee), a = (T - 1) / T, u = (t - knee) / (1 - knee);
    return knee + (1 - knee) * u / (1 + a * u);
}

float auto_exposure_tmax(const RFilmic *f, float gain) {
    if (!f) return gain;
    return f->blend * filmic_aces(f->exposure * f->white * gain) * f->norm + (1 - f->blend) * gain;
}
