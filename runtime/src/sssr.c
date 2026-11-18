/* --reflections options (sssr.h). */
#include "sssr.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int sssr_arg(SssrConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--reflections")) {
        if (!val) return -1;
        if (!strcmp(val, "classic")) c->on = 0;
        else if (!strcmp(val, "sssr")) c->on = 1;
        else return -1;
        return 2;
    }
    if (!strcmp(opt, "--reflections-quality")) {
        if (!val) return -1;
        if (!strcmp(val, "low")) c->quality = SSSR_QUALITY_LOW;
        else if (!strcmp(val, "medium")) c->quality = SSSR_QUALITY_MEDIUM;
        else if (!strcmp(val, "high")) c->quality = SSSR_QUALITY_HIGH;
        else return -1;
        return 2;
    }
    if (!strcmp(opt, "--reflections-glass")) {
        if (!val || !*val) return -1;
        char *end;
        const float v = strtof(val, &end);
        if (*end || !(v >= 0 && v <= SSSR_GLASS_MAX)) return -1;
        c->glass = v;
        return 2;
    }
    if (!strcmp(opt, "--reflections-res")) {
        if (!val) return -1;
        if (!strcmp(val, "half")) c->full = 0;
        else if (!strcmp(val, "full")) c->full = 1;
        else return -1;
        return 2;
    }
    return 0;
}

int sssr_renderer_steps(const SssrConfig *c) { return c->on ? sssr_max_steps(c->quality) : 0; }

int sssr_max_steps(int quality) {
    switch (quality) {
    case SSSR_QUALITY_LOW: return 32;
    case SSSR_QUALITY_HIGH: return 80;
    default: return 48;
    }
}

int sssr_rays(int quality) { return quality == SSSR_QUALITY_HIGH ? 2 : 1; }

int sssr_renderer_glass(const SssrConfig *c) { return c->on && c->glass > 0; }

int sssr_glass_frame(const SssrConfig *c, const float view[16], const float proj[16], const float ab[2], RRainWet *out) {
    /* A perspective projection (as damp_wet_build). */
    if (!sssr_renderer_glass(c) || !(proj[15] == 0 && fabsf(proj[11]) == 1) || !(proj[0] > 0) || !(proj[5] > 0)) return 0;
    RRainWet p;
    memset(&p, 0, sizeof p);
    p.depth_a = ab ? ab[0] : 1.0002f;   /* rain.h RAIN_DEPTH_A/B_DEFAULT */
    p.depth_b = ab ? ab[1] : 0.0102f;
    p.px = proj[0];
    p.py = proj[5];
    const float back = -proj[11];   /* world z in view space and the camera in the world (as damp_wet_build) */
    p.up_view[0] = view[8];
    p.up_view[1] = view[9];
    p.up_view[2] = back * view[10];
    for (int j = 0; j < 3; j++) {
        p.right[j] = view[4 * j];
        p.up_v[j] = view[4 * j + 1];
        p.back[j] = back * view[4 * j + 2];
        p.eye[j] = -(view[12] * view[4 * j] + view[13] * view[4 * j + 1] + view[14] * view[4 * j + 2]);
    }
    p.reach = SSSR_GLASS_REACH;
    p.glass = c->glass;
    p.dry = 1;
    *out = p;
    return 1;
}

void sssr_glass_apply(const SssrConfig *c, RRainWet *w) { w->glass = sssr_renderer_glass(c) ? c->glass : 0; }
