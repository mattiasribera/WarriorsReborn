/* --taa: the pure part (see taa.h). */
#include "taa.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

TaaOptions taa_options = {0, TAA_SHARPNESS_DEFAULT, TAA_UP_OFF, TAA_UPSCALER_TAA, 0.0f, 0, TAA_LITE_AUTO, 0};

float taa_auto_sharpness(float k) {
    if (!(k > 1.3f)) return TAA_SHARPNESS_DEFAULT;   /* also NaN */
    if (k >= 2.0f) return TAA_SHARPNESS_LOW_RES;
    return TAA_SHARPNESS_DEFAULT + (TAA_SHARPNESS_LOW_RES - TAA_SHARPNESS_DEFAULT) * (k - 1.3f) / 0.7f;
}

int taa_parse_sharpness(const char *s, float *out) {
    char *end = NULL;
    const double v = strtod(s, &end);
    if (end == s || *end || !(v >= 0 && v <= TAA_SHARPNESS_MAX)) return -1;
    *out = (float)v;
    return 0;
}

float taa_halton(uint32_t index, uint32_t base) {
    double f = 1, r = 0;
    while (index > 0) {
        f /= base;
        r += f * (index % base);
        index /= base;
    }
    return (float)r;
}

void taa_jitter(uint32_t n, float out[2]) { taa_jitter_n(n, TAA_JITTER_PHASES, out); }

void taa_jitter_n(uint32_t n, uint32_t phases, float out[2]) {
    const uint32_t i = n % (phases ? phases : TAA_JITTER_PHASES) + 1;
    out[0] = taa_halton(i, 2) - 0.5f;
    out[1] = taa_halton(i, 3) - 0.5f;
}

/* Indexed by TaaUpscale; TAA_UP_CUSTOM's ratio is the option's. */
static const char *const up_names[] = {"off", "quality", "balanced", "performance", "ultra", "custom", "native"};
static const float up_scales[] = {1.0f, 1.5f, 1.7f, 2.0f, 1.3f, 0.0f, 1.0f};

int taa_parse_upscale(const char *s, TaaUpscale *out, float *ratio) {
    for (int k = 0; k <= TAA_UP_NATIVE; k++)
        if (k != TAA_UP_CUSTOM && !strcmp(s, up_names[k])) {
            *out = (TaaUpscale)k;
            *ratio = 0.0f;
            return 0;
        }
    char *end = NULL;
    const double v = strtod(s, &end);
    if (end == s || *end || !(v >= TAA_UP_SCALE_MIN - 1e-6 && v <= TAA_UP_SCALE_MAX + 1e-6)) return -1;
    *out = TAA_UP_CUSTOM;
    *ratio = (float)v;
    return 0;
}

int taa_parse_upscaler(const char *s, TaaUpscaler *out) {
    if (!strcmp(s, "taa")) *out = TAA_UPSCALER_TAA;
    else if (!strcmp(s, "fsr3")) *out = TAA_UPSCALER_FSR3;
    else return -1;
    return 0;
}

const char *taa_upscaler_name(TaaUpscaler u) { return u == TAA_UPSCALER_FSR3 ? "fsr3" : "taa"; }

int taa_parse_lite(const char *s, TaaLite *out) {
    if (!strcmp(s, "auto")) *out = TAA_LITE_AUTO;
    else if (!strcmp(s, "on")) *out = TAA_LITE_ON;
    else if (!strcmp(s, "off")) *out = TAA_LITE_OFF;
    else return -1;
    return 0;
}

const char *taa_lite_name(TaaLite m) { return m == TAA_LITE_ON ? "on" : m == TAA_LITE_OFF ? "off" : "auto"; }

int taa_lite_wanted(TaaLite m, int integrated_gpu, int samples) {
    if (samples > 1 || m == TAA_LITE_OFF) return 0;
    return m == TAA_LITE_ON || integrated_gpu;
}

const char *taa_upscale_name(TaaUpscale m) { return (unsigned)m <= TAA_UP_NATIVE ? up_names[m] : "off"; }

float taa_upscale_scale(TaaUpscale m) { return (unsigned)m <= TAA_UP_NATIVE ? up_scales[m] : 1.0f; }

void taa_upscale_size_k(float k, int out_w, int out_h, int *rw, int *rh) {
    if (!(k > 1.0f)) k = 1.0f;
    const int w = (int)floor(out_w / (double)k + 0.5), h = (int)floor(out_h / (double)k + 0.5);
    *rw = w < 1 ? 1 : w;
    *rh = h < 1 ? 1 : h;
}

void taa_upscale_size(TaaUpscale m, int out_w, int out_h, int *rw, int *rh) { taa_upscale_size_k(taa_upscale_scale(m), out_w, out_h, rw, rh); }

uint32_t taa_jitter_phases(int rw, int dw) {
    if (rw <= 0 || rw >= dw) return TAA_JITTER_PHASES;
    const double r = (double)dw / rw, n = ceil(TAA_JITTER_PHASES * r * r - 1e-9);
    return n > TAA_JITTER_PHASES ? (uint32_t)n : TAA_JITTER_PHASES;
}

float taa_lod_bias(int rw, int dw) { return rw <= 0 || rw >= dw ? 0.0f : (float)log2((double)rw / dw); }

/* 4 x 4 inverse by cofactors (double); returns 0 when singular. */
static int inverse4(const double m[16], double inv[16]) {
    double t[16];
    t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    double scale = 0;
    for (int i = 0; i < 16; i++) scale = fmax(scale, fabs(m[i]));
    if (!(fabs(det) > 1e-12 * scale * scale * scale * scale) || !isfinite(det)) return 0;
    for (int i = 0; i < 16; i++) inv[i] = t[i] / det;
    return 1;
}

int taa_reprojection(const float cur[4][4], const float prev[4][4], float out[3][4]) {
    static const int rows[3] = {0, 1, 3};   /* clip x, y, w */
    double a[16], inv[16];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) a[i * 4 + j] = cur[rows[i]][j];
    a[12] = a[13] = a[14] = 0;
    a[15] = 1;
    if (!inverse4(a, inv)) return -1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += (double)prev[rows[i]][k] * inv[k * 4 + j];
            out[i][j] = (float)v;
        }
    return 0;
}

/* Previous NDC of the homogeneous vector v through m; -1 behind the camera. */
static int map_point(const float m[3][4], const double v[4], double out[2]) {
    double c[3];
    for (int i = 0; i < 3; i++) c[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] * v[3];
    if (!(c[2] > 0)) return -1;
    out[0] = c[0] / c[2];
    out[1] = c[1] / c[2];
    return 0;
}

float taa_camera_motion(const float m[3][4], float dist) {
    static const double g[3] = {-0.8, 0, 0.8};
    double worst = 0;
    for (int iy = 0; iy < 3; iy++)
        for (int ix = 0; ix < 3; ix++)
            for (int far = 0; far < 2; far++) {
                const double x = g[ix], y = g[iy], w = far ? 1 : dist;
                const double v[4] = {x * w, y * w, w, far ? 0 : 1};
                double p[2];
                if (map_point(m, v, p)) return 1e9f;
                worst = fmax(worst, fmax(fabs(p[0] - x), fabs(p[1] - y)));
            }
    return (float)worst;
}

int taa_camera_cut(const float m[3][4]) { return taa_camera_motion(m, TAA_CUT_DIST) > TAA_CUT_NDC; }

int taa_reproject_point(const float m[3][4], float a, float b, float x, float y, float d, float out[2]) {
    double v[4] = {x, y, 1, 0};   /* at infinity: depth 1 or beyond the projection's range */
    if (d < 1 && a - d > 0 && b > 0) {
        const double w = b / (a - d);
        v[0] = x * w;
        v[1] = y * w;
        v[2] = w;
        v[3] = 1;
    }
    double p[2];
    if (map_point(m, v, p)) return -1;
    out[0] = (float)p[0];
    out[1] = (float)p[1];
    return 0;
}
