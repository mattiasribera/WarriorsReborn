/* --fire-light options, flame sprites, per-fire clustering, flicker and lights (fire_light.h). */
#include "fire_light.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int fire_light_arg(FireLightConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--fire-light") || !strcmp(opt, "--no-fire-light")) {
        c->on = opt[2] == 'f';
        return 1;
    }
    if (!strcmp(opt, "--fire-light-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= FIRE_STRENGTH_MIN && f <= FIRE_STRENGTH_MAX)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    return 0;
}

int fire_sprite_quad(const float (*world)[3], const uint32_t *argb, int nv, LampSprite *out) {
    if (nv < 3 || nv > 6) return -1;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, sum[3] = {0, 0, 0}, alpha = 0;
    for (int i = 0; i < nv; i++) {
        for (int a = 0; a < 3; a++) {
            const float p = world[i][a];
            sum[a] += p;
            lo[a] = p < lo[a] ? p : lo[a];
            hi[a] = p > hi[a] ? p : hi[a];
        }
        alpha += (argb[i] >> 24) / 255.0f;
    }
    float size = 0;
    for (int a = 0; a < 3; a++) size = hi[a] - lo[a] > size ? hi[a] - lo[a] : size;
    alpha /= (float)nv;
    if (!(size >= FIRE_MIN_SIZE) || !(alpha >= FIRE_MIN_WEIGHT)) return -1;
    for (int a = 0; a < 3; a++) out->pos[a] = sum[a] / (float)nv;
    out->size = size;
    out->rgb[0] = FIRE_R;
    out->rgb[1] = FIRE_G;
    out->rgb[2] = FIRE_B;
    out->weight = 1;   /* the flame animation fades its quads in and out: the fire itself does not dim */
    return 0;
}

uint32_t fire_cluster(LampSprite *s, uint32_t n) {
    /* Always the closest pair first, until none is within FIRE_MERGE: the result does not depend on
     * the order of the flames (a first-found pair would make it so). */
    while (n > 1) {
        uint32_t bi = 0, bj = 0;
        float bd = FIRE_MERGE * FIRE_MERGE;
        int found = 0;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = i + 1; j < n; j++) {
                const float x = s[i].pos[0] - s[j].pos[0], y = s[i].pos[1] - s[j].pos[1], z = s[i].pos[2] - s[j].pos[2];
                const float d = x * x + y * y + z * z;
                if (d <= bd) {
                    bd = d;
                    bi = i;
                    bj = j;
                    found = 1;
                }
            }
        if (!found) break;
        const float wi = s[bi].weight, wj = s[bj].weight, w = wi + wj;
        for (int a = 0; a < 3; a++) s[bi].pos[a] = (s[bi].pos[a] * wi + s[bj].pos[a] * wj) / w;
        s[bi].weight = w;
        s[bi].size = fmaxf(s[bi].size, s[bj].size);
        s[bj] = s[--n];
    }
    return n;
}

/* Value noise: a pseudo-random value in [0, 1] per (id, channel, integer knot), smoothly
 * interpolated (smoothstep) between knots. */
static float knot(uint32_t id, uint32_t ch, int64_t k) {
    uint32_t h = id * 0x9E3779B1u ^ ch * 0x85EBCA77u ^ (uint32_t)k * 0xC2B2AE3Du ^ (uint32_t)(k >> 32) * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return (h >> 8) / 16777215.0f;
}
static float vnoise(uint32_t id, uint32_t ch, double x) {
    const double f = floor(x);
    const float u = (float)(x - f), s = u * u * (3 - 2 * u);
    const int64_t k = (int64_t)f;
    const float a = knot(id, ch, k), b = knot(id, ch, k + 1);
    return a + (b - a) * s;
}

float fire_flicker(uint32_t id, double t) {
    /* A slow breathing, a livelier flutter and a fast lick; weights sum to 1. */
    const float n = 0.5f * vnoise(id, 0, t * 1.3) + 0.3f * vnoise(id, 1, t * 3.1) + 0.2f * vnoise(id, 2, t * 7.3);
    return 1.0f - FIRE_FLICKER_AMP * n;
}

void fire_jitter(uint32_t id, double t, float off[3]) {
    for (int a = 0; a < 3; a++) {
        const float n = 0.6f * vnoise(id, 10 + a, t * 0.9) + 0.4f * vnoise(id, 20 + a, t * 2.3);
        off[a] = FIRE_JITTER * (2 * n - 1) * (a == 2 ? 0.6f : 1.0f);
    }
}

void fire_light_shade(LampLight *l, uint32_t n, double t, float strength) {
    for (uint32_t i = 0; i < n; i++) {
        const float f = fire_flicker(l[i].id, t);
        float o[3];
        fire_jitter(l[i].id, t, o);
        for (int a = 0; a < 3; a++) l[i].pos[a] += o[a];
        l[i].rgb[0] = FIRE_R;
        l[i].rgb[1] = FIRE_G;
        l[i].rgb[2] = FIRE_B;
        l[i].radius = FIRE_RADIUS * (0.92f + 0.08f * f);
        l[i].intensity = FIRE_INTENSITY * l[i].intensity * f * strength;
    }
}

int fire_light_texture(uint64_t hash) { return hash == FIRE_TEX_FLAMES || hash == FIRE_TEX_FLAME_STRIP; }
