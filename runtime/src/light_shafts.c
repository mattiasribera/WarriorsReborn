/* --light-shafts options, rain easing, density and cone shape (light_shafts.h). */
#include "light_shafts.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int light_shafts_arg(LightShaftsConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--light-shafts") || !strcmp(opt, "--no-light-shafts")) {
        c->on = opt[2] == 'l';
        return 1;
    }
    if (!strcmp(opt, "--light-shafts-cheap") || !strcmp(opt, "--no-light-shafts-cheap")) {
        c->cheap = opt[2] == 'l';
        return 1;
    }
    if (!strcmp(opt, "--light-shafts-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= LIGHT_SHAFTS_STRENGTH_MIN && f <= LIGHT_SHAFTS_STRENGTH_MAX)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    return 0;
}

float light_shafts_rain_step(float level, int raining) {
    level += raining ? LIGHT_SHAFTS_RAIN_IN : -LIGHT_SHAFTS_RAIN_OUT;
    return level < 0 ? 0 : level > 1 ? 1 : level;
}

float light_shafts_density(float strength, float rain) { return LIGHT_SHAFTS_DENSITY * strength * (1 + LIGHT_SHAFTS_RAIN_GAIN * rain); }

float light_shafts_length(float radius) {
    const float l = radius * LIGHT_SHAFTS_LEN_SCALE;
    return l < LIGHT_SHAFTS_LEN_MIN ? LIGHT_SHAFTS_LEN_MIN : l > LIGHT_SHAFTS_LEN_MAX ? LIGHT_SHAFTS_LEN_MAX : l;
}

static float smooth(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

float light_shafts_cone(const float p[3], const float apex[3], const float axis[3], float len) {
    const float v[3] = {p[0] - apex[0], p[1] - apex[1], p[2] - apex[2]};
    const float h = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
    if (!(h > 0) || h >= len) return 0;
    const float d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float edge = smooth(LIGHT_SHAFTS_COS_OUTER, LIGHT_SHAFTS_COS_INNER, h / d);
    const float x = h / len, a = 1 - x;
    return edge * a * a / (1 + LIGHT_SHAFTS_NEAR_K * x * x);
}
