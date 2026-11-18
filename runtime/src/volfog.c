/* --volfog options and the helpers shared with the renderer's shaders (volfog.h). */
#include "volfog.h"
#include "geom.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int volfog_arg(VolfogConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--volfog") || !strcmp(opt, "--no-volfog")) {
        c->on = opt[2] == 'v';
        return 1;
    }
    if (!strcmp(opt, "--volfog-quality")) {
        if (!val) return -1;
        if (!strcmp(val, "low")) c->quality = VOLFOG_LOW;
        else if (!strcmp(val, "medium")) c->quality = VOLFOG_MEDIUM;
        else if (!strcmp(val, "high")) c->quality = VOLFOG_HIGH;
        else return -1;
        return 2;
    }
    if (!strcmp(opt, "--volfog-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= VOLFOG_STRENGTH_MIN && f <= VOLFOG_STRENGTH_MAX)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    return 0;
}

const char *volfog_quality_name(int quality) { return quality == VOLFOG_LOW ? "low" : quality == VOLFOG_HIGH ? "high" : "medium"; }

VolfogGrid volfog_grid(int quality) {
    switch (quality) {
    case VOLFOG_LOW: return (VolfogGrid){96, 54, 32, 12};
    case VOLFOG_HIGH: return (VolfogGrid){160, 90, 64, VOLFOG_LIGHTS_MAX};
    default: return (VolfogGrid){128, 72, 48, 24};
    }
}

float volfog_far(int fog_on, uint32_t mode, float start, float end, float density) {
    (void)start;
    float d = VOLFOG_FAR_MAX / VOLFOG_FAR_FOG;
    if (fog_on) {
        const float k = -logf(0.05f);   /* where the fog factor is 5 % */
        if (mode == GEOM_FOG_LINEAR && end > 0) d = end;
        else if (mode == GEOM_FOG_EXP && density > 0) d = k / density;
        else if (mode == GEOM_FOG_EXP2 && density > 0) d = sqrtf(k) / density;
    }
    const float f = VOLFOG_FAR_FOG * d;
    return f > VOLFOG_FAR_MAX ? VOLFOG_FAR_MAX : f < VOLFOG_FAR_MIN ? VOLFOG_FAR_MIN : f;
}

float volfog_slice_depth(float s, int d, float near_z, float far_z) { return near_z * powf(far_z / near_z, s / (float)d); }

float volfog_depth_slice(float z, int d, float near_z, float far_z) {
    if (!(z > near_z)) return 0;
    const float s = (float)d * logf(z / near_z) / logf(far_z / near_z);
    return s > (float)d ? (float)d : s;
}

float volfog_fog_keep(int fog_on, uint32_t mode, float start, float end, float density, float z) {
    return fog_on ? sqrtf(geom_fog_factor(mode, start, end, density, z)) : 1.0f;
}

float volfog_density(float strength, float rain, float damp, int indoor) {
    return VOLFOG_DENSITY * strength * (1 + VOLFOG_RAIN_GAIN * rain) * (1 + VOLFOG_DAMP_GAIN * damp) * (indoor ? VOLFOG_INDOOR : 1.0f);
}

uint32_t volfog_select(const VolfogLight *in, uint32_t n, uint32_t cap, float far_z, VolfogLight *out) {
    static const int order[3] = {VOLFOG_FIRE, VOLFOG_LAMP, VOLFOG_NEON};
    if (cap > VOLFOG_LIGHTS_MAX) cap = VOLFOG_LIGHTS_MAX;
    uint32_t k = 0;
    for (int o = 0; o < 3; o++)
        for (uint32_t i = 0; i < n && k < cap; i++) {
            const VolfogLight *l = &in[i];
            if (l->kind != order[o]) continue;
            if (!(l->intensity > 0) || !(l->radius > 0) || !(l->rgb[0] > 0 || l->rgb[1] > 0 || l->rgb[2] > 0)) continue;
            float r = l->kind == VOLFOG_LAMP ? l->radius * VOLFOG_LAMP_REACH : l->radius;
            if (l->kind == VOLFOG_NEON) r = r < VOLFOG_NEON_RADIUS_MIN ? VOLFOG_NEON_RADIUS_MIN : r > VOLFOG_NEON_RADIUS_MAX ? VOLFOG_NEON_RADIUS_MAX : r;
            else if (r < VOLFOG_REACH_MIN) r = VOLFOG_REACH_MIN;
            if (r > VOLFOG_RADIUS_MAX) r = VOLFOG_RADIUS_MAX;
            const float depth = -l->pos[2];   /* the camera looks down -z */
            if (depth + r < 0 || depth - r > far_z) continue;
            out[k] = *l;
            out[k++].radius = r;
        }
    return k;
}

float volfog_phase_g(int kind) { return kind == VOLFOG_FIRE ? 0.15f : kind == VOLFOG_NEON ? 0.3f : 0.45f; }

float volfog_falloff(float r, float radius) {
    if (!(radius > 0) || r >= radius) return 0;
    if (r < VOLFOG_R_MIN) r = VOLFOG_R_MIN;
    const float x = r / radius, x2 = x * x;
    if (x2 >= 1) return 0;
    const float w = 1 - x2;
    const float core = VOLFOG_CORE > VOLFOG_CORE_SHARE * radius ? VOLFOG_CORE : VOLFOG_CORE_SHARE * radius;
    return w * w / (1 + r * r / (core * core));
}

/* The 4x4 inverse of an affine matrix in rows (v = w x V; last column 0, 0, 0, 1). 0 when it cannot
 * be inverted. */
static int affine_inverse(const float V[16], float I[16]) {
    const float a = V[0], b = V[1], c = V[2], d = V[4], e = V[5], f = V[6], g = V[8], h = V[9], k = V[10];
    const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (!(fabsf(det) > 1e-12f)) return 0;
    const float id = 1 / det;
    float R[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id, (f * g - d * k) * id, (a * k - c * g) * id,
                  (c * d - a * f) * id, (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};
    memset(I, 0, 16 * sizeof(float));
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) I[4 * i + j] = R[3 * i + j];
    for (int j = 0; j < 3; j++) I[12 + j] = -(V[12] * R[j] + V[13] * R[3 + j] + V[14] * R[6 + j]);
    I[15] = 1;
    return 1;
}

int volfog_reproject(const float prev_view[16], const float cur_view[16], float out[3][4]) {
    float I[16];
    if (!affine_inverse(cur_view, I)) return 1;
    /* prev = (cur x I) x P as row vectors: M = I x P; out holds its transpose (prev = out x (cur, 1)). */
    float M[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += I[4 * i + k] * prev_view[4 * k + j];
            M[4 * i + j] = s;
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) out[i][j] = M[4 * j + i];
    const float move = sqrtf(M[12] * M[12] + M[13] * M[13] + M[14] * M[14]);
    const float cosang = (M[0] + M[5] + M[10] - 1) * 0.5f;   /* the rotation's angle from its trace */
    const float turn = acosf(cosang > 1 ? 1 : cosang < -1 ? -1 : cosang) * 57.29578f;
    return move > VOLFOG_CUT_MOVE || turn > VOLFOG_CUT_TURN || move != move || cosang != cosang;
}

static float halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    while (i) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}

void volfog_jitter(uint32_t frame, float out[3]) {
    const uint32_t i = frame % 16 + 1;
    out[0] = halton(i, 2);
    out[1] = halton(i, 3);
    out[2] = halton(i, 5);
}

static float smooth01(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

static float lamp_glow(float c) { return VOLFOG_GLOW * (VOLFOG_GLOW_ABOVE + (1 - VOLFOG_GLOW_ABOVE) * smooth01(-0.2f, 0.3f, c)); }
static float lamp_cone(float c) { return VOLFOG_CONE_GAIN * smooth01(VOLFOG_CONE_COS_OUTER, VOLFOG_CONE_COS_INNER, c); }

float volfog_lamp_shape(float cos_down) { return lamp_glow(cos_down) + lamp_cone(cos_down); }

float volfog_lamp_light(float cos_down, float r, float radius) {
    if (!(radius > 0) || r >= radius) return 0;
    if (r < VOLFOG_R_MIN) r = VOLFOG_R_MIN;
    const float x2 = r * r / (radius * radius);
    if (x2 >= 1) return 0;
    return lamp_glow(cos_down) * volfog_falloff(r, radius) + lamp_cone(cos_down) * (1 - x2) * (1 - x2);
}

float volfog_mist(float h) {
    if (!(h == h)) return 0;
    if (h < -VOLFOG_MIST_LOW) h = -VOLFOG_MIST_LOW;
    return expf(-h / VOLFOG_MIST_HEIGHT);
}

float volfog_mist_density(float strength, float wet) {
    wet = wet < 0 ? 0 : wet > 1 ? 1 : wet;
    return VOLFOG_MIST * strength * (1 + VOLFOG_MIST_WET * wet);
}

float volfog_ground_step(float prev, int have_prev, float sample, int valid, float eye_z) {
    const float target = valid ? sample : eye_z - VOLFOG_CAM_HEIGHT;
    if (!have_prev || !(fabsf(target - prev) <= VOLFOG_GROUND_SNAP)) return target;
    return prev + (target - prev) * VOLFOG_GROUND_EASE;
}

uint32_t volfog_dedupe(const VolfogLight *halo, uint32_t nh, VolfogLight *level, uint32_t nl, const float up[3], VolfogLight *out, uint32_t cap) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < nh; i++) {
        int pair = 0;
        for (uint32_t j = 0; j < nl && !pair; j++) {
            float d[3];
            for (int c = 0; c < 3; c++) d[c] = halo[i].pos[c] - level[j].pos[c];
            const float h = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];   /* the halo's height over the level light */
            const float a2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - h * h;
            if (a2 < VOLFOG_DEDUPE * VOLFOG_DEDUPE && h >= -VOLFOG_PAIR_ABOVE && h <= VOLFOG_PAIR_BELOW) {
                memcpy(level[j].pos, halo[i].pos, sizeof level[j].pos);
                pair = 1;
            }
        }
        if (!pair && k < cap) out[k++] = halo[i];
    }
    return k;
}

float volfog_lamp_reach(float reach, float height) {
    const float g = VOLFOG_CONE_GROUND * height;
    if (reach < g) reach = g;
    return reach > VOLFOG_RADIUS_MAX ? VOLFOG_RADIUS_MAX : reach;
}

float volfog_rain_scatter(float rain) { return 1 + VOLFOG_RAIN_LIT * (rain < 0 ? 0 : rain > 1 ? 1 : rain); }

float volfog_phase_g_rain(int kind, float rain) {
    const float g = volfog_phase_g(kind);
    if (kind != VOLFOG_LAMP) return g;
    rain = rain < 0 ? 0 : rain > 1 ? 1 : rain;
    return g + (VOLFOG_RAIN_G - g) * rain;
}
