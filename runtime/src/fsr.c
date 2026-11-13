/* --fsr: options, sizes, conflict rules and shader constants (see fsr.h). The constant setup
 * follows FsrEasuCon and FsrRcasCon of AMD FidelityFX FSR 1 (ffx_fsr1.h, v1.20210629, MIT
 * license; see Third-party software in runtime/README.md and the notice in
 * shaders/fsr_easu.frag). */
#include "fsr.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

FsrOptions fsr_options = {FSR_OFF, FSR_SHARPNESS_DEFAULT};

/* Scale per axis as num / den: exact integer rounding of the render size. */
static const struct {
    const char *name;
    int num, den;
} modes[] = {
    [FSR_OFF] = {"off", 1, 1},
    [FSR_ULTRA] = {"ultra", 13, 10},
    [FSR_QUALITY] = {"quality", 3, 2},
    [FSR_BALANCED] = {"balanced", 17, 10},
    [FSR_PERFORMANCE] = {"performance", 2, 1},
};
enum { NMODES = sizeof modes / sizeof modes[0] };

static int known(FsrMode m) { return (unsigned)m < NMODES; }

int fsr_parse_mode(const char *s, FsrMode *out) {
    for (int k = 0; k < NMODES; k++)
        if (!strcmp(s, modes[k].name)) {
            *out = (FsrMode)k;
            return 0;
        }
    return -1;
}

int fsr_parse_sharpness(const char *s, float *out) {
    char *end;
    float v = strtof(s, &end);
    if (end == s || *end || !(v >= 0.0f && v <= FSR_SHARPNESS_MAX)) return -1;   /* also rejects nan */
    *out = v;
    return 0;
}

const char *fsr_mode_name(FsrMode m) { return known(m) ? modes[m].name : "off"; }

float fsr_mode_scale(FsrMode m) { return known(m) ? (float)modes[m].num / (float)modes[m].den : 1.0f; }

/* round(v * den / num), halves up, at least 1. */
static int scaled(int v, int num, int den) {
    long long r = ((long long)v * den * 2 + num) / (2LL * num);
    return r < 1 ? 1 : (int)r;
}

void fsr_render_size(FsrMode m, int out_w, int out_h, int *rw, int *rh) {
    if (!known(m) || m == FSR_OFF) {
        *rw = out_w;
        *rh = out_h;
        return;
    }
    *rw = scaled(out_w, modes[m].num, modes[m].den);
    *rh = scaled(out_h, modes[m].num, modes[m].den);
}

int fsr_resolve(FsrMode m, int *supersample, int *sharpen) {
    if (!known(m) || m == FSR_OFF) return 0;
    int dropped = 0;
    if (*supersample > 1) dropped |= FSR_DROPPED_SUPERSAMPLE;
    if (*sharpen > 0) dropped |= FSR_DROPPED_SHARPEN;
    if (dropped & FSR_DROPPED_SUPERSAMPLE) *supersample = 0;
    if (dropped & FSR_DROPPED_SHARPEN) *sharpen = 0;
    return dropped;
}

static uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

void fsr_easu_con(uint32_t con[16], int in_w, int in_h, int out_w, int out_h) {
    const float iw = (float)in_w, ih = (float)in_h, ow = (float)out_w, oh = (float)out_h;
    const float rx = 1.0f / iw, ry = 1.0f / ih;
    /* Output integer position to a pixel position in the input. */
    con[0] = bits(iw * (1.0f / ow));
    con[1] = bits(ih * (1.0f / oh));
    con[2] = bits(0.5f * iw * (1.0f / ow) - 0.5f);
    con[3] = bits(0.5f * ih * (1.0f / oh) - 0.5f);
    /* Input pixel position to normalized image space (upper left of the 'F' tap), then the
     * centers of the four gathers. */
    con[4] = bits(rx);
    con[5] = bits(ry);
    con[6] = bits(1.0f * rx);
    con[7] = bits(-1.0f * ry);
    con[8] = bits(-1.0f * rx);
    con[9] = bits(2.0f * ry);
    con[10] = bits(1.0f * rx);
    con[11] = bits(2.0f * ry);
    con[12] = bits(0.0f * rx);
    con[13] = bits(4.0f * ry);
    con[14] = con[15] = 0;
}

/* IEEE half of a float in the normal half range (round to nearest even); 0 below it. */
static uint16_t half_of(float f) {
    uint32_t u = bits(f);
    uint32_t sign = (u >> 16) & 0x8000u;
    int e = (int)((u >> 23) & 0xFF) - 127 + 15;
    uint32_t m = u & 0x7FFFFFu;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    uint32_t h = sign | (uint32_t)e << 10 | m >> 13;
    uint32_t rest = m & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1))) h++;
    return (uint16_t)h;
}

void fsr_rcas_con(uint32_t con[4], float sharpness) {
    if (!(sharpness >= 0.0f)) sharpness = 0.0f;
    if (sharpness > FSR_SHARPNESS_MAX) sharpness = FSR_SHARPNESS_MAX;
    /* Stops to linear. */
    const float lin = exp2f(-sharpness);
    const uint32_t h = half_of(lin);
    con[0] = bits(lin);
    con[1] = h | h << 16;
    con[2] = 0;
    con[3] = 0;
}
