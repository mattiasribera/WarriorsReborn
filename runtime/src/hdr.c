/* HDR10 output and saturation: options, the CPU reference of shaders/output.frag, the surface format
 * choice (hdr.h). */
#include "hdr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_float(const char *v, float lo, float hi, float *out) {
    char *end;
    if (!v || !*v) return -1;
    float f = (float)strtod(v, &end);
    if (*end || !(f >= lo && f <= hi)) return -1;   /* NaN fails the range check */
    *out = f;
    return 0;
}

int hdr_arg(HdrConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--hdr")) {
        if (!val) return -1;
        if (!strcmp(val, "off")) c->mode = HDR_OFF;
        else if (!strcmp(val, "auto")) c->mode = HDR_AUTO;
        else if (!strcmp(val, "on")) c->mode = HDR_ON;
        else return -1;
        return 2;
    }
    if (!strcmp(opt, "--hdr-peak")) return parse_float(val, HDR_PEAK_MIN, HDR_PEAK_MAX, &c->peak) ? -1 : 2;
    if (!strcmp(opt, "--hdr-paper-white")) return parse_float(val, HDR_PAPER_MIN, HDR_PAPER_MAX, &c->paper) ? -1 : 2;
    if (!strcmp(opt, "--saturation")) return parse_float(val, HDR_SAT_MIN, HDR_SAT_MAX, &c->saturation) ? -1 : 2;
    return 0;
}

/* SMPTE ST 2084 constants. */
#define PQ_M1 0.1593017578125f
#define PQ_M2 78.84375f
#define PQ_C1 0.8359375f
#define PQ_C2 18.8515625f
#define PQ_C3 18.6875f

float hdr_pq_encode(float nits) {
    const float y = nits <= 0 ? 0 : nits >= 10000 ? 1 : nits / 10000;
    const float p = powf(y, PQ_M1);
    return powf((PQ_C1 + PQ_C2 * p) / (1 + PQ_C3 * p), PQ_M2);
}

float hdr_pq_decode(float v) {
    const float e = powf(v <= 0 ? 0 : v >= 1 ? 1 : v, 1 / PQ_M2);
    const float num = e - PQ_C1 > 0 ? e - PQ_C1 : 0;
    return 10000 * powf(num / (PQ_C2 - PQ_C3 * e), 1 / PQ_M1);
}

void hdr_709_to_2020(const float in[3], float out[3]) {
    static const float m[3][3] = {{0.6274040f, 0.3292820f, 0.0433136f}, {0.0690970f, 0.9195400f, 0.0113612f}, {0.0163916f, 0.0880132f, 0.8955950f}};
    float o[3];
    for (int i = 0; i < 3; i++) o[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
    memcpy(out, o, sizeof o);
}

float hdr_luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

void hdr_saturate(const float lin[3], float sat, float out[3]) {
    if (sat == 1.0f) {
        memmove(out, lin, 3 * sizeof *out);
        return;
    }
    const float y = hdr_luma(lin);
    for (int i = 0; i < 3; i++) {
        const float v = y + sat * (lin[i] - y);
        out[i] = v > 0 ? v : 0;
    }
}

float hdr_expand(float y, float ratio, float w) {
    if (!(y > HDR_KNEE) || !(ratio > 1) || !(w > 0)) return y;
    float t = (y - HDR_KNEE) / (1 - HDR_KNEE);
    if (t > 1) t = 1;
    return y + w * (ratio - 1) * t * t * t;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

float hdr_pixel(const HdrLevels *l, const float display[3], float scene, float light, float pq[3]) {
    float lin[3];
    for (int i = 0; i < 3; i++) lin[i] = powf(clamp01(display[i]), 2.2f);
    hdr_saturate(lin, l->saturation, lin);
    const float y = hdr_luma(lin), ratio = l->peak > l->paper ? l->peak / l->paper : 1;
    const float w = clamp01(scene) * (HDR_SCENE_WEIGHT + (1 - HDR_SCENE_WEIGHT) * clamp01(light));
    const float g = y > 1e-6f ? hdr_expand(y, ratio, w) / y : 1;
    float nits[3], top = 0;
    for (int i = 0; i < 3; i++) {
        nits[i] = lin[i] * g * l->paper;
        if (nits[i] > top) top = nits[i];
    }
    if (top > l->peak) {   /* a saturated colour's brightest channel: down to the peak, hue kept */
        for (int i = 0; i < 3; i++) nits[i] *= l->peak / top;
        top = l->peak;
    }
    hdr_709_to_2020(nits, nits);
    for (int i = 0; i < 3; i++) pq[i] = hdr_pq_encode(nits[i] > 0 ? nits[i] : 0);
    return top;
}

void hdr_sdr_grade(float saturation, const float display[3], float out[3]) {
    float lin[3];
    for (int i = 0; i < 3; i++) lin[i] = powf(clamp01(display[i]), 2.2f);
    hdr_saturate(lin, saturation, lin);
    for (int i = 0; i < 3; i++) out[i] = powf(clamp01(lin[i]), 1 / 2.2f);
}
