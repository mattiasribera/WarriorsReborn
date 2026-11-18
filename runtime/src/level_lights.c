/* --level-lights options, the LightManager reader and the per-frame selection (level_lights.h). */
#include "level_lights.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int level_lights_arg(LevelLightsConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--level-lights") || !strcmp(opt, "--no-level-lights")) {
        c->on = opt[2] == 'l';
        return 1;
    }
    if (!strcmp(opt, "--level-lights-max")) {
        char *end;
        if (!val || !*val) return -1;
        const long v = strtol(val, &end, 10);
        if (*end || v < LEVEL_LIGHTS_CAP_MIN || v > LEVEL_LIGHTS_CAP_MAX) return -1;
        c->cap = (uint32_t)v;
        return 2;
    }
    return 0;
}

static uint32_t u32_at(const uint8_t *b, uint32_t off) {
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}
static uint16_t u16_at(const uint8_t *b, uint32_t off) {
    uint16_t v;
    memcpy(&v, b + off, 2);
    return v;
}
static void f3_at(const uint8_t *b, uint32_t off, float *f) { memcpy(f, b + off, 12); }
static int finite3(const float *f) { return isfinite(f[0]) && isfinite(f[1]) && isfinite(f[2]); }
static int color_ok(const float *c) {
    for (int i = 0; i < 3; i++)
        if (!(c[i] >= 0 && c[i] <= LEVEL_COLOR_MAX)) return 0;   /* NaN fails */
    return 1;
}

/* One light object into s (-1: unreadable or invalid, 0: kept, 1: skipped). */
static int read_light(LevelRead rd, void *ctx, uint32_t va, LevelLightSet *s) {
    uint8_t b[LEVEL_LIGHT_BYTES];
    if (!va || va & 3 || rd(va, b, sizeof b, ctx)) return -1;
    const uint32_t kind = u32_at(b, LEVEL_LIGHT_KIND);
    float col[3];
    f3_at(b, LEVEL_LIGHT_COLOR, col);
    if (kind < LEVEL_KIND_POINT || kind > LEVEL_KIND_AMBIENT || !color_ok(col)) return -1;
    if (!u16_at(b, LEVEL_LIGHT_ENABLE)) return 1;
    if (kind == LEVEL_KIND_AMBIENT) {
        for (int i = 0; i < 3; i++) s->ambient[i] += col[i];
        return 0;
    }
    if (kind == LEVEL_KIND_DIR) {
        float d[3];
        f3_at(b, LEVEL_LIGHT_DIR, d);
        const float len = finite3(d) ? sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) : 0;
        if (!(len > 0.5f && len < 2.0f)) return -1;
        if (s->ndir < LEVEL_DIRS_MAX) {
            LevelDir *o = &s->dir[s->ndir++];
            for (int i = 0; i < 3; i++) o->dir[i] = d[i] / len, o->rgb[i] = col[i];
        }
        return 0;
    }
    if (kind != LEVEL_KIND_POINT) return 1;   /* spots: no level uses one */
    float pos[3], radius;
    f3_at(b, LEVEL_LIGHT_POS, pos);
    memcpy(&radius, b + LEVEL_LIGHT_RADIUS, 4);
    if (!finite3(pos) || fabsf(pos[0]) > LEVEL_POS_MAX || fabsf(pos[1]) > LEVEL_POS_MAX || fabsf(pos[2]) > LEVEL_POS_MAX) return -1;
    if (!(radius >= 0 && radius <= LEVEL_RADIUS_MAX)) return -1;
    if (radius == 0) return 1;   /* a glow-only record: its halo is drawn, it lights nothing */
    if (s->n == LEVEL_POINTS_MAX) {
        s->dropped++;
        return 0;
    }
    LevelPoint *p = &s->p[s->n++];
    memcpy(p->pos, pos, sizeof p->pos);
    memcpy(p->rgb, col, sizeof p->rgb);
    p->radius = radius;
    p->id = va;
    p->halo = u16_at(b, LEVEL_LIGHT_GLOW) != 0xffff;
    return 0;
}

static int cmp_u32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* Appends the k light pointers at va to v (tagged with tag in the low bits, as they are aligned). */
static int take(LevelRead rd, void *ctx, uint32_t va, uint32_t k, uint32_t tag, uint32_t *v, uint32_t *n) {
    if (!k) return 0;
    if (!va || va & 3 || k > LEVEL_LIST_MAX || rd(va, v + *n, k * 4, ctx)) return -1;
    for (uint32_t i = 0; i < k; i++) v[*n + i] = (v[*n + i] & 3) ? 1u : v[*n + i] | tag;   /* 1: a misaligned pointer, rejected */
    *n += k;
    return 0;
}

int level_lights_read(LevelRead rd, void *ctx, uint32_t mgr_ptr_va, LevelLightSet *out) {
    memset(out, 0, sizeof *out);
    uint32_t mgr;
    if (rd(mgr_ptr_va, &mgr, 4, ctx) || !mgr || mgr & 3) return -1;
    uint8_t m[0x80];
    if (rd(mgr, m, sizeof m, ctx)) return -1;
    /* The pool's live entries (tag 0) and both lists' entries (tag 2), each light once (sorted by
     * address; a light in a list keeps the tag). */
    static uint32_t va[3 * LEVEL_LIST_MAX];
    uint32_t n = 0;
    int sources = 0;
    const uint32_t pool = u32_at(m, LEVEL_MGR_POOL), cap = u32_at(m, LEVEL_MGR_POOL + 4), live = u32_at(m, LEVEL_MGR_POOL + 8);
    if (pool && !(pool & 3) && live <= cap && cap <= LEVEL_LIST_MAX && !take(rd, ctx, pool, live, 0, va, &n)) sources++;
    for (int l = 0; l < 2; l++) {
        const uint32_t at = l ? LEVEL_MGR_LIST1 : LEVEL_MGR_LIST0, begin = u32_at(m, at + 4), end = u32_at(m, at + 8);
        if (end < begin || (end - begin) & 3) continue;
        if (!take(rd, ctx, begin, (end - begin) / 4, 2, va, &n)) sources++;
    }
    if (!sources) return -1;
    qsort(va, n, sizeof va[0], cmp_u32);
    const uint32_t skip = u32_at(m, LEVEL_MGR_CHAR_LIGHT);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t a = va[i] & ~3u, near = (va[i] & 2) != 0;
        if (i + 1 < n && (va[i + 1] & ~3u) == a) continue;   /* the last copy (a tagged one sorts last) */
        out->listed++;
        out->in_lists += near;
        if (a == skip && a) {
            out->off++;
            continue;
        }
        const uint32_t before = out->n;
        const int r = va[i] == 1 ? -1 : read_light(rd, ctx, a, out);
        if (r < 0) out->rejected++;
        else if (r > 0) out->off++;
        if (out->n > before) out->p[out->n - 1].near = (uint8_t)near;
    }
    return 0;
}

void level_tracker_reset(LevelTracker *k) { memset(k, 0, sizeof *k); }

LampLight level_lamp(const LevelPoint *p, float level) {
    LampLight l;
    const float peak = fmaxf(p->rgb[0], fmaxf(p->rgb[1], p->rgb[2]));
    const float scale = peak > 1 ? 1 / peak : 1;
    for (int i = 0; i < 3; i++) l.pos[i] = p->pos[i], l.rgb[i] = p->rgb[i] * scale;
    l.radius = p->radius * LEVEL_RADIUS_FIT;
    l.intensity = fminf(peak > 1 ? peak : 1, LEVEL_INTENSITY_MAX) * LEVEL_GAIN * level;
    l.id = p->id;
    return l;
}

static float priority(const LevelPoint *p, int shown, const float eye[3]) {
    const float x = p->pos[0] - eye[0], y = p->pos[1] - eye[1], z = p->pos[2] - eye[2];
    const float r = p->radius * LEVEL_RADIUS_FIT;
    const float lum = 0.2126f * p->rgb[0] + 0.7152f * p->rgb[1] + 0.0722f * p->rgb[2];
    return (lum + 1e-3f) * r * r / (x * x + y * y + z * z + r * r) * (shown ? LAMP_KEEP : 1.0f) * (p->near ? LEVEL_NEAR_BONUS : 1.0f);
}

uint32_t level_lights_select(LevelTracker *k, const LevelLightSet *s, const float eye[3], int (*visible)(const float pos[3], float radius, void *ctx),
                             void *ctx, LampLight *out, uint32_t cap) {
    if (cap > LAMP_MAX_LIGHTS) cap = LAMP_MAX_LIGHTS;
    /* Each light of the frame to its track (by id; a new one starts at level 0). */
    static int16_t of[LEVEL_POINTS_MAX];   /* the frame light of each track, -1 none */
    for (uint32_t i = 0; i < k->n; i++) k->t[i].seen = 0, of[i] = -1;
    const uint32_t ns = s ? s->n : 0;
    for (uint32_t j = 0; j < ns; j++) {
        const LevelPoint *p = &s->p[j];
        uint32_t i = 0;
        while (i < k->n && k->t[i].id != p->id) i++;
        if (i == k->n) {
            if (k->n == LEVEL_TRACK_MAX) continue;
            k->t[k->n++] = (LevelTrack){p->id, 0, 0, 0};
        }
        if (k->t[i].seen) continue;   /* the same id twice: the first */
        k->t[i].seen = 1;
        of[i] = (int16_t)j;
    }
    /* Wanted: seen and in view. Ranked among the wanted (and those still fading out). */
    float pr[LEVEL_TRACK_MAX];
    uint8_t want[LEVEL_TRACK_MAX];
    for (uint32_t i = 0; i < k->n; i++) {
        const LevelTrack *t = &k->t[i];
        want[i] = 0;
        pr[i] = -1;
        if (of[i] < 0) continue;
        const LevelPoint *p = &s->p[of[i]];
        want[i] = !visible || visible(p->pos, p->radius * LEVEL_RADIUS_FIT, ctx);
        if (want[i]) pr[i] = priority(p, t->shown, eye);
    }
    /* The cap's choice: highest priority first among the wanted. */
    uint8_t pick[LEVEL_TRACK_MAX];
    memset(pick, 0, sizeof pick);
    for (uint32_t c = 0; c < cap; c++) {
        uint32_t best = k->n;
        for (uint32_t i = 0; i < k->n; i++)
            if (want[i] && !pick[i] && (best == k->n || pr[i] > pr[best])) best = i;
        if (best == k->n) break;
        pick[best] = 1;
    }
    /* Levels: picked lights fade in, the rest fade out; tracks out at 0 and unseen are dropped.
     * The output: picked lights, then those fading out while there is room (still seen ones only:
     * a light gone from the list has no position this frame). */
    uint32_t no = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < k->n; i++) {
            LevelTrack *t = &k->t[i];
            if (pass == 0) {
                t->level = pick[i] ? fminf(1.0f, t->level + LAMP_FADE) : fmaxf(0.0f, t->level - LAMP_FADE);
                t->shown = 0;
            }
            if ((pass == 0) != (pick[i] != 0) || of[i] < 0 || !(t->level > 1e-4f) || no == cap) continue;
            out[no++] = level_lamp(&s->p[of[i]], t->level);
            t->shown = pick[i];
        }
    uint32_t w = 0;
    for (uint32_t i = 0; i < k->n; i++)
        if (k->t[i].seen || k->t[i].level > 1e-4f) k->t[w++] = k->t[i];
    k->n = w;
    return no;
}
