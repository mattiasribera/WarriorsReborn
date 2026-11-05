/* --lamp-lights options, halo sprites, clustering, tracking and falloff (lamp_lights.h). */
#include "lamp_lights.h"
#include <math.h>
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

int lamp_lights_arg(LampLightsConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--lamp-lights") || !strcmp(opt, "--no-lamp-lights")) {
        c->on = opt[2] == 'l';
        return 1;
    }
    if (!strcmp(opt, "--lamp-lights-strength")) return parse_float(val, LAMP_STRENGTH_MIN, LAMP_STRENGTH_MAX, &c->strength) ? -1 : 2;
    return 0;
}

int lamp_sprite_quad(const float (*world)[3], const uint32_t *argb, int nv, LampSprite *out) {
    if (nv < 3 || nv > 6) return -1;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, sum[3] = {0, 0, 0}, col[4] = {0, 0, 0, 0};
    for (int i = 0; i < nv; i++) {
        for (int a = 0; a < 3; a++) {
            const float p = world[i][a];
            sum[a] += p;
            lo[a] = p < lo[a] ? p : lo[a];
            hi[a] = p > hi[a] ? p : hi[a];
        }
        const uint32_t c = argb[i];
        col[0] += (c >> 16 & 255) / 255.0f;
        col[1] += (c >> 8 & 255) / 255.0f;
        col[2] += (c & 255) / 255.0f;
        col[3] += (c >> 24) / 255.0f;
    }
    float size = 0;
    for (int a = 0; a < 3; a++) size = hi[a] - lo[a] > size ? hi[a] - lo[a] : size;
    for (int a = 0; a < 4; a++) col[a] /= (float)nv;
    const float peak = fmaxf(col[0], fmaxf(col[1], col[2])), weight = col[3] * peak;
    if (!(size >= LAMP_MIN_SIZE) || !(weight >= LAMP_MIN_WEIGHT) || !(peak >= LAMP_MIN_PEAK)) return -1;
    for (int a = 0; a < 3; a++) {
        out->pos[a] = sum[a] / (float)nv;
        out->rgb[a] = col[a];
    }
    out->size = size;
    out->weight = weight;
    return 0;
}

static float dist2(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

uint32_t lamp_cluster(LampSprite *s, uint32_t n) {
    /* Repeated pairwise merging until no pair is close: the result does not depend on which pair
     * of one lamp's quads is merged first. */
    int merged = 1;
    while (merged && n > 1) {
        merged = 0;
        for (uint32_t i = 0; i < n && !merged; i++)
            for (uint32_t j = i + 1; j < n && !merged; j++) {
                const float r = LAMP_MERGE * fmaxf(s[i].size, s[j].size);
                if (dist2(s[i].pos, s[j].pos) > r * r) continue;
                const float wi = s[i].weight, wj = s[j].weight, w = wi + wj;
                for (int a = 0; a < 3; a++) {
                    s[i].pos[a] = (s[i].pos[a] * wi + s[j].pos[a] * wj) / w;
                    s[i].rgb[a] = (s[i].rgb[a] * wi + s[j].rgb[a] * wj) / w;
                }
                s[i].weight = w;
                s[i].size = fmaxf(s[i].size, s[j].size);
                s[j] = s[--n];
                merged = 1;
            }
    }
    return n;
}

float lamp_radius(float size) {
    const float r = LAMP_RADIUS_SCALE * size;
    return r < LAMP_RADIUS_MIN ? LAMP_RADIUS_MIN : r > LAMP_RADIUS_MAX ? LAMP_RADIUS_MAX : r;
}

void lamp_tracker_reset(LampTracker *k) {
    memset(k, 0, sizeof *k);
    k->next_id = 1;
}

static float priority(const LampTrack *t, const float eye[3]) {
    const float r = lamp_radius(t->size);
    return t->weight * t->level * r * r / (dist2(t->pos, eye) + r * r) * (t->shown ? LAMP_KEEP : 1.0f);
}

uint32_t lamp_track_update(LampTracker *k, const LampSprite *s, uint32_t n, const float eye[3], LampLight *out, uint32_t cap,
                           int (*in_view)(const float pos[3], void *ctx), void *ctx) {
    const uint32_t old = k->n;
    for (uint32_t i = 0; i < old; i++) k->t[i].matched = 0;
    /* Each sprite to the nearest unmatched old track within its match distance; sprites are taken
     * by weight (the brightest first), so a faint neighbour does not steal a lamp's track. */
    static uint8_t used[LAMP_TRACK_MAX];
    uint8_t taken[1024];
    const uint32_t ns = n < sizeof taken ? n : (uint32_t)sizeof taken;
    memset(taken, 0, ns);
    memset(used, 0, sizeof used);
    for (uint32_t step = 0; step < ns; step++) {
        uint32_t best = ns;
        for (uint32_t j = 0; j < ns; j++)
            if (!taken[j] && (best == ns || s[j].weight > s[best].weight)) best = j;
        taken[best] = 1;
        const LampSprite *sp = &s[best];
        const float m = fmaxf(LAMP_MATCH_MIN, 0.5f * lamp_radius(sp->size));
        uint32_t ti = old;
        float bd = m * m;
        for (uint32_t i = 0; i < old; i++) {
            if (k->t[i].matched) continue;
            const float d = dist2(k->t[i].pos, sp->pos);
            if (d <= bd) {
                bd = d;
                ti = i;
            }
        }
        if (ti < old) {   /* ease towards the sprite: halos jitter, flicker and change size a little */
            LampTrack *t = &k->t[ti];
            for (int a = 0; a < 3; a++) {
                t->pos[a] += 0.25f * (sp->pos[a] - t->pos[a]);
                t->rgb[a] += 0.25f * (sp->rgb[a] - t->rgb[a]);
            }
            t->size += 0.25f * (sp->size - t->size);
            t->weight += 0.25f * (sp->weight - t->weight);
            t->level = fminf(1.0f, t->level + LAMP_FADE);
            t->matched = 1;
            t->held = 0;
        } else if (k->n < LAMP_TRACK_MAX) {   /* a new light, faded in from 0 */
            LampTrack *t = &k->t[k->n++];
            memset(t, 0, sizeof *t);
            memcpy(t->pos, sp->pos, sizeof t->pos);
            memcpy(t->rgb, sp->rgb, sizeof t->rgb);
            t->size = sp->size;
            t->weight = sp->weight;
            t->id = k->next_id++;
            t->matched = 1;
        }
    }
    /* Unmatched old tracks fade (unless held out of view); those at 0 are dropped (new tracks start
     * at 0 and stay). */
    uint32_t w = 0;
    for (uint32_t i = 0; i < k->n; i++) {
        LampTrack *t = &k->t[i];
        if (i < old && !t->matched && in_view && t->held < LAMP_HOLD && !in_view(t->pos, ctx)) {
            t->held++;
        } else if (i < old && !t->matched) {
            t->level -= LAMP_FADE;
            if (t->level <= 1e-4f) continue;
        }
        k->t[w++] = *t;
    }
    k->n = w;
    /* The cap: highest priority first (selection, cap is small). */
    if (cap > LAMP_MAX_LIGHTS) cap = LAMP_MAX_LIGHTS;
    float pr[LAMP_TRACK_MAX];
    for (uint32_t i = 0; i < k->n; i++) {
        pr[i] = k->t[i].level > 0 ? priority(&k->t[i], eye) : -1;
        used[i] = 0;
    }
    uint32_t no = 0;
    while (no < cap) {
        uint32_t best = k->n;
        for (uint32_t i = 0; i < k->n; i++)
            if (!used[i] && pr[i] > 0 && (best == k->n || pr[i] > pr[best])) best = i;
        if (best == k->n) break;
        used[best] = 1;
        const LampTrack *t = &k->t[best];
        LampLight *l = &out[no++];
        memcpy(l->pos, t->pos, sizeof l->pos);
        memcpy(l->rgb, t->rgb, sizeof l->rgb);
        l->radius = lamp_radius(t->size);
        l->intensity = fminf(t->weight, 1.0f) * t->level;   /* merged quads rank higher, not brighter */
        l->id = t->id;
    }
    for (uint32_t i = 0; i < k->n; i++) k->t[i].shown = used[i];
    return no;
}

float lamp_falloff(float d, float r) {
    if (!(r > 0)) return 0;
    const float x = d / r;
    if (x >= 1) return 0;
    const float x2 = x * x, a = 1 - x2;
    return a * a / (1 + LAMP_FALLOFF_K * x2);
}
