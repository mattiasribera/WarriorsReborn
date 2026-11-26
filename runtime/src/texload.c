/* Texture pack load queue (see texload.h). */
#include "texload.h"
#include <stdlib.h>
#include <string.h>

void tl_init(TexLoadQueue *q, TlSlot *slots, int n, size_t staged_cap, uint32_t keep_frames, size_t budget_bytes, double budget_ms) {
    memset(q, 0, sizeof *q);
    memset(slots, 0, sizeof *slots * (size_t)n);
    q->slot = slots;
    q->n = n;
    q->staged_cap = staged_cap;
    q->keep_frames = keep_frames;
    q->budget_bytes = budget_bytes;
    q->budget_ms = budget_ms;
}

#define IDX_MASK (2 * TL_MAX_SLOTS - 1)
static uint32_t idx_home(uint64_t hash, int kind) {
    uint64_t h = hash ^ (uint64_t)(kind + 1) * 0x9e3779b97f4a7c15ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (uint32_t)h & IDX_MASK;
}

int tl_find(const TexLoadQueue *q, uint64_t hash, int kind) {
    for (uint32_t k = idx_home(hash, kind); q->index[k]; k = (k + 1) & IDX_MASK) {
        const TlSlot *s = &q->slot[q->index[k] - 1];
        if (s->hash == hash && s->kind == kind) return q->index[k] - 1;
    }
    return -1;
}

static void idx_add(TexLoadQueue *q, int i) {
    uint32_t k = idx_home(q->slot[i].hash, q->slot[i].kind);
    while (q->index[k]) k = (k + 1) & IDX_MASK;
    q->index[k] = (uint16_t)(i + 1);
}

/* Removes slot i (still holding its hash and kind) from the index: backward-shift deletion. */
static void idx_remove(TexLoadQueue *q, int i) {
    uint32_t p = idx_home(q->slot[i].hash, q->slot[i].kind);
    while (q->index[p] && q->index[p] != i + 1) p = (p + 1) & IDX_MASK;
    if (!q->index[p]) return;
    for (uint32_t j = (p + 1) & IDX_MASK; q->index[j]; j = (j + 1) & IDX_MASK) {
        const TlSlot *s = &q->slot[q->index[j] - 1];
        const uint32_t home = idx_home(s->hash, s->kind);
        /* the entry at j may move to p when its home is not in (p, j] (cyclically) */
        if (((j - home) & IDX_MASK) >= ((j - p) & IDX_MASK)) {
            q->index[p] = q->index[j];
            p = j;
        }
    }
    q->index[p] = 0;
}

/* Frees slot i (its payload is the caller's or already moved out). */
static void release(TexLoadQueue *q, int i) {
    idx_remove(q, i);
    memset(&q->slot[i], 0, sizeof q->slot[i]);
}

/* Empties slot i (queued, ready or failed; never one being loaded). */
static void drop(TexLoadQueue *q, int i) {
    TlSlot *s = &q->slot[i];
    if (s->state == TL_READY) q->staged -= s->img.bytes;
    free(s->path);
    free(s->img.data);
    release(q, i);
}

/* The oldest prefetch slot in `state`, or -1. */
static int oldest_prefetch(const TexLoadQueue *q, int state) {
    int best = -1;
    for (int i = 0; i < q->n; i++) {
        const TlSlot *s = &q->slot[i];
        if (s->state == state && s->prio == TL_PREFETCH && (best < 0 || s->seq < q->slot[best].seq)) best = i;
    }
    return best;
}

int tl_request(TexLoadQueue *q, uint64_t hash, int kind, int prio, uint32_t frame, int *created) {
    *created = 0;
    int i = tl_find(q, hash, kind);
    if (i >= 0) {
        TlSlot *s = &q->slot[i];
        s->touched = frame;
        if (prio == TL_DEMAND && s->prio == TL_PREFETCH) {
            q->promoted++;
            q->prefetch_hits += s->state == TL_READY;
            s->prio = TL_DEMAND;
            s->seq = ++q->seq;   /* among demand requests it was asked for now */
        }
        return i;
    }
    for (i = 0; i < q->n && q->slot[i].state != TL_FREE; i++) {}
    if (i == q->n) {
        if (prio != TL_DEMAND) return -1;
        if ((i = oldest_prefetch(q, TL_QUEUED)) < 0 && (i = oldest_prefetch(q, TL_READY)) < 0) return -1;
        drop(q, i);
        q->dropped_cap++;
    }
    q->slot[i] = (TlSlot){.hash = hash, .kind = kind, .prio = prio, .state = TL_QUEUED, .touched = frame, .seq = ++q->seq};
    idx_add(q, i);
    *created = 1;
    return i;
}

int tl_next(TexLoadQueue *q) {
    int best = -1, prefetch_ok = q->staged < q->staged_cap, loading = 0;
    const int par = q->prefetch_par > 1 ? q->prefetch_par : 1;
    for (int i = 0; i < q->n && prefetch_ok; i++) prefetch_ok = (loading += q->slot[i].state == TL_LOADING && q->slot[i].prio == TL_PREFETCH) < par;
    for (int i = 0; i < q->n; i++) {
        const TlSlot *s = &q->slot[i];
        if (s->state != TL_QUEUED || (s->prio == TL_PREFETCH && !prefetch_ok)) continue;
        if (best < 0 || s->prio < q->slot[best].prio || (s->prio == q->slot[best].prio && s->seq < q->slot[best].seq)) best = i;
    }
    if (best >= 0) q->slot[best].state = TL_LOADING;
    return best;
}

void tl_done(TexLoadQueue *q, int i, const TlImage *img, int failed, uint32_t frame) {
    TlSlot *s = &q->slot[i];
    s->img = *img;
    if (failed) {
        free(s->img.data);
        s->img.data = NULL;
        s->img.bytes = 0;
    }
    s->state = failed ? TL_FAILED : TL_READY;
    if (!failed) q->staged += s->img.bytes;
    /* The keep_frames window starts when the image is there, not when it was asked for. */
    if ((int32_t)(frame - s->touched) > 0) s->touched = frame;
}

static void budget_frame(TexLoadQueue *q, uint32_t frame) {
    if (frame == q->frame) return;
    q->frame = frame;
    q->up_bytes = 0;
    q->up_ms = 0;
    q->ups = 0;
}

int tl_take(TexLoadQueue *q, int i, uint32_t frame, TlImage *out) {
    TlSlot *s = &q->slot[i];
    if (s->state == TL_FAILED) {
        *out = s->img;
        release(q, i);
        return -1;
    }
    if (s->state != TL_READY) return 0;
    budget_frame(q, frame);
    if (q->ups && (q->up_bytes + s->img.bytes > q->budget_bytes || q->up_ms >= q->budget_ms)) {
        q->deferred += !s->waited;
        s->waited = 1;
        return 0;
    }
    q->ups++;
    q->up_bytes += s->img.bytes;
    q->staged -= s->img.bytes;
    *out = s->img;
    release(q, i);
    return 1;
}

void tl_charge_ms(TexLoadQueue *q, uint32_t frame, double ms) {
    budget_frame(q, frame);
    q->up_ms += ms;
}

void tl_reclaim(TexLoadQueue *q, uint32_t frame) {
    for (int i = 0; i < q->n; i++) {
        const TlSlot *s = &q->slot[i];
        const uint32_t keep = s->prio == TL_PREFETCH ? 4 * q->keep_frames : q->keep_frames;   /* a level may load long before it draws */
        if ((s->state == TL_FAILED || s->state == TL_READY) && frame - s->touched > keep) {
            drop(q, i);
            q->dropped_stale++;
        }
    }
    int i;
    while (q->staged > q->staged_cap && (i = oldest_prefetch(q, TL_READY)) >= 0) {
        drop(q, i);
        q->dropped_cap++;
    }
}

int tl_count(const TexLoadQueue *q, int state) {
    int n = 0;
    for (int i = 0; i < q->n; i++) n += q->slot[i].state == state;
    return n;
}

int tl_take_prefetch(TexLoadQueue *q, int kind, uint32_t frame, TlImage *out, uint64_t *hash) {
    int best = -1;
    for (int i = 0; i < q->n; i++) {
        const TlSlot *s = &q->slot[i];
        if (s->state == TL_READY && s->prio == TL_PREFETCH && s->kind == kind && (best < 0 || s->seq < q->slot[best].seq)) best = i;
    }
    if (best < 0) return 0;
    TlSlot *s = &q->slot[best];
    budget_frame(q, frame);
    if (q->up_bytes + s->img.bytes > q->budget_bytes || q->up_ms >= q->budget_ms) return 0;
    q->ups++;
    q->up_bytes += s->img.bytes;
    q->staged -= s->img.bytes;
    *out = s->img;
    *hash = s->hash;
    release(q, best);
    return 1;
}

static uint32_t list_slot(uint64_t hash) {
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    return (uint32_t)hash & (2 * TL_LIST_MAX - 1);
}

void tl_list_clear(TlList *l) {
    l->n = 0;
    memset(l->index, 0, sizeof l->index);
}

int tl_list_find(const TlList *l, uint64_t hash) {
    for (uint32_t k = list_slot(hash);; k = (k + 1) & (2 * TL_LIST_MAX - 1)) {
        const int e = l->index[k];
        if (!e) return -1;
        if (l->hash[e - 1] == hash) return e - 1;
    }
}

int tl_list_add(TlList *l, uint64_t hash, int late) {
    uint32_t k = list_slot(hash);
    for (; l->index[k]; k = (k + 1) & (2 * TL_LIST_MAX - 1))
        if (l->hash[l->index[k] - 1] == hash) {
            if (!late) l->late[l->index[k] - 1] = 0;
            return 0;
        }
    if (l->n >= TL_LIST_MAX) return -1;   /* the index is at most half full: probes end */
    l->hash[l->n] = hash;
    l->late[l->n] = late != 0;
    l->index[k] = (uint16_t)++l->n;
    return 1;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int tl_list_parse(TlList *l, const char *text) {
    int added = 0;
    for (const char *p = text; *p;) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        size_t len = (size_t)(e - p);
        if (len && p[len - 1] == '\r') len--;
        if (len == 18 && (p[0] == 'f' || p[0] == 'l') && p[1] == ' ') {
            uint64_t h = 0;
            int ok = 1;
            for (int i = 2; i < 18 && ok; i++) {
                const int v = hexval(p[i]);
                ok = v >= 0;
                h = h << 4 | (uint64_t)(v & 15);
            }
            if (ok && tl_list_add(l, h, p[0] == 'l') == 1) added++;
        }
        p = *e ? e + 1 : e;
    }
    return added;
}

size_t tl_list_format(const TlList *l, char *buf, size_t cap) {
    static const char head[] = "# xbrun level texture list (texture pack content hashes): f = drawn in the level's first seconds, l = later\n";
    static const char hex[] = "0123456789abcdef";
    const size_t need = sizeof head - 1 + (size_t)l->n * 19;
    if (need >= cap) return need;
    memcpy(buf, head, sizeof head - 1);
    char *o = buf + sizeof head - 1;
    for (int i = 0; i < l->n; i++) {
        *o++ = l->late[i] ? 'l' : 'f';
        *o++ = ' ';
        for (int d = 15; d >= 0; d--) *o++ = hex[(l->hash[i] >> (4 * d)) & 15];
        *o++ = '\n';
    }
    *o = 0;
    return need;
}

int tl_upload_failed(TlUploadFails *f, uint64_t hash) {
    const int i = (int)(hash % TL_UPLOAD_SLOTS);
    if (f->hash[i] != hash) {
        f->hash[i] = hash;
        f->fails[i] = 0;
    }
    if (f->fails[i] < TL_UPLOAD_TRIES) f->fails[i]++;
    return f->fails[i] < TL_UPLOAD_TRIES;
}

void tl_upload_ok(TlUploadFails *f, uint64_t hash) {
    const int i = (int)(hash % TL_UPLOAD_SLOTS);
    if (f->hash[i] == hash) f->fails[i] = 0;
}
