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

int tl_find(const TexLoadQueue *q, uint64_t hash, int kind) {
    for (int i = 0; i < q->n; i++)
        if (q->slot[i].state != TL_FREE && q->slot[i].hash == hash && q->slot[i].kind == kind) return i;
    return -1;
}

/* Empties slot i (queued, ready or failed; never one being loaded). */
static void drop(TexLoadQueue *q, int i) {
    TlSlot *s = &q->slot[i];
    if (s->state == TL_READY) q->staged -= s->img.bytes;
    free(s->path);
    free(s->img.data);
    memset(s, 0, sizeof *s);
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
    *created = 1;
    return i;
}

int tl_next(TexLoadQueue *q) {
    int best = -1, prefetch_ok = q->staged < q->staged_cap;
    for (int i = 0; i < q->n && prefetch_ok; i++) prefetch_ok = !(q->slot[i].state == TL_LOADING && q->slot[i].prio == TL_PREFETCH);
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
        memset(s, 0, sizeof *s);
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
    memset(s, 0, sizeof *s);
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
