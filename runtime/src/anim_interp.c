/* --anim-interp: the pure part (anim_interp.h). */
#include "anim_interp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOLDEN 0x9E3779B97F4A7C15ull

void anim_rate_init(AnimRate *r) { memset(r, 0, sizeof *r); }

void anim_rate_frame(AnimRate *r) {
    r->frame++;
    r->cur ^= 1;
    memset(r->tab[r->cur], 0, sizeof r->tab[r->cur]);
    r->count[r->cur] = 0;
}

static uint32_t slot_of(uint64_t key) { return (uint32_t)((key * GOLDEN) >> 54) & (ANIM_RATE_SLOTS - 1); }

static AnimRateSlot *find(AnimRateSlot *tab, uint64_t key) {
    for (uint32_t i = slot_of(key), n = 0; n < ANIM_RATE_SLOTS; i = (i + 1) & (ANIM_RATE_SLOTS - 1), n++) {
        if (!tab[i].used) return NULL;
        if (tab[i].key == key) return &tab[i];
    }
    return NULL;
}

int anim_rate_draw(AnimRate *r, uint64_t id, uint64_t hash) {
    AnimRateSlot *now = r->tab[r->cur];
    /* the n-th draw of id this frame: the first key not yet in this frame's table */
    uint64_t key;
    for (uint64_t n = 0;; n++) {
        key = id ^ n * GOLDEN;
        if (!key) key = 1;
        if (!find(now, key)) break;
    }
    if (r->count[r->cur] >= ANIM_RATE_SLOTS * 3 / 4) {
        r->dropped++;
        return -1;
    }
    const uint32_t f = r->frame ? r->frame - 1 : 0;
    uint32_t i = slot_of(key);
    while (now[i].used) i = (i + 1) & (ANIM_RATE_SLOTS - 1);
    AnimRateSlot *s = &now[i];
    s->used = 1;
    s->key = key;
    s->hash = hash;
    s->since = f;
    r->count[r->cur]++;
    const AnimRateSlot *p = find(r->tab[r->cur ^ 1], key);
    if (!p) return -1;
    r->draws++;
    if (p->hash == hash) {
        s->since = p->since;
        return 0;
    }
    const uint32_t hold = f - p->since;
    r->hold[hold >= ANIM_RATE_HOLDS ? ANIM_RATE_HOLDS - 1 : hold ? hold - 1 : 0]++;
    r->fresh++;
    return 1;
}

void anim_rate_window(AnimRate *r) {
    r->draws = r->fresh = r->dropped = 0;
    memset(r->hold, 0, sizeof r->hold);
}

void anim_rate_format(const AnimRate *r, char *out, size_t cap) {
    snprintf(out, cap, "draws %llu, new %.1f%%, holds 1/2/3/4+: %llu/%llu/%llu/%llu", (unsigned long long)r->draws,
             r->draws ? 100.0 * (double)r->fresh / (double)r->draws : 0.0, (unsigned long long)r->hold[0], (unsigned long long)r->hold[1],
             (unsigned long long)r->hold[2], (unsigned long long)r->hold[3]);
}

uint64_t anim_hash_rows(const float (*rows)[4], int n) {
    uint64_t h = 14695981039346656037ull;
    const uint8_t *p = (const uint8_t *)rows;
    for (size_t i = 0; i < (size_t)(n > 0 ? n : 0) * 16; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

int anim_palette_bones(int base) {
    const int n = base < 0 || base >= 192 ? 0 : (192 - base) / 3;
    return n < ANIM_MAX_BONES ? n : ANIM_MAX_BONES;
}

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
/* column j of the bone at row r */
static void col(const float (*m)[4], int r, int j, float c[3]) {
    for (int i = 0; i < 3; i++) c[i] = m[r + i][j];
}

void anim_blend_bones(const float (*a)[4], const float (*b)[4], int base, int nbones, float t, float (*out)[4]) {
    for (int k = 0; k < nbones; k++) {
        const int r = base + 3 * k;
        if (!(t > 0) || !(t < 1)) {   /* the ends exactly (and NaN t: a) */
            memcpy(out[r], t >= 1 ? b[r] : a[r], 3 * sizeof out[r]);
            continue;
        }
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) out[r + i][j] = a[r + i][j] + (b[r + i][j] - a[r + i][j]) * t;
    }
}

int anim_bones_snap(const float (*a)[4], const float (*b)[4], int base, int nbones) {
    const float cos_max = cosf(ANIM_SNAP_DEG * 3.14159265f / 180.0f);
    for (int k = 0; k < nbones; k++) {
        const int r = base + 3 * k;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                if (!isfinite(a[r + i][j]) || !isfinite(b[r + i][j])) return 1;
        const float d[3] = {b[r][3] - a[r][3], b[r + 1][3] - a[r + 1][3], b[r + 2][3] - a[r + 2][3]};
        if (dot3(d, d) > ANIM_SNAP_DIST * ANIM_SNAP_DIST) return 1;
        /* the turn: trace of Ra^T Rb over the unit columns, cos = (trace - 1) / 2 */
        float tr = 0;
        int scaled_out = 0;
        for (int j = 0; j < 3; j++) {
            float ca[3], cb[3];
            col(a, r, j, ca);
            col(b, r, j, cb);
            const float la = sqrtf(dot3(ca, ca)), lb = sqrtf(dot3(cb, cb));
            if (la < 1e-6f || lb < 1e-6f) scaled_out = 1;
            else tr += dot3(ca, cb) / (la * lb);
        }
        if (!scaled_out && (tr - 1) * 0.5f < cos_max) return 1;
    }
    return 0;
}

AnimInterpOptions anim_interp_options;

void anim_poses_init(AnimPoses *p) { memset(p, 0, sizeof *p); }
void anim_poses_free(AnimPoses *p) {
    free(p->buf[0]);
    free(p->buf[1]);
    memset(p, 0, sizeof *p);
}
void anim_poses_frame(AnimPoses *p, int cut) {
    p->frame++;
    p->cut = cut != 0;
    p->cur ^= 1;
    memset(p->tab[p->cur], 0, sizeof p->tab[p->cur]);
    p->count[p->cur] = 0;
    p->used[p->cur] = 0;
}
void anim_poses_window(AnimPoses *p) { p->draws = p->blended = p->snaps = 0; }

static uint32_t pose_slot_of(uint64_t key) { return (uint32_t)((key * GOLDEN) >> 52) & (ANIM_POSE_SLOTS - 1); }
static AnimPoseSlot *pose_find(AnimPoseSlot *tab, uint64_t key) {
    for (uint32_t i = pose_slot_of(key), n = 0; n < ANIM_POSE_SLOTS; i = (i + 1) & (ANIM_POSE_SLOTS - 1), n++) {
        if (!tab[i].used) return NULL;
        if (tab[i].key == key) return &tab[i];
    }
    return NULL;
}
/* n rows in this frame's buffer; returns their offset, or UINT32_MAX without memory */
static uint32_t pose_alloc(AnimPoses *p, uint32_t n) {
    const int c = p->cur;
    if (p->used[c] + n > p->cap[c]) {
        size_t cap = p->cap[c] ? p->cap[c] : 4096;
        while (cap < p->used[c] + n) cap *= 2;
        float (*b)[4] = realloc(p->buf[c], cap * sizeof b[0]);
        if (!b) return UINT32_MAX;
        p->buf[c] = b;
        p->cap[c] = cap;
    }
    const uint32_t at = (uint32_t)p->used[c];
    p->used[c] += n;
    return at;
}
/* the motion rows of c packed: bones first, then the lerped rows */
static void pack_rows(const float (*c)[4], const AnimRows *m, float (*dst)[4]) {
    memcpy(dst, c[m->base > 0 ? m->base : 0], (size_t)(3 * m->nbones) * sizeof c[0]);
    memcpy(dst + 3 * m->nbones, c[m->lin_first > 0 ? m->lin_first : 0], (size_t)m->lin_n * sizeof c[0]);
}

int anim_poses_draw(AnimPoses *p, uint64_t id, const float (*c)[4], const AnimRows *m, float (*out)[4]) {
    const int nb = m->nbones > 0 ? m->nbones : 0, nl = m->lin_n > 0 ? m->lin_n : 0;
    if ((nb && (m->base < 0 || m->base + 3 * nb > 192)) || (nl && (m->lin_first < 0 || m->lin_first + nl > 192)) || !(nb || nl)) return 0;
    const AnimRows mr = {m->base, nb, m->lin_first, nl};
    AnimPoseSlot *now = p->tab[p->cur];
    uint64_t key;
    for (uint64_t n = 0;; n++) {
        key = id ^ n * GOLDEN;
        if (!key) key = 1;
        if (!pose_find(now, key)) break;
    }
    if (p->count[p->cur] >= ANIM_POSE_SLOTS * 3 / 4) return 0;
    const uint32_t rows = (uint32_t)(3 * nb + nl), f = p->frame ? p->frame - 1 : 0;
    /* this frame's buffer may move while it grows: offsets only */
    const uint32_t at = pose_alloc(p, rows);
    if (at == UINT32_MAX) return 0;
    pack_rows(c, &mr, p->buf[p->cur] + at);
    uint32_t i = pose_slot_of(key);
    while (now[i].used) i = (i + 1) & (ANIM_POSE_SLOTS - 1);
    AnimPoseSlot *s = &now[i];
    *s = (AnimPoseSlot){key, f, 1, 1, rows, at, UINT32_MAX};
    p->count[p->cur]++;
    p->draws++;
    const AnimPoseSlot *q = pose_find(p->tab[p->cur ^ 1], key);
    if (!q || q->rows != rows) return 0;
    const float (*old)[4] = (const float (*)[4])p->buf[p->cur ^ 1];
    if (!memcmp(old[q->cur], p->buf[p->cur][at], rows * sizeof c[0])) {   /* held: keep the previous state and cadence */
        s->since = q->since;
        s->period = q->period;
        if (q->prev != UINT32_MAX) {
            const uint32_t pv = pose_alloc(p, rows);
            if (pv != UINT32_MAX) {
                memcpy(p->buf[p->cur][pv], old[q->prev], rows * sizeof c[0]);
                s->prev = pv;
            }
        }
    } else {   /* new values: the last hold is the cadence */
        const uint32_t hold = f - q->since;
        s->period = hold >= 2 && hold <= ANIM_MAX_PERIOD && !p->cut ? hold : 1;
        if (s->period > 1) {
            if (nb && anim_bones_snap(old + q->cur, (const float (*)[4])(p->buf[p->cur] + at), 0, nb)) {
                p->snaps++;
                s->period = 1;
            } else {
                const uint32_t pv = pose_alloc(p, rows);
                if (pv != UINT32_MAX) {
                    memcpy(p->buf[p->cur][pv], old[q->cur], rows * sizeof c[0]);
                    s->prev = pv;
                }
            }
        }
    }
    if (s->prev == UINT32_MAX || s->period < 2) return 0;
    const float t = (float)(f - s->since + 1) / (float)s->period;
    if (!(t < 1)) return 0;
    const float (*b)[4] = (const float (*)[4])p->buf[p->cur];
    const float (*pa)[4] = b + s->prev, (*pb)[4] = b + s->cur;
    if (nb) anim_blend_bones(pa, pb, 0, nb, t, out + mr.base);
    for (int r = 0; r < nl; r++)
        for (int j = 0; j < 4; j++) {
            const float x = pa[3 * nb + r][j], y = pb[3 * nb + r][j];
            out[mr.lin_first + r][j] = x + (y - x) * t;
        }
    p->blended++;
    return 1;
}
