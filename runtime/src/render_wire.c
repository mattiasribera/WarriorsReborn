/* Wire format of the renderer.h call stream: writer, reader and the codecs (render_wire.h). */
#include "render_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RW_GEN_CONFIG
#include "render_wire_gen.h"

const char *const rw_cat_name[RW_CATS] = {"args", "vertices", "indices", "normals", "skin-programs", "skin-constants",
                                          "skin-streams", "base", "textures", "movie", "maps"};

/* ---- writer ---- */
/* Out of memory: the buffer keeps its bytes but reads as full (cap 0), so the inline writers
 * (render_wire.h rw_put_fast) take the slow path, which drops everything until rw_reset. */
static void reserve(RwWriter *w, size_t more) {
    if (w->bad || w->n + more <= w->cap) return;
    size_t cap = w->cap ? w->cap : 1u << 16;
    while (cap < w->n + more) cap *= 2;
    uint8_t *b = realloc(w->buf, cap);
    if (!b) { w->bad = 1; w->cap = 0; return; }
    w->buf = b;
    w->cap = cap;
}
void rw_put_slow(RwWriter *w, const void *p, size_t n) {
    reserve(w, n);
    if (w->bad) return;
    memcpy(w->buf + w->n, p, n);
    w->n += n;
}
#define put rw_put_fast
static void pad(RwWriter *w) {
    static const uint8_t zero[RW_ALIGN];
    const size_t k = (RW_ALIGN - (w->n - w->rec) % RW_ALIGN) % RW_ALIGN;
    if (k) put(w, zero, k);
}
void rw_reset(RwWriter *w) {
    w->n = w->rec = 0;
    w->bad = 0;
    memset(w->cat, 0, sizeof w->cat);
}
void rw_free(RwWriter *w) {
    free(w->buf);
    memset(w, 0, sizeof *w);
}
void rw_begin_record(RwWriter *w, uint32_t id, uint32_t flags) {
    w->rec = w->n;
    const RwRecordHeader h = {(uint16_t)id, (uint16_t)flags, 0};
    put(w, &h, sizeof h);
}
void rw_end_record(RwWriter *w) {
    pad(w);
    if (w->bad) return;
    const uint32_t len = (uint32_t)(w->n - w->rec - sizeof(RwRecordHeader));
    memcpy(w->buf + w->rec + offsetof(RwRecordHeader, len), &len, sizeof len);
}
void rw_bytes(RwWriter *w, const void *p, size_t n) { put(w, p, n); }
/* ---- stream state (render_wire.h: references, constant deltas, the base image) ---- */
#define HASH_BITS 16
#define HASH_N (1u << HASH_BITS)
typedef struct { const void *p; size_t n, off; uint32_t idx, gen, wgen; } Ref;
typedef struct RwCache RwCache;
struct RwEnc {
    Ref pay[HASH_N];    /* whole payloads of the chunk by (pointer, size) */
    Ref prog[HASH_N];   /* skinning programs of the chunk by pointer */
    uint32_t gen, npay, used, prog_used, wgen;
    RwCache *cache;     /* the content cache (NULL: off) */
    const void *last_consts;
    uint8_t consts[192 * 16];   /* the registers as the reader has them */
    uint8_t *base;
    size_t base_n, base_cap;
    int have_base, base_h_ok;   /* base_h: rw_hash64 of the last image (when base_h_ok) */
    uint64_t base_h;
    RwEncStats st;
    void (*whole)(const void *p, size_t n, int cat);
    int (*known_hash)(const void *p, size_t n, uint64_t *h);
    int (*known_delta)(const void *cur, const void *prev, uint32_t mask[6]);
};
RwEnc *rw_enc_new(void) {
    RwEnc *e = calloc(1, sizeof *e);
    if (e) e->gen = 1;
    return e;
}
static void cache_free(RwCache *c);
static void cache_frame(RwCache *c);
static void cache_clear(RwCache *c);
void rw_enc_free(RwEnc *e) {
    if (e) {
        free(e->base);
        cache_free(e->cache);
    }
    free(e);
}
void rw_enc_frame(RwEnc *e) {
    if (++e->gen == 0) {   /* wrapped: every entry is stale */
        memset(e->pay, 0, sizeof e->pay);
        memset(e->prog, 0, sizeof e->prog);
        e->gen = 1;
    }
    e->npay = e->used = e->prog_used = 0;
    e->last_consts = NULL;
    if (e->cache) cache_frame(e->cache);
}
void rw_enc_writer_reset(RwEnc *e) { e->wgen++; }
void rw_enc_stream(RwEnc *e) {
    rw_enc_frame(e);
    if (e->cache) cache_clear(e->cache);
    memset(e->consts, 0, sizeof e->consts);
    e->have_base = 0;
}
RwEncStats *rw_enc_stats(RwEnc *e) { return &e->st; }
void rw_enc_on_whole(RwEnc *e, void (*fn)(const void *p, size_t n, int cat)) { e->whole = fn; }
void rw_enc_hash_hook(RwEnc *e, int (*fn)(const void *p, size_t n, uint64_t *h)) { e->known_hash = fn; }
void rw_enc_consts_hook(RwEnc *e, int (*fn)(const void *cur, const void *prev, uint32_t mask[6])) { e->known_delta = fn; }
static uint32_t ref_hash(const void *p, size_t n) {
    return (uint32_t)((((uintptr_t)p >> 2) ^ (uint32_t)n * 0x9E3779B1u) * 2654435761u) >> (32 - HASH_BITS);
}
/* The entry of key (p, n; by_ptr: p alone) in this chunk, or the free slot it would take (NULL: full). */
static Ref *ref_slot(Ref *tab, uint32_t gen, const void *p, size_t n, int by_ptr) {
    uint32_t i = ref_hash(p, by_ptr ? 0 : n);
    for (uint32_t k = 0; k < HASH_N; k++, i = (i + 1) & (HASH_N - 1)) {
        Ref *r = &tab[i];
        if (r->gen != gen) return r;
        if (r->p == p && (by_ptr || r->n == n)) return r;
    }
    return NULL;
}
static void whole_bytes(RwWriter *w, const void *p, size_t n, int cat) {
    rw_u32(w, (uint32_t)n);
    pad(w);
    put(w, p, n);
    pad(w);
    if (cat != RW_CAT_ARGS) w->cat[cat] += n;
}
/* ---- the content hash (rw_hash64): an XXH3-style accumulator ----
 * 8 64-bit lanes; each 64-byte stripe adds, per lane, the neighbour lane's data word and the product
 * of the low and high halves of (data ^ key); every 16 stripes the lanes are scrambled; the last
 * stripe is the input's last 64 bytes (overlapping); then the lanes, the size and the key are mixed
 * down to 64 bits. Shorter inputs are hashed as one zero-padded stripe. SSE2 and scalar code give the
 * same values (the 64-bit products of 32-bit halves are exact in both). */
#define HSECRET 24
static uint64_t hsecret[HSECRET];
static void hash_init(void) {
    if (hsecret[0]) return;
    uint64_t x = 0x243F6A8885A308D3ull;
    for (int i = 0; i < HSECRET; i++) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        hsecret[i] = (z ^ (z >> 31)) | 1;   /* never 0: hsecret[0] marks the table as made */
    }
}
static const uint64_t hacc0[8] = {0xC2B2AE3Dull, 0x9E3779B185EBCA87ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull,
                                  0x85EBCA77C2B2AE63ull, 0x85EBCA77ull, 0x27D4EB2F165667C5ull, 0x9E3779B1ull};
#define HPRIME32 0x9E3779B1u
static uint64_t load64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static void stripe_scalar(uint64_t acc[8], const uint8_t *p, const uint64_t *k) {
    uint64_t d[8];
    for (int i = 0; i < 8; i++) d[i] = load64(p + 8 * i);
    for (int i = 0; i < 8; i++) {
        const uint64_t dk = d[i] ^ k[i];
        acc[i] += d[i ^ 1] + (uint64_t)(uint32_t)dk * (uint32_t)(dk >> 32);
    }
}
static void scramble_scalar(uint64_t acc[8], const uint64_t *k) {
    for (int i = 0; i < 8; i++) {
        uint64_t a = acc[i] ^ (acc[i] >> 47);
        a ^= k[i];
        acc[i] = a * HPRIME32;
    }
}
static uint64_t hash_final(const uint64_t acc[8], size_t n) {
    uint64_t h = (uint64_t)n * 0x9E3779B185EBCA87ull;
    for (int i = 0; i < 8; i++) {
        h = (h ^ (acc[i] + hsecret[16 + i])) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 32;
    }
    h ^= h >> 37;
    h *= 0x165667919E3779F9ull;
    return h ^ (h >> 32);
}
static uint64_t hash_short(const void *p, size_t n) {
    uint8_t b[64] = {0};
    memcpy(b, p, n);
    uint64_t acc[8];
    memcpy(acc, hacc0, sizeof acc);
    stripe_scalar(acc, b, hsecret);
    return hash_final(acc, n);
}
uint64_t rw_hash64_scalar(const void *p, size_t n) {
    hash_init();
    if (n < 64) return hash_short(p, n);
    const uint8_t *b = p;
    uint64_t acc[8];
    memcpy(acc, hacc0, sizeof acc);
    const size_t stripes = (n - 1) / 64;
    for (size_t s = 0; s < stripes; s++) {
        stripe_scalar(acc, b + 64 * s, hsecret + (s & 15));
        if ((s & 15) == 15) scramble_scalar(acc, hsecret + 16);
    }
    stripe_scalar(acc, b + n - 64, hsecret + 7);
    return hash_final(acc, n);
}
#if defined(__SSE2__)
#include <emmintrin.h>
static void stripe_sse2(__m128i acc[4], const uint8_t *p, const uint64_t *k) {
    for (int i = 0; i < 4; i++) {
        const __m128i d = _mm_loadu_si128((const __m128i *)(const void *)(p + 16 * i));
        const __m128i dk = _mm_xor_si128(d, _mm_loadu_si128((const __m128i *)(const void *)(k + 2 * i)));
        const __m128i prod = _mm_mul_epu32(dk, _mm_srli_epi64(dk, 32));
        const __m128i swap = _mm_shuffle_epi32(d, _MM_SHUFFLE(1, 0, 3, 2));
        acc[i] = _mm_add_epi64(acc[i], _mm_add_epi64(prod, swap));
    }
}
static void scramble_sse2(__m128i acc[4], const uint64_t *k) {
    const __m128i prime = _mm_set1_epi32((int)HPRIME32);
    for (int i = 0; i < 4; i++) {
        __m128i a = _mm_xor_si128(acc[i], _mm_srli_epi64(acc[i], 47));
        a = _mm_xor_si128(a, _mm_loadu_si128((const __m128i *)(const void *)(k + 2 * i)));
        const __m128i lo = _mm_mul_epu32(a, prime), hi = _mm_mul_epu32(_mm_srli_epi64(a, 32), prime);
        acc[i] = _mm_add_epi64(lo, _mm_slli_epi64(hi, 32));
    }
}
uint64_t rw_hash64(const void *p, size_t n) {
    hash_init();
    if (n < 64) return hash_short(p, n);
    const uint8_t *b = p;
    __m128i acc[4];
    for (int i = 0; i < 4; i++) acc[i] = _mm_loadu_si128((const __m128i *)(const void *)(hacc0 + 2 * i));
    const size_t stripes = (n - 1) / 64;
    for (size_t s = 0; s < stripes; s++) {
        stripe_sse2(acc, b + 64 * s, hsecret + (s & 15));
        if ((s & 15) == 15) scramble_sse2(acc, hsecret + 16);
    }
    stripe_sse2(acc, b + n - 64, hsecret + 7);
    uint64_t a[8];
    for (int i = 0; i < 4; i++) _mm_storeu_si128((__m128i *)(void *)(a + 2 * i), acc[i]);
    return hash_final(a, n);
}
#else
uint64_t rw_hash64(const void *p, size_t n) { return rw_hash64_scalar(p, n); }
#endif

/* ---- the content cache (render_wire.h; the writer's side) ----
 * Entries by (hash, size) in an open-addressed table of 64-byte buckets of two 32-byte entries, so a
 * lookup reads one cache line (the chained buckets + entry array cost two
 * misses per payload). An entry stays in its slot until it leaves (its index is stable: the queue refers to
 * it); a bucket counts the entries that probed past it (ovf), so a lookup ends at the first bucket without
 * one and a removal needs no tombstone. At most half the slots are used. The arena is filled circularly
 * from `head`, and the allocations are kept in a queue in allocation order (= arena order from head on).
 * Content of the previous lap lies at or after head, so the queue's front is always the next content head
 * reaches. A queue item whose entry moved or left is stale (its seq differs) and is skipped. */
#define CACHE_ENTS (1u << 17)          /* entries at most (slot 0 is reserved: index 0 = none) */
#define CACHE_SLOTS (2u * CACHE_ENTS)   /* table slots: two per bucket */
#define CACHE_BUCKETS (CACHE_SLOTS / 2)
#define CACHE_Q (1u << 18)
#define CACHE_ALIGN 16u
/* n 0: a free slot; h2: rw_enc_cache_verify; ovf: in a bucket's first slot, the entries that probed past it */
typedef struct { uint64_t h; uint32_t n, off, seq, last, h2; uint16_t ovf, unused; } CEnt;
_Static_assert(sizeof(CEnt) == 32, "two entries per 64-byte bucket");
typedef struct { uint32_t ent, seq; } CQItem;
struct RwCache {
    uint32_t arena, head, frame, frame_bytes, budget, zone, seq;
    CEnt *ent;              /* the table (CACHE_SLOTS, 64-byte aligned in ent_mem); [0] reserved: 0 = none */
    void *ent_mem;
    uint32_t nused;
    CQItem *q;
    uint32_t qhead, qn;     /* front index, items */
    int keep_popped;        /* cache_alloc dropped the queue item of the entry it moves */
    int verify;
};
static uint32_t bucket_of(uint64_t h);
static void cache_free(RwCache *c) {
    if (!c) return;
    free(c->ent_mem);
    free(c->q);
    free(c);
}
static void cache_clear(RwCache *c) {
    memset(c->ent, 0, CACHE_SLOTS * sizeof *c->ent);
    c->ent[0].n = UINT32_MAX;   /* never free, never matches (cached payloads are at most arena/8) */
    c->nused = c->qhead = c->qn = 0;
    c->head = c->frame_bytes = 0;
    c->frame += 2;   /* nothing counts as used in this chunk or the one before */
}
static void cache_frame(RwCache *c) {
    c->frame++;
    c->frame_bytes = 0;
}
int rw_enc_cache(RwEnc *e, uint32_t arena_bytes) {
    cache_free(e->cache);
    e->cache = NULL;
    if (!arena_bytes) return 0;
    if (arena_bytes % CACHE_ALIGN || arena_bytes < (1u << 20)) return -1;
    RwCache *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->ent_mem = malloc(CACHE_SLOTS * sizeof *c->ent + 63);
    c->q = malloc(CACHE_Q * sizeof *c->q);
    if (!c->ent_mem || !c->q) { cache_free(c); return -1; }
    c->ent = (CEnt *)(((uintptr_t)c->ent_mem + 63) & ~(uintptr_t)63);
    c->arena = arena_bytes;
    c->budget = arena_bytes / 8;   /* new + moved bytes per frame chunk */
    c->zone = arena_bytes / 2;     /* hits closer than this to head move */
    c->frame = 2;
    cache_clear(c);
    e->cache = c;
    hash_init();
    return 0;
}
void rw_enc_cache_prefetch(RwEnc *e, uint64_t h, int stage) {
    RwCache *c = e->cache;
    if (c) __builtin_prefetch(&c->ent[2 * bucket_of(h)]);   /* the bucket's line: the whole lookup (both stages) */
}
void rw_enc_cache_verify(RwEnc *e, int on) {
    if (e->cache) e->cache->verify = on;
}
static uint32_t hash2(const uint8_t *p, size_t n) {   /* FNV-1a over 32-bit words, then the tail */
    uint32_t h = 2166136261u ^ (uint32_t)n;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t v;
        memcpy(&v, p + i, 4);
        h = (h ^ v) * 16777619u;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h ^ (h >> 15);
}
static uint32_t bucket_of(uint64_t h) { return (uint32_t)(h >> 32 ^ h) & (CACHE_BUCKETS - 1); }
static uint32_t cache_find(RwCache *c, uint64_t h, uint32_t n) {
    for (uint32_t b = bucket_of(h);; b = (b + 1) & (CACHE_BUCKETS - 1)) {
        const CEnt *x = &c->ent[2 * b];
        if (x[0].n == n && x[0].h == h) return 2 * b;
        if (x[1].n == n && x[1].h == h) return 2 * b + 1;
        if (!x[0].ovf) return 0;
    }
}
/* A free slot for hash h (there is one: at most half the slots are used); the buckets it probes past count it. */
static uint32_t cache_slot_for(RwCache *c, uint64_t h) {
    for (uint32_t b = bucket_of(h);; b = (b + 1) & (CACHE_BUCKETS - 1)) {
        CEnt *x = &c->ent[2 * b];
        if (!x[0].n) return 2 * b;
        if (!x[1].n) return 2 * b + 1;
        x[0].ovf++;
    }
}
static void cache_drop(RwCache *c, uint32_t i) {
    for (uint32_t b = bucket_of(c->ent[i].h); b != i / 2; b = (b + 1) & (CACHE_BUCKETS - 1)) c->ent[2 * b].ovf--;
    c->ent[i].n = 0;
    c->ent[i].seq = 0;
    c->nused--;
}
static int q_front_stale(RwCache *c) {
    const CQItem *it = &c->q[c->qhead];
    return c->ent[it->ent].seq != it->seq;
}
static void q_pop(RwCache *c) {
    c->qhead = (c->qhead + 1) & (CACHE_Q - 1);
    c->qn--;
}
/* Evicts the queue's front entry; 0 when it is in use (this chunk or the one before). keep: an entry
 * being moved (its queue item is only dropped). */
static int evict_front(RwCache *c, RwEncStats *st, uint32_t keep) {
    const CQItem it = c->q[c->qhead];
    if (c->ent[it.ent].seq != it.seq) { q_pop(c); return 1; }
    if (it.ent != keep && c->ent[it.ent].last + 1 >= c->frame) return 0;
    q_pop(c);
    if (it.ent != keep) {
        cache_drop(c, it.ent);
        st->cache_evicted++;
    } else c->keep_popped = 1;
    return 1;
}
/* An arena offset for n bytes (evicting what it overlaps), or UINT32_MAX (refused: the payload goes whole). */
static uint32_t cache_alloc(RwCache *c, RwEncStats *st, uint32_t n, uint32_t keep) {
    const uint32_t n16 = (n + CACHE_ALIGN - 1) & ~(CACHE_ALIGN - 1);
    c->keep_popped = 0;
    if (n16 > c->arena / 8 || c->frame_bytes + n16 > c->budget) return UINT32_MAX;
    if (c->head + n16 > c->arena) {   /* wrap: the tail end of the arena is given up */
        while (c->qn && (q_front_stale(c) || c->ent[c->q[c->qhead].ent].off >= c->head))
            if (!evict_front(c, st, keep)) return UINT32_MAX;
        c->head = 0;
    }
    while (c->qn) {
        if (q_front_stale(c)) { q_pop(c); continue; }
        const CEnt *f = &c->ent[c->q[c->qhead].ent];
        if (f->off < c->head || f->off >= c->head + n16) break;
        if (!evict_front(c, st, keep)) return UINT32_MAX;
    }
    /* room for the entry and its queue item (the oldest go first) */
    while ((c->qn == CACHE_Q || (!keep && c->nused >= CACHE_ENTS - 1)) && c->qn)
        if (!evict_front(c, st, keep)) return UINT32_MAX;
    if (c->qn == CACHE_Q || (!keep && c->nused >= CACHE_ENTS - 1)) return UINT32_MAX;
    const uint32_t off = c->head;
    c->head += n16;
    c->frame_bytes += n16;
    return off;
}
static void q_push(RwCache *c, uint32_t i) {
    c->ent[i].seq = ++c->seq ? c->seq : ++c->seq;
    c->q[(c->qhead + c->qn) & (CACHE_Q - 1)] = (CQItem){i, c->ent[i].seq};
    c->qn++;
}
#if defined(__i386__) || defined(__x86_64__)
static uint64_t ticks(void) { return __builtin_ia32_rdtsc(); }
#else
static uint64_t ticks(void) { return 0; }
#endif
/* The payload through the cache: 1 when written (a hit, a move or new content), 0 when it must go whole. */
static int cache_put(RwWriter *w, RwEnc *e, const void *p, size_t n, int cat) {
    RwCache *c = e->cache;
    if (cat == RW_CAT_TEXTURES || cat == RW_CAT_MOVIE || cat == RW_CAT_BASE || n < RW_CACHE_MIN || n > c->arena / 8) return 0;
    if (cat == RW_CAT_ARGS && n < RW_CACHE_ARGS_MIN) return 0;   /* cheaper to send than to hash and look up */
    const uint64_t t0 = w->timed ? ticks() : 0;
    uint64_t h;
    /* the hash-ahead worker queues geometry, never other arguments (render_rec_prehash) */
    if (cat != RW_CAT_ARGS && e->known_hash && e->known_hash(p, n, &h)) e->st.cache_prehashed += n;
    else {
        h = rw_hash64(p, n);
        e->st.cache_hashed += n;
    }
    if (w->timed) e->st.cache_hash_ticks += ticks() - t0;
    uint32_t i = cache_find(c, h, (uint32_t)n);
    const uint32_t h2 = c->verify ? hash2(p, n) : 0;
    if (i && c->ent[i].h2 != h2) {   /* a collision: the bytes differ */
        e->st.cache_collisions++;
        cache_drop(c, i);   /* its queue item turns stale */
        i = 0;
    }
    if (i) {
        CEnt *x = &c->ent[i];
        const uint32_t dist = x->off >= c->head ? x->off - c->head : x->off + c->arena - c->head;
        if (dist < c->zone && x->last != c->frame) {   /* about to be overwritten: move it to the head (once a chunk) */
            const uint32_t to = cache_alloc(c, &e->st, x->n, i);
            if (to != UINT32_MAX) {
                rw_u32(w, RW_CACHE_MOVE);
                rw_u32(w, x->off);
                rw_u32(w, to);
                rw_u32(w, x->n);
                x->off = to;
                q_push(c, i);
                x->last = c->frame;
                e->st.cache_moves++;
                e->st.cache_move_bytes += n;
                e->st.cache_hit_bytes[cat] += n;
                return 1;
            }
            /* no room this chunk: still a hit where it is (nothing overwrites it in this chunk) */
            if (c->keep_popped) {   /* it was the oldest: back to the front of the queue */
                c->qhead = (c->qhead - 1) & (CACHE_Q - 1);
                c->q[c->qhead] = (CQItem){i, x->seq};
                c->qn++;
            }
        }
        rw_u32(w, RW_CACHED);
        rw_u32(w, x->off);
        rw_u32(w, x->n);
        x->last = c->frame;
        e->st.cache_hits++;
        e->st.cache_hit_bytes[cat] += n;
        return 1;
    }
    const uint32_t off = cache_alloc(c, &e->st, (uint32_t)n, 0);
    if (off == UINT32_MAX) { e->st.cache_refused++; return 0; }
    i = cache_slot_for(c, h);
    c->nused++;
    CEnt *x = &c->ent[i];
    x->h = h;
    x->h2 = h2;
    x->n = (uint32_t)n;
    x->off = off;
    x->last = c->frame;
    q_push(c, i);
    rw_u32(w, RW_CACHE_NEW);
    rw_u32(w, off);
    rw_u32(w, (uint32_t)n);
    pad(w);
    put(w, p, n);
    pad(w);
    w->cat[cat] += n;
    e->st.cache_new++;
    e->st.cache_new_bytes += n;
    return 1;
}

void rw_blob_plain(RwWriter *w, const void *p, size_t n) { whole_bytes(w, p, n, RW_CAT_ARGS); }
void rw_blob(RwWriter *w, const void *p, size_t n, int present, int cat) {
    if (!present) { rw_u32(w, RW_NULL); return; }
    RwEnc *e = w->enc;
    if (!e || n < RW_REF_MIN) { whole_bytes(w, p, n, cat); return; }
    if (e->cache) {   /* the content cache replaces the in-frame references */
        if (w->no_refs || !cache_put(w, e, p, n, cat)) {
            whole_bytes(w, p, n, cat);
            e->npay++;   /* numbered as the reader numbers it (the programs' references count on it) */
            if (e->whole) e->whole(p, n, cat);
        }
        return;
    }
    Ref *r = ref_slot(e->pay, e->gen, p, n, 0);
    const int hit = r && r->gen == e->gen;
    if (hit && !w->bad && !memcmp(w->buf + r->off, p, n)) {
        if (!w->no_refs) {
            rw_u32(w, RW_REF);
            rw_u32(w, r->idx);
            e->st.ref_bytes[cat] += n;
            e->st.refs++;
            return;
        }
    } else if (hit) e->st.stale++;
    rw_u32(w, (uint32_t)n);
    pad(w);
    const size_t off = w->n;
    put(w, p, n);
    pad(w);
    if (cat != RW_CAT_ARGS) w->cat[cat] += n;
    const uint32_t idx = e->npay++;
    if (r && (hit || e->used * 2 < HASH_N)) {
        if (!hit) e->used++;
        *r = (Ref){p, n, off, idx, e->gen, e->wgen};
    }
    if (e->whole) e->whole(p, n, cat);
}
void rw_put_skin_prog(RwWriter *w, const void *ins, size_t n) {
    if (!ins) { rw_u32(w, RW_NULL); return; }
    RwEnc *e = w->enc;
    if (!e) { whole_bytes(w, ins, n, RW_CAT_SKIN_PROG); return; }
    Ref *r = ref_slot(e->prog, e->gen, ins, 0, 1);
    if (r && r->gen == e->gen && !w->no_refs) {
        if (r->n != n || (r->wgen == e->wgen && !w->bad && memcmp(w->buf + r->off, ins, n))) e->st.prog_stale++;
        rw_u32(w, RW_REF);
        rw_u32(w, r->idx);
        e->st.ref_bytes[RW_CAT_SKIN_PROG] += n;
        e->st.prog_refs++;
        return;
    }
    rw_u32(w, (uint32_t)n);
    pad(w);
    const size_t off = w->n;
    put(w, ins, n);
    pad(w);
    w->cat[RW_CAT_SKIN_PROG] += n;
    const uint32_t idx = e->npay++;
    if (r && r->gen != e->gen && e->prog_used * 2 < HASH_N) {
        e->prog_used++;
        *r = (Ref){ins, n, off, idx, e->gen, e->wgen};
    }
}
/* Two 16-byte registers equal (no memcmp call per register). */
#if defined(__SSE2__)
static int reg_same(const uint8_t *x, const uint8_t *y) {
    const __m128i a = _mm_loadu_si128((const __m128i *)(const void *)x), b = _mm_loadu_si128((const __m128i *)(const void *)y);
    return _mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) == 0xFFFF;
}
#else
static int reg_same(const uint8_t *x, const uint8_t *y) {
    uint64_t a[2], b[2];
    memcpy(a, x, 16);
    memcpy(b, y, 16);
    return !((a[0] ^ b[0]) | (a[1] ^ b[1]));
}
#endif
/* mode 0 NULL, 1 the last draw's pointer + delta, 2 a new block + delta, 3 the whole block (no state) */
void rw_put_skin_consts(RwWriter *w, const float (*c)[4]) {
    if (!c) { rw_u32(w, 0); return; }
    RwEnc *e = w->enc;
    if (!e) {
        rw_u32(w, 3);
        whole_bytes(w, c, 192 * 16, RW_CAT_SKIN_CONSTS);
        return;
    }
    const uint8_t *b = (const uint8_t *)c;
    uint32_t mask[6] = {0}, k = 0;
    /* the registers that differ from the reader's (= the last block's bytes, e->last_consts): the hash-ahead
     * worker's delta when it has one for exactly these two blocks, else compared here */
    int known = e->last_consts && e->known_delta && e->known_delta(c, e->last_consts, mask);
    if (known && e->cache && e->cache->verify) {   /* --renderer-cache check: the worker's delta against the bytes */
        uint32_t m2[6] = {0};
        for (uint32_t i = 0; i < 192; i++)
            if (!reg_same(e->consts + i * 16, b + i * 16)) m2[i >> 5] |= 1u << (i & 31);
        if (memcmp(m2, mask, sizeof m2)) {
            e->st.consts_delta_wrong++;
            memcpy(mask, m2, sizeof m2);
        }
    }
    if (known) {
        e->st.consts_delta_known++;
        for (int i = 0; i < 6; i++) k += (uint32_t)__builtin_popcount(mask[i]);
    } else
        for (uint32_t i = 0; i < 192; i++)
            if (!reg_same(e->consts + i * 16, b + i * 16)) mask[i >> 5] |= 1u << (i & 31), k++;
    const int same = (const void *)c == e->last_consts;
    rw_u32(w, same ? 1 : 2);
    for (int i = 0; i < 6; i++) rw_u32(w, mask[i]);
    for (uint32_t i = 0; i < 192; i++)
        if (mask[i >> 5] >> (i & 31) & 1) {
            put(w, b + i * 16, 16);
            memcpy(e->consts + i * 16, b + i * 16, 16);
        }
    pad(w);
    w->cat[RW_CAT_SKIN_CONSTS] += (uint64_t)k * 16;
    e->last_consts = c;
    e->st.consts_draws++;
    e->st.consts_same_ptr += same;
    e->st.consts_regs += k;
    e->st.consts_saved += (192 - k) * 16;
}
/* mode 0 NULL, 1 the same bytes as the last image sent, 2 the image */
void rw_put_base(RwWriter *w, const void *base, size_t n) {
    if (!base) { rw_u32(w, 0); return; }
    RwEnc *e = w->enc;
    /* the hash-ahead worker's hash of the image (rw_enc_hash_hook), compared with the last one's instead of the bytes */
    uint64_t h = 0;
    const int hk = e && e->known_hash && e->known_hash(base, n, &h);
    if (e && e->have_base && n == e->base_n) {
        int same;
        if (hk && e->base_h_ok) {
            same = h == e->base_h;
            if (e->cache && e->cache->verify && same != !memcmp(e->base, base, n)) {   /* --renderer-cache check */
                e->st.cache_collisions++;
                same = 0;
            }
        } else same = !memcmp(e->base, base, n);
        if (same) {
            rw_u32(w, 1);
            e->st.base_same++;
            e->st.base_saved += n;
            e->base_h = h;
            e->base_h_ok = hk;
            return;
        }
    }
    rw_u32(w, 2);
    whole_bytes(w, base, n, RW_CAT_BASE);
    if (!e) return;
    e->base_h = h;
    e->base_h_ok = hk;
    e->st.base_sent++;
    if (n > e->base_cap) {
        uint8_t *c = realloc(e->base, n);
        if (!c) { e->have_base = 0; return; }
        e->base = c;
        e->base_cap = n;
    }
    memcpy(e->base, base, n);
    e->base_n = n;
    e->have_base = 1;
}
void rw_put_filmic(RwWriter *w, const RFilmic *f) {
    rw_u32(w, f != NULL);
    if (!f) return;
    const float v[9] = {f->exposure, f->white, f->norm, f->blend, f->hdr, f->lut_mix, f->halation, f->grain, f->vignette};
    for (int i = 0; i < 9; i++) rw_f32(w, v[i]);
    rw_i32(w, f->lut_size);
    const size_t n = f->lut && f->lut_size > 0 ? (size_t)f->lut_size * f->lut_size * f->lut_size * 4 : 0;
    rw_blob(w, f->lut, n, f->lut != NULL, RW_CAT_ARGS);
}
void rw_put_map(RwWriter *w, const RMaterialMap *m) {
    rw_u32(w, m != NULL);
    if (!m) return;
    rw_i32(w, m->w);
    rw_i32(w, m->h);
    rw_f32(w, m->x0);
    rw_f32(w, m->y0);
    rw_f32(w, m->cell);
    rw_f32(w, m->z0);
    rw_f32(w, m->zstep);
    const size_t n = m->texels && m->w > 0 && m->h > 0 ? (size_t)m->w * m->h * 4 : 0;
    rw_blob(w, m->texels, n, m->texels != NULL, RW_CAT_MAPS);
}
void rw_put_skin_normals(RwWriter *w, const RSkinNormals *s, uint32_t nv) {
    rw_u32(w, s != NULL);
    if (!s) return;
    rw_u32(w, s->ng);
    rw_u32(w, s->ncorner);
    rw_f32(w, s->px);
    rw_f32(w, s->py);
    rw_blob(w, s->group, (size_t)nv * 4, s->group != NULL, RW_CAT_NORMALS);
    rw_blob(w, s->start, ((size_t)s->ng + 1) * 4, s->start != NULL, RW_CAT_NORMALS);
    rw_blob(w, s->corner, (size_t)s->ncorner * 4, s->corner != NULL, RW_CAT_NORMALS);
}

/* ---- reader ---- */
void rw_reader(RwReader *r, const void *payload, size_t n) {
    r->p = payload;
    r->end = r->p + n;
    r->rec = r->p - sizeof(RwRecordHeader);   /* the payload follows its header in the buffer */
    r->bad = 0;
    r->dec = NULL;
}
static int take(RwReader *r, void *out, size_t n) {
    if (r->bad || (size_t)(r->end - r->p) < n) { r->bad = 1; memset(out, 0, n); return 0; }
    memcpy(out, r->p, n);
    r->p += n;
    return 1;
}
static void rpad(RwReader *r) {
    const size_t k = (RW_ALIGN - (size_t)(r->p - r->rec) % RW_ALIGN) % RW_ALIGN;
    if ((size_t)(r->end - r->p) < k) { r->bad = 1; return; }
    r->p += k;
}
uint32_t rw_ru32(RwReader *r) { uint32_t v; take(r, &v, 4); return v; }
int32_t rw_ri32(RwReader *r) { int32_t v; take(r, &v, 4); return v; }
float rw_rf32(RwReader *r) { float v; take(r, &v, 4); return v; }
uint64_t rw_ru64(RwReader *r) { uint64_t v; take(r, &v, 8); return v; }
double rw_rf64(RwReader *r) { double v; take(r, &v, 8); return v; }
struct RwDec {
    struct { const void *p; size_t n; } *pay;
    uint32_t npay, cap;
    uint8_t consts[192 * 16];
    uint8_t *last_consts;
    uint8_t *pool[64];   /* blocks of the constants handed out this chunk (1 MB each, kept) */
    size_t pool_used;
    int pool_n, pool_cur;
    uint8_t *base;
    size_t base_n, base_cap;
    int have_base;
    uint8_t *arena;     /* the content cache's arena */
    uint32_t arena_n;
    int arena_own, copy_progs;
};
RwDec *rw_dec_new(void) { return calloc(1, sizeof(RwDec)); }
void rw_dec_arena(RwDec *d, void *arena, uint32_t n) {
    if (d->arena_own) free(d->arena);
    d->arena = arena;
    d->arena_n = arena ? n : 0;
    d->arena_own = 0;
}
const void *rw_dec_arena_base(RwDec *d, uint32_t *n) {
    if (n) *n = d->arena_n;
    return d->arena;
}
void rw_dec_copy_programs(RwDec *d, int on) { d->copy_progs = on; }
void rw_dec_free(RwDec *d) {
    if (!d) return;
    if (d->arena_own) free(d->arena);
    free(d->pay);
    for (int i = 0; i < d->pool_n; i++) free(d->pool[i]);
    free(d->base);
    free(d);
}
void rw_dec_frame(RwDec *d) {
    d->npay = 0;
    d->last_consts = NULL;
    d->pool_cur = 0;
    d->pool_used = 0;
}
void rw_dec_stream(RwDec *d) {
    rw_dec_frame(d);
    memset(d->consts, 0, sizeof d->consts);
    d->have_base = 0;
}
static void dec_add(RwDec *d, const void *p, size_t n) {
    if (d->npay == d->cap) {
        const uint32_t cap = d->cap ? d->cap * 2 : 4096;
        void *b = realloc(d->pay, cap * sizeof *d->pay);
        if (!b) return;   /* the next reference to it reads as bad */
        d->pay = b;
        d->cap = cap;
    }
    d->pay[d->npay].p = p;
    d->pay[d->npay].n = n;
    d->npay++;
}
#define POOL_BLOCK (1u << 20)
static uint8_t *pool_take(RwDec *d, size_t n) {
    if (d->pool_cur < d->pool_n && d->pool_used + n > POOL_BLOCK) d->pool_cur++, d->pool_used = 0;
    if (d->pool_cur == d->pool_n) {
        if (d->pool_n == 64 || !(d->pool[d->pool_n] = malloc(POOL_BLOCK))) return NULL;
        d->pool_n++;
        d->pool_used = 0;
    }
    uint8_t *p = d->pool[d->pool_cur] + d->pool_used;
    d->pool_used += n;
    return p;
}
static const void *whole_read(RwReader *r, uint32_t len, size_t *n) {
    rpad(r);
    if (r->bad || (size_t)(r->end - r->p) < len) { r->bad = 1; return NULL; }
    const void *p = r->p;
    r->p += len;
    rpad(r);
    if (n) *n = len;
    return p;
}
static const void *ref_read(RwReader *r, size_t *n) {
    const uint32_t idx = rw_ru32(r);
    if (r->bad || !r->dec || idx >= r->dec->npay || !r->dec->pay[idx].p) { r->bad = 1; return NULL; }
    if (n) *n = r->dec->pay[idx].n;
    return r->dec->pay[idx].p;
}
/* RW_CACHED / RW_CACHE_NEW / RW_CACHE_MOVE (render_wire.h): the content's place in the arena. */
static const void *cache_read(RwReader *r, uint32_t kind, size_t *n) {
    RwDec *d = r->dec;
    const uint32_t a = rw_ru32(r), b = rw_ru32(r);
    const uint32_t from = a, to = kind == RW_CACHE_MOVE ? b : a, len = kind == RW_CACHE_MOVE ? rw_ru32(r) : b;
    if (r->bad || !d) { r->bad = 1; return NULL; }
    if (!d->arena && kind == RW_CACHE_NEW) {
        d->arena = malloc(RW_CACHE_BYTES);
        d->arena_n = d->arena ? RW_CACHE_BYTES : 0;
        d->arena_own = 1;
    }
    if ((uint64_t)to + len > d->arena_n || (uint64_t)from + len > d->arena_n) { r->bad = 1; return NULL; }
    if (kind == RW_CACHE_NEW) {
        const void *p = whole_read(r, len, NULL);
        if (!p) return NULL;
        memcpy(d->arena + to, p, len);
    } else if (kind == RW_CACHE_MOVE)
        memmove(d->arena + to, d->arena + from, len);
    if (n) *n = len;
    return d->arena + to;
}
const void *rw_rblob(RwReader *r, size_t *n) {
    const uint32_t len = rw_ru32(r);
    if (n) *n = 0;
    if (r->bad || len == RW_NULL) return NULL;
    if (len == RW_REF) return ref_read(r, n);
    if (len == RW_CACHED || len == RW_CACHE_NEW || len == RW_CACHE_MOVE) return cache_read(r, len, n);
    const void *p = whole_read(r, len, n);
    if (p && r->dec && len >= RW_REF_MIN) dec_add(r->dec, p, len);
    return p;
}
const void *rw_rblob_plain(RwReader *r, size_t *n) {
    const uint32_t len = rw_ru32(r);
    if (n) *n = 0;
    if (r->bad || len == RW_NULL) return NULL;
    if (len == RW_REF) { r->bad = 1; return NULL; }
    return whole_read(r, len, n);
}
const void *rw_get_skin_prog(RwReader *r) {
    const uint32_t len = rw_ru32(r);
    if (r->bad || len == RW_NULL) return NULL;
    if (len == RW_REF) return ref_read(r, NULL);
    const void *p = whole_read(r, len, NULL);
    if (p && r->dec && r->dec->copy_progs) {   /* an address of its own for the chunk */
        uint8_t *c = pool_take(r->dec, (len + 15) & ~15u);
        if (!c) { r->bad = 1; return NULL; }
        memcpy(c, p, len);
        p = c;
    }
    if (p && r->dec) dec_add(r->dec, p, len);
    return p;
}
const void *rw_get_skin_consts(RwReader *r) {
    const uint32_t mode = rw_ru32(r);
    if (r->bad || mode == 0) return NULL;
    if (mode == 3) {
        size_t n = 0;
        const void *p = rw_rblob_plain(r, &n);
        if (p && n != 192 * 16) r->bad = 1;
        return r->bad ? NULL : p;
    }
    RwDec *d = r->dec;
    if (!d || mode > 2) { r->bad = 1; return NULL; }
    uint32_t mask[6];
    for (int i = 0; i < 6; i++) mask[i] = rw_ru32(r);
    for (uint32_t i = 0; i < 192; i++)
        if (mask[i >> 5] >> (i & 31) & 1) take(r, d->consts + i * 16, 16);
    rpad(r);
    if (r->bad) return NULL;
    uint8_t *b = mode == 1 && d->last_consts ? d->last_consts : pool_take(d, 192 * 16);
    if (!b) { r->bad = 1; return NULL; }
    memcpy(b, d->consts, 192 * 16);   /* mode 1: the block the last draw had, rewritten in place */
    d->last_consts = b;
    return b;
}
const void *rw_get_base(RwReader *r) {
    const uint32_t mode = rw_ru32(r);
    if (r->bad || mode == 0) return NULL;
    RwDec *d = r->dec;
    if (mode == 1) {
        if (!d || !d->have_base) { r->bad = 1; return NULL; }
        return d->base;
    }
    if (mode != 2) { r->bad = 1; return NULL; }
    size_t n = 0;
    const void *p = rw_rblob_plain(r, &n);
    if (!p || !d) return p;
    if (n > d->base_cap) {
        uint8_t *c = realloc(d->base, n);
        if (!c) { d->have_base = 0; return p; }
        d->base = c;
        d->base_cap = n;
    }
    memcpy(d->base, p, n);
    d->base_n = n;
    d->have_base = 1;
    return p;
}
const RFilmic *rw_get_filmic(RwReader *r) {
    static RFilmic f;
    if (!rw_ru32(r)) return NULL;
    float v[9];
    for (int i = 0; i < 9; i++) v[i] = rw_rf32(r);
    f.exposure = v[0], f.white = v[1], f.norm = v[2], f.blend = v[3], f.hdr = v[4];
    f.lut_mix = v[5], f.halation = v[6], f.grain = v[7], f.vignette = v[8];
    f.lut_size = rw_ri32(r);
    f.lut = rw_rblob(r, NULL);
    return r->bad ? NULL : &f;
}
int rw_get_map(RwReader *r, RMaterialMap *m) {
    memset(m, 0, sizeof *m);
    if (!rw_ru32(r)) return 0;
    m->w = rw_ri32(r);
    m->h = rw_ri32(r);
    m->x0 = rw_rf32(r);
    m->y0 = rw_rf32(r);
    m->cell = rw_rf32(r);
    m->z0 = rw_rf32(r);
    m->zstep = rw_rf32(r);
    m->texels = rw_rblob(r, NULL);
    return !r->bad;
}
int rw_get_skin_normals(RwReader *r, RSkinNormals *s) {
    memset(s, 0, sizeof *s);
    if (!rw_ru32(r)) return 0;
    s->ng = rw_ru32(r);
    s->ncorner = rw_ru32(r);
    s->px = rw_rf32(r);
    s->py = rw_rf32(r);
    size_t ng = 0, ns = 0, nc = 0;
    s->group = rw_rblob(r, &ng);
    s->start = rw_rblob(r, &ns);
    s->corner = rw_rblob(r, &nc);
    if (r->bad) return 0;
    /* Tables that disagree with their counts (the renderer copies ng + 1 and ncorner words, and the GPU
     * walks them): no normals, the draw goes unlit. */
    if (!s->group || !ng || ng % 4 || !s->start || ns != ((size_t)s->ng + 1) * 4 || (s->ncorner && !s->corner) || nc != (size_t)s->ncorner * 4) {
        memset(s, 0, sizeof *s);
        return 0;
    }
    return 1;
}

/* ---- draw-state slots ---- */
static const uint32_t ds_ids[RW_DS_SLOTS] = {
    RW_ID_SET_STAGE_TEXTURE, RW_ID_SET_STAGE_TEXTURE, RW_ID_SET_STAGE_TEXTURE, RW_ID_SET_STAGE_TEXTURE,
    RW_ID_SET_PIXEL_SHADER, RW_ID_SET_ALPHA_TEST, RW_ID_SET_STATE, RW_ID_SET_TARGET, RW_ID_SET_EMISSION,
    RW_ID_SET_RELIEF, RW_ID_SET_HAIR, RW_ID_SET_SKIN, RW_ID_SET_EYE_OPEN,
};
_Static_assert(RENDERER_STAGES == 4, "one draw-state slot per texture stage");
int rw_draw_state_slot(uint32_t id, uint32_t first_arg) {
    if (id == RW_ID_SET_STAGE_TEXTURE) return first_arg < RENDERER_STAGES ? (int)first_arg : -1;
    for (int i = RENDERER_STAGES; i < RW_DS_SLOTS; i++)
        if (ds_ids[i] == id) return i;
    return -1;
}
uint32_t rw_draw_state_id(int slot) { return slot >= 0 && slot < RW_DS_SLOTS ? ds_ids[slot] : 0; }

/* ---- file header, sizes ---- */
void rw_file_header(RwFileHeader *h) {
    static const size_t sizes[RW_STRUCT_COUNT] = RW_STRUCT_SIZES;
    memset(h, 0, sizeof *h);
    memcpy(h->magic, RW_MAGIC, 8);
    h->schema = RW_SCHEMA;
    h->ptr_bytes = (uint32_t)sizeof(void *);
    h->nstructs = RW_STRUCT_COUNT;
    for (int i = 0; i < RW_STRUCT_COUNT; i++) h->sizes[i] = (uint32_t)sizes[i];
}
int rw_check_header(const RwFileHeader *h, char *why, size_t n) {
    RwFileHeader me;
    rw_file_header(&me);
    if (memcmp(h->magic, RW_MAGIC, 8)) { snprintf(why, n, "not a renderer recording"); return -1; }
    if (h->schema != me.schema) {
        snprintf(why, n, "schema %08x, this build reads %08x (renderer.h changed: record again)", h->schema, me.schema);
        return -1;
    }
    if (h->nstructs != me.nstructs || memcmp(h->sizes, me.sizes, sizeof me.sizes)) {
        snprintf(why, n, "struct sizes differ between the writer and this build");
        return -1;
    }
    return 0;
}
size_t rw_texture_bytes(uint32_t format, uint32_t w, uint32_t h, uint32_t levels) {
    size_t n = 0;
    for (uint32_t l = 0; l < levels && l < 32; l++) {
        const size_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        switch (format) {
        case RENDERER_TEX_BGRA8: n += lw * lh * 4; break;
        case RENDERER_TEX_L8: n += lw * lh; break;
        case RENDERER_TEX_BC1: n += ((lw + 3) / 4) * ((lh + 3) / 4) * 8; break;
        default: n += ((lw + 3) / 4) * ((lh + 3) / 4) * 16; break;   /* BC2, BC3, BC7 */
        }
    }
    return n;
}
size_t rw_str_bytes(const char *s) { return s ? strlen(s) + 1 : 0; }
uint64_t rw_frame_hash(const uint32_t *px, int w, int h, int pitch_bytes) {
    uint64_t a = 0x9E3779B97F4A7C15ull ^ ((uint64_t)w << 32 | (uint32_t)h), b = 0xC2B2AE3D27D4EB4Full;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)px + (size_t)y * pitch_bytes);
        for (int x = 0; x + 1 < w; x += 2) {
            a = (a ^ (row[x] | (uint64_t)row[x + 1] << 32)) * 0x100000001B3ull;
            b += a >> 29;
        }
        if (w & 1) a = (a ^ row[w - 1]) * 0x100000001B3ull;
    }
    return a ^ (b * 0x9E3779B97F4A7C15ull);
}
