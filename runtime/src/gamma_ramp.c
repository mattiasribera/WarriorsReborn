/* --gamma-ramp: the title's gamma ramps (see gamma_ramp.h). */
#include "gamma_ramp.h"
#include <string.h>

int gamma_ramp_enabled = 1;

int gamma_ramp_arg(int *on, const char *opt) {
    if (!strcmp(opt, "--gamma-ramp")) *on = 1;
    else if (!strcmp(opt, "--no-gamma-ramp")) *on = 0;
    else return 0;
    return 1;
}

int gamma_ramp_parse(const void *src, size_t size, GammaRamp *out) {
    if (!src || size < GAMMA_RAMP_BYTES) return -1;
    memcpy(out, src, GAMMA_RAMP_BYTES);
    return 0;
}

int gamma_ramp_is_identity(const GammaRamp *g) {
    for (int i = 0; i < 256; i++)
        if (g->red[i] != i || g->green[i] != i || g->blue[i] != i) return 0;
    return 1;
}

void gamma_ramp_lut(const GammaRamp *g, uint32_t lut[256]) {
    for (int i = 0; i < 256; i++) lut[i] = (uint32_t)g->red[i] | (uint32_t)g->green[i] << 8 | (uint32_t)g->blue[i] << 16;
}

uint32_t gamma_ramp_apply(const uint32_t lut[256], uint32_t bgra) {
    const uint32_t r = lut[bgra >> 16 & 0xff] & 0xff, g = lut[bgra >> 8 & 0xff] >> 8 & 0xff, b = lut[bgra & 0xff] >> 16 & 0xff;
    return (bgra & 0xff000000u) | r << 16 | g << 8 | b;
}
