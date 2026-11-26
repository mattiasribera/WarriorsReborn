/* Wire format of the renderer call stream (src/render_wire.h): every codec round-trips, payloads
 * are aligned, short records are detected, the header refuses another schema. Built for i386
 * (build/render_wire_test) and x86_64 (build64/render_wire_test); `--write FILE` in one and
 * `--read FILE` in the other checks the same records across the two ABIs. */
#include "../src/render_wire.h"
#include "../src/render_wire_gen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int fake_surface(void *instance, uint64_t *surface) { (void)instance; (void)surface; return 0; }

/* Fixed content of every record kind. */
static uint8_t lut[2 * 2 * 2 * 4];
static uint16_t texels[3 * 2 * 2];
static uint32_t group[5] = {0, 0, 1, 1, 2}, start[4] = {0, 2, 4, 5}, corner[5] = {4, 3, 2, 1, 0};
static RVertex verts[3];
static RFilmic filmic;
static RAutoExposure ae;
static RDof dof;
static RendererConfig cfg;
static RFrameGen fg;
static void fill(void) {
    for (size_t i = 0; i < sizeof lut; i++) lut[i] = (uint8_t)(i * 7 + 1);
    for (size_t i = 0; i < sizeof texels / sizeof *texels; i++) texels[i] = (uint16_t)(0x1000 * (i % 4) + i);
    for (int i = 0; i < 3; i++) {
        verts[i].x = (float)i;
        verts[i].w = 1;
        verts[i].color = 0xFF000000u | (uint32_t)i;
        verts[i].tex[3][3] = 0.25f * i;
        verts[i].fog = 0.5f;
    }
    filmic = (RFilmic){1.1f, 2.2f, 3.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, lut, 2};
    ae = (RAutoExposure){-2.5f, 0.7f, -1, 2, 0.5f, 1.5f, 3, 0.8f, 1};
    dof = (RDof){0.01f, 0.5f, 3, 0.25f, 0.5f, 0.2f, 0.3f, 0.1f, 0.5f, 2};
    for (int i = 0; i < 12; i++) fg.reproject[i / 4][i % 4] = 0.125f * (float)(i + 1);
    fg.depth_a = 1.0f, fg.depth_b = 0.05f, fg.vp[2] = 1440, fg.vp[3] = 1080, fg.valid = 1, fg.reset = 0;
    /* every scalar field a distinct value: the struct as bytes 1, 2, 3, ..., then the pointers */
    uint8_t *b = (uint8_t *)&cfg;
    for (size_t i = 0; i < sizeof cfg; i++) b[i] = (uint8_t)((i * 13 + 5) & 0x3f);   /* no NaN floats */
    static const char *const ext[1] = {"VK_KHR_surface"};
    cfg.instance_ext = ext;
    cfg.instance_ext_count = 1;
    cfg.create_surface = fake_surface;
    cfg.drawable_size = NULL;
    cfg.filmic = &filmic;
    cfg.auto_exposure = &ae;
    cfg.dof = &dof;
    cfg.pipeline_cache_dir = "/tmp/pipelines";
    cfg.taa_upscaler = 1;   /* --upscaler fsr3 */
    cfg.frame_gen = 1;
    cfg.taa_lite = 2;   /* --taa-lite off (TAA_LITE_OFF) */
    cfg.vrs = 3;        /* --vrs performance (RENDERER_VRS_PERFORMANCE) */
}

static void write_records(RwWriter *w) {
    rw_begin_record(w, 7, 0);
    rw_u32(w, 0xDEADBEEFu);
    rw_i32(w, -42);
    rw_f32(w, 1.5f);
    rw_u64(w, 0x0123456789ABCDEFull);
    rw_f64(w, 123456789.0625);   /* renderer_begin's frame time */
    rw_blob(w, NULL, 0, 0, RW_CAT_ARGS);
    rw_blob(w, "", 0, 1, RW_CAT_ARGS);
    rw_blob(w, "abcde", 5, 1, RW_CAT_ARGS);
    rw_blob(w, verts, sizeof verts, 1, RW_CAT_VERTS);
    rw_blob(w, &fg, sizeof fg, 1, RW_CAT_ARGS);   /* renderer_frame_gen's RFrameGen */
    rw_u32(w, 99);
    rw_end_record(w);
    rw_begin_record(w, 8, RW_FLAG_PREAMBLE);
    rw_put_config(w, &cfg);
    rw_put_config(w, NULL);
    rw_end_record(w);
    rw_begin_record(w, 9, 0);
    const RMaterialMap m = {3, 2, 1.0f, 2.0f, 0.5f, -1.0f, 0.25f, texels};
    rw_put_map(w, &m);
    rw_put_map(w, NULL);
    const RSkinNormals n = {group, start, corner, 3, 5, 1.25f, 0.75f};
    rw_put_skin_normals(w, &n, 5);
    rw_put_skin_normals(w, NULL, 5);
    rw_end_record(w);
}

static const uint8_t *next(const uint8_t *p, const uint8_t *end, RwRecordHeader *h, RwReader *r);
/* The stream state (render_wire.h): records 10-12 written with an encoder. */
static uint8_t pa[128], pb[32], prog[48], img[256];
static float c1[192][4], c2[192][4];
static RwEncStats stream_st;
static void write_stream(RwWriter *w) {
    for (size_t i = 0; i < sizeof pa; i++) pa[i] = (uint8_t)(i * 3 + 1);
    for (size_t i = 0; i < sizeof pb; i++) pb[i] = (uint8_t)(i + 100);
    for (size_t i = 0; i < sizeof prog; i++) prog[i] = (uint8_t)(i ^ 0x5a);
    for (size_t i = 0; i < sizeof img; i++) img[i] = (uint8_t)(255 - i);
    memset(c1, 0, sizeof c1);
    for (int i = 0; i < 10; i++) c1[i * 7][i & 3] = 0.5f + (float)i;   /* 10 registers not zero */
    RwEnc *e = rw_enc_new();
    w->enc = e;
    rw_enc_stream(e);
    rw_enc_frame(e);
    rw_begin_record(w, 10, 0);
    rw_blob(w, pa, sizeof pa, 1, RW_CAT_VERTS);    /* whole, index 0 */
    rw_blob(w, pa, sizeof pa, 1, RW_CAT_VERTS);    /* the same: a reference */
    rw_blob(w, pb, sizeof pb, 1, RW_CAT_INDICES);  /* under RW_REF_MIN: whole, not indexed */
    rw_blob(w, pb, sizeof pb, 1, RW_CAT_INDICES);
    pa[0] ^= 0xFF;
    rw_blob(w, pa, sizeof pa, 1, RW_CAT_VERTS);    /* same pointer and size, other bytes: whole, index 1 */
    pa[0] ^= 0xFF;
    rw_put_skin_prog(w, prog, sizeof prog);        /* whole, index 2 */
    prog[0] ^= 1;
    rw_put_skin_prog(w, prog, sizeof prog);        /* the same pointer: a reference whatever the bytes */
    prog[0] ^= 1;
    rw_put_skin_consts(w, c1);                     /* 10 registers */
    c1[1][0] = 9, c1[191][3] = -1;
    rw_put_skin_consts(w, c1);                     /* the last pointer, 2 registers */
    memcpy(c2, c1, sizeof c1);
    rw_put_skin_consts(w, c2);                     /* another pointer, nothing changed */
    rw_put_base(w, img, sizeof img);               /* whole */
    rw_put_base(w, img, sizeof img);               /* unchanged */
    rw_put_skin_consts(w, NULL);
    rw_put_base(w, NULL, 0);
    rw_end_record(w);
    w->no_refs = 1;
    rw_begin_record(w, 11, 0);
    rw_blob(w, pa, sizeof pa, 1, RW_CAT_VERTS);    /* no references in this record: whole, index 3 */
    rw_end_record(w);
    w->no_refs = 0;
    rw_enc_frame(e);
    rw_begin_record(w, 12, 0);
    rw_blob(w, pa, sizeof pa, 1, RW_CAT_VERTS);    /* a new chunk: whole again */
    rw_put_skin_consts(w, c1);                     /* deltas go on across chunks: nothing changed */
    rw_put_base(w, img, sizeof img);               /* and the base image too */
    img[5] ^= 1;
    rw_put_base(w, img, sizeof img);               /* changed: whole */
    img[5] ^= 1;
    rw_end_record(w);
    stream_st = *rw_enc_stats(e);
    w->enc = NULL;
    rw_enc_free(e);
}
static const uint8_t *read_stream(const uint8_t *p, const uint8_t *end) {
    RwRecordHeader h;
    RwReader r;
    RwDec *d = rw_dec_new();
    rw_dec_stream(d);
    rw_dec_frame(d);
    const uint8_t *rec10 = p;
    p = next(p, end, &h, &r);
    r.dec = d;
    CHECK(p && h.id == 10, "stream record 10");
    size_t n = 0, n2 = 0;
    const uint8_t *a1 = rw_rblob(&r, &n), *a2 = rw_rblob(&r, &n2);
    CHECK(a1 && a1 == a2 && n == sizeof pa && n2 == n && !memcmp(a1, pa, sizeof pa), "a payload passed again: the earlier one's pointer and bytes");
    const uint8_t *b1 = rw_rblob(&r, NULL), *b2 = rw_rblob(&r, NULL);
    CHECK(b1 && b2 && b1 != b2 && !memcmp(b2, pb, sizeof pb), "a payload under %u bytes: sent whole each time", RW_REF_MIN);
    const uint8_t *a3 = rw_rblob(&r, &n);
    CHECK(a3 && a3 != a1 && a3[0] == (uint8_t)(pa[0] ^ 0xFF) && !memcmp(a3 + 1, pa + 1, sizeof pa - 1), "same pointer, other bytes: sent whole");
    const uint8_t *p1 = rw_get_skin_prog(&r), *p2 = rw_get_skin_prog(&r);
    CHECK(p1 && p1 == p2 && !memcmp(p1, prog, sizeof prog), "a skinning program by the same pointer: the first one's pointer (its bytes)");
    const float(*k1)[4] = rw_get_skin_consts(&r);
    float want[192][4] = {{0}};
    for (int i = 0; i < 10; i++) want[i * 7][i & 3] = 0.5f + (float)i;
    CHECK(k1 && !memcmp(k1, want, sizeof want), "constants: the first draw's registers");
    want[1][0] = 9, want[191][3] = -1;
    const float(*k2)[4] = rw_get_skin_consts(&r);
    CHECK(k2 == k1 && !memcmp(k2, want, sizeof want), "constants by the last draw's pointer: that block, rewritten in place");
    const float(*k3)[4] = rw_get_skin_consts(&r);
    CHECK(k3 && k3 != k1 && !memcmp(k3, want, sizeof want), "constants by another pointer: a new block with the same registers");
    const uint8_t *i1 = rw_get_base(&r), *i2 = rw_get_base(&r);
    CHECK(i1 && i2 && !memcmp(i1, img, sizeof img) && !memcmp(i2, img, sizeof img), "base image sent, then unchanged");
    CHECK(rw_get_skin_consts(&r) == NULL && rw_get_base(&r) == NULL && !r.bad && r.end - r.p < (long)RW_ALIGN, "NULL constants and base image");
    p = next(p, end, &h, &r);
    r.dec = d;
    const uint8_t *a4 = rw_rblob(&r, &n);
    CHECK(p && h.id == 11 && a4 && a4 != a1 && !memcmp(a4, pa, sizeof pa), "a record without references: whole");
    rw_dec_frame(d);
    p = next(p, end, &h, &r);
    r.dec = d;
    const uint8_t *a5 = rw_rblob(&r, &n);
    const float(*k4)[4] = rw_get_skin_consts(&r);
    const uint8_t *i3 = rw_get_base(&r);
    const int i3_ok = i3 && !memcmp(i3, img, sizeof img);   /* valid until the next base image is read */
    const uint8_t *i4 = rw_get_base(&r);
    CHECK(p && h.id == 12 && a5 && !memcmp(a5, pa, sizeof pa), "a new chunk: no references to the last one");
    CHECK(k4 && !memcmp(k4, want, sizeof want), "constants continue across chunks");
    CHECK(i3_ok && i4 && i4[5] == (uint8_t)(img[5] ^ 1), "base image unchanged across chunks, then changed");
    CHECK(!r.bad && r.end - r.p < (long)RW_ALIGN, "stream records fully read");
    /* without the stream state, a reference is an error */
    next(rec10, end, &h, &r);
    rw_rblob(&r, NULL);
    CHECK(rw_rblob(&r, NULL) == NULL && r.bad, "a reference read without the stream state: bad record");
    rw_dec_free(d);
    return p;
}

static const uint8_t *next(const uint8_t *p, const uint8_t *end, RwRecordHeader *h, RwReader *r) {
    if (end - p < (long)sizeof *h) return NULL;
    memcpy(h, p, sizeof *h);
    rw_reader(r, p + sizeof *h, h->len);
    return p + sizeof *h + h->len;
}

/* ---- the content hash and the content cache (W4) ---- */
static uint32_t rng_state = 12345;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void hash_tests(void) {
    static uint8_t buf[70000];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)rng();
    int same = 1;
    for (size_t n = 0; n <= 300; n++) same &= rw_hash64(buf + 3, n) == rw_hash64_scalar(buf + 3, n);
    for (size_t n = 1000; n < sizeof buf; n += 4093) same &= rw_hash64(buf + 1, n) == rw_hash64_scalar(buf + 1, n);
    CHECK(same, "content hash: the SSE2 and the scalar versions agree (0-300 bytes, up to 70 KB, unaligned)");
    int flips = 1;
    for (size_t n = 64; n < sizeof buf; n = n * 3 + 7) {
        const uint64_t h = rw_hash64(buf, n);
        for (size_t at = 0; at < n; at += n / 5 + 1) {
            buf[at] ^= 0x10;
            flips &= rw_hash64(buf, n) != h;
            buf[at] ^= 0x10;
        }
        flips &= rw_hash64(buf, n - 1) != h;
    }
    CHECK(flips, "content hash: one changed byte or one byte less changes it");
    uint8_t pin[256];
    for (int i = 0; i < 256; i++) pin[i] = (uint8_t)(i * 31 + 7);
    const uint64_t h = rw_hash64(pin, sizeof pin), h2 = rw_hash64(pin, 100);
    CHECK(h == 0x564e55f2343f5bbbull && h2 == 0x472f6e981fd47eaaull, "content hash: pinned values (the same in i386 and x86_64): %016llx %016llx",
          (unsigned long long)h, (unsigned long long)h2);
}
/* A stream of payloads through a small arena (1 MB): hits, moves, eviction, wrap-around, the per-chunk
 * budget, reused pointers with other bytes; every payload reads back with its bytes, and the pointers of
 * this chunk and the one before still hold theirs at the end of the chunk. */
#define POOL 400
static int hook_calls;
static int hook_every_other(const void *p, size_t n, uint64_t *h) {
    if (hook_calls++ % 2) return 0;
    *h = rw_hash64(p, n);
    return 1;
}
static void cache_tests(void) {
    static uint8_t *pool[POOL];
    static size_t size[POOL];
    for (int i = 0; i < POOL; i++) {
        size[i] = 64 + rng() % (i % 10 == 0 ? 40000 : 3000);
        pool[i] = malloc(size[i]);
        for (size_t k = 0; k < size[i]; k++) pool[i][k] = (uint8_t)rng();
    }
    RwEnc *e = rw_enc_new();
    RwDec *d = rw_dec_new();
    const uint32_t arena_n = 1u << 20;
    uint8_t *arena = malloc(arena_n);
    CHECK(e && d && arena && !rw_enc_cache(e, arena_n) && rw_enc_cache(e, 1000) == -1 && !rw_enc_cache(e, arena_n), "content cache on (1 MB arena)");
    rw_dec_arena(d, arena, arena_n);
    rw_enc_cache_verify(e, 1);
    rw_enc_stream(e);
    rw_dec_stream(d);
    RwWriter w = {0};
    w.enc = e;
    enum { PER = 60 };
    const void *got[2][PER];
    uint8_t *want[2][PER] = {{0}};
    size_t wn[2][PER];
    int ok = 1, held = 1, cur = 0;
    memset(got, 0, sizeof got);
    for (int f = 0; f < 300; f++) {
        rw_enc_frame(e);
        rw_dec_frame(d);
        rw_reset(&w);
        int idx[PER];
        for (int k = 0; k < PER; k++) {
            /* a working set that drifts: most payloads of the last frames again, some new ones */
            idx[k] = (int)((f / 3 + (rng() % 100 < 85 ? rng() % 40 : rng() % POOL)) % POOL);
            if (rng() % 50 == 0) pool[idx[k]][rng() % size[idx[k]]] ^= 0x5A;   /* same pointer, other bytes */
            if (f % 50 == 49) {   /* a burst of new large payloads: more than the chunk's budget */
                idx[k] = (k * 10) % POOL;
                if (k < POOL / 10) pool[idx[k]][0] ^= (uint8_t)f;
            }
            rw_begin_record(&w, 9, 0);
            rw_blob(&w, pool[idx[k]], size[idx[k]], 1, k % 2 ? RW_CAT_VERTS : RW_CAT_SKIN_DATA);
            rw_end_record(&w);
            free(want[cur][k]);
            want[cur][k] = malloc(size[idx[k]]);
            memcpy(want[cur][k], pool[idx[k]], size[idx[k]]);
            wn[cur][k] = size[idx[k]];
        }
        const uint8_t *q = w.buf, *end = w.buf + w.n;
        for (int k = 0; k < PER && q && q < end; k++) {
            RwRecordHeader h;
            RwReader r;
            q = next(q, end, &h, &r);
            if (!q) break;
            r.dec = d;
            size_t n = 0;
            got[cur][k] = rw_rblob(&r, &n);
            ok &= !r.bad && got[cur][k] && n == wn[cur][k] && !memcmp(got[cur][k], want[cur][k], n);
        }
        for (int k = 0; k < PER; k++) {
            /* whole payloads point into this test's writer buffer: only cached ones count */
            const uint8_t *g = got[cur][k];
            if (g && g >= arena && g < arena + arena_n) held &= !memcmp(g, want[cur][k], wn[cur][k]);
        }
        cur ^= 1;
    }
    const RwEncStats *st = rw_enc_stats(e);
    CHECK(ok, "content cache: 300 chunks x %d payloads read back with their bytes", PER);
    CHECK(held, "content cache: every cached payload of a chunk still holds its bytes at the chunk's end");
    CHECK(st->cache_hits > 5000 && st->cache_moves > 100 && st->cache_evicted > 500 && st->cache_refused > 0 && st->cache_new > 1000,
          "content cache: %llu hits, %llu moves, %llu new, %llu evicted, %llu sent whole (the chunk budget)", (unsigned long long)st->cache_hits,
          (unsigned long long)st->cache_moves, (unsigned long long)st->cache_new, (unsigned long long)st->cache_evicted,
          (unsigned long long)st->cache_refused);
    CHECK(st->cache_collisions == 0, "content cache: no hash collision (a second hash of every hit compared)");
    /* program references keep their numbering with the cache on: whole payloads of RW_REF_MIN bytes or
     * more (a texture, a kept record's) between a program and its reference */
    {
        RwEnc *e2 = rw_enc_new();
        RwDec *d2 = rw_dec_new();
        rw_enc_cache(e2, arena_n);
        rw_enc_stream(e2);
        rw_dec_arena(d2, arena, arena_n);
        rw_dec_stream(d2);
        RwWriter w2 = {0};
        w2.enc = e2;
        static uint8_t progs[2][64], tex[256];
        memset(progs[0], 1, 64), memset(progs[1], 2, 64), memset(tex, 3, sizeof tex);
        rw_begin_record(&w2, 9, 0);
        rw_put_skin_prog(&w2, progs[0], 64);
        rw_blob(&w2, tex, sizeof tex, 1, RW_CAT_TEXTURES);   /* whole, never cached */
        rw_put_skin_prog(&w2, progs[1], 64);
        w2.no_refs = 1;
        rw_blob(&w2, pool[3], size[3], 1, RW_CAT_ARGS);   /* a kept record's payload: whole */
        w2.no_refs = 0;
        rw_put_skin_prog(&w2, progs[1], 64);   /* references */
        rw_put_skin_prog(&w2, progs[0], 64);
        rw_end_record(&w2);
        RwRecordHeader h2;
        RwReader r2;
        next(w2.buf, w2.buf + w2.n, &h2, &r2);
        r2.dec = d2;
        const uint8_t *g0 = rw_get_skin_prog(&r2), *gt = rw_rblob(&r2, NULL), *g1 = rw_get_skin_prog(&r2);
        const uint8_t *gk = rw_rblob(&r2, NULL), *g1r = rw_get_skin_prog(&r2), *g0r = rw_get_skin_prog(&r2);
        CHECK(!r2.bad && g0 && gt && gk && g1 && g1r == g1 && g0r == g0 && g1[0] == 2 && g0[0] == 1,
              "content cache on: program references resolve to their own program after whole payloads");
        rw_free(&w2);
        rw_enc_free(e2);
        rw_dec_free(d2);
    }
    /* hashes from elsewhere (rw_enc_hash_hook, render_rec.c's hash-ahead worker): the same stream, byte for
     * byte, as hashing every payload, and those bytes are not hashed by the encoder */
    {
        RwEnc *ea = rw_enc_new(), *eb = rw_enc_new();
        rw_enc_cache(ea, arena_n);
        rw_enc_cache(eb, arena_n);
        rw_enc_hash_hook(eb, hook_every_other);
        rw_enc_stream(ea);
        rw_enc_stream(eb);
        RwWriter wa = {0}, wb = {0};
        wa.enc = ea;
        wb.enc = eb;
        int same = 1;
        for (int f = 0; f < 40; f++) {
            rw_enc_frame(ea);
            rw_enc_frame(eb);
            rw_reset(&wa);
            rw_reset(&wb);
            for (int k = 0; k < PER; k++) {
                const int i = (f + k * 7) % 80;
                rw_begin_record(&wa, 9, 0);
                rw_begin_record(&wb, 9, 0);
                rw_blob(&wa, pool[i], size[i], 1, RW_CAT_SKIN_DATA);
                rw_blob(&wb, pool[i], size[i], 1, RW_CAT_SKIN_DATA);
                rw_end_record(&wa);
                rw_end_record(&wb);
            }
            /* the base image: unchanged in most frames, compared by the hook's hash when it has one */
            if (f % 7 == 3) pool[90][f % size[90]] ^= 0x33;
            rw_begin_record(&wa, 9, 0);
            rw_begin_record(&wb, 9, 0);
            rw_put_base(&wa, pool[90], size[90]);
            rw_put_base(&wb, pool[90], size[90]);
            rw_end_record(&wa);
            rw_end_record(&wb);
            same &= wa.n == wb.n && !memcmp(wa.buf, wb.buf, wa.n);
        }
        const RwEncStats *sa = rw_enc_stats(ea), *sb = rw_enc_stats(eb);
        CHECK(sb->base_same == sa->base_same && sb->base_sent == sa->base_sent && sa->base_sent > 3 && sa->base_same > 20,
              "hashes from the hash hook: the base image unchanged in %llu and sent in %llu frames, as without it",
              (unsigned long long)sb->base_same, (unsigned long long)sb->base_sent);
        CHECK(same && sb->cache_prehashed > 0 && sb->cache_hashed + sb->cache_prehashed == sa->cache_hashed && sb->cache_hits == sa->cache_hits,
              "hashes from the hash hook: the same stream as hashing every payload (%llu of %llu bytes not hashed by the encoder)",
              (unsigned long long)sb->cache_prehashed, (unsigned long long)sa->cache_hashed);
        rw_free(&wa);
        rw_free(&wb);
        rw_enc_free(ea);
        rw_enc_free(eb);
    }
    /* a reader without the stream state refuses a cached payload */
    rw_reset(&w);
    rw_enc_frame(e);
    rw_begin_record(&w, 9, 0);
    rw_blob(&w, pool[1], size[1], 1, RW_CAT_INDICES);
    rw_end_record(&w);
    RwRecordHeader h;
    RwReader r;
    next(w.buf, w.buf + w.n, &h, &r);
    CHECK(rw_rblob(&r, NULL) == NULL && r.bad, "a cached payload read without the stream state: bad record");
    /* textures never use the cache; an offset past the arena is refused */
    rw_reset(&w);
    rw_begin_record(&w, 9, 0);
    rw_blob(&w, pool[2], size[2], 1, RW_CAT_TEXTURES);
    rw_u32(&w, RW_CACHED);
    rw_u32(&w, arena_n - 8);
    rw_u32(&w, 64);
    rw_end_record(&w);
    next(w.buf, w.buf + w.n, &h, &r);
    r.dec = d;
    size_t tn = 0;
    const void *t = rw_rblob(&r, &tn);
    CHECK(t && (const uint8_t *)t >= w.buf && (const uint8_t *)t < w.buf + w.n && tn == size[2], "a texture payload: whole, never cached");
    CHECK(rw_rblob(&r, NULL) == NULL && r.bad, "a cached payload past the arena's end: bad record");
    rw_free(&w);
    rw_enc_free(e);
    rw_dec_free(d);
    free(arena);
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < PER; k++) free(want[c][k]);
    for (int i = 0; i < POOL; i++) free(pool[i]);
}

/* The cache table at its entry limit (open addressing, two entries per bucket):
 * 400 k distinct small payloads in chunks of 8 k, each chunk also repeating the previous one's (hits), through
 * a 64 MB arena; the entry limit (2^17 - 1) evicts the oldest. Every payload reads back with its bytes, every
 * repeat is a hit, and no hit is a collision. */
static void cache_table_tests(void) {
    const uint32_t arena_n = 64u << 20;
    uint8_t *arena = malloc(arena_n);
    RwEnc *e = rw_enc_new();
    RwDec *d = rw_dec_new();
    CHECK(arena && e && d && !rw_enc_cache(e, arena_n), "content cache table: 64 MB arena");
    rw_dec_arena(d, arena, arena_n);
    rw_enc_cache_verify(e, 1);
    rw_enc_stream(e);
    rw_dec_stream(d);
    RwWriter w = {0};
    w.enc = e;
    enum { CHUNK = 8192, CHUNKS = 50 };
    int ok = 1;
    uint64_t repeats = 0;
    for (int f = 0; f < CHUNKS; f++) {
        rw_enc_frame(e);
        rw_dec_frame(d);
        rw_reset(&w);
        for (int pass = 0; pass < 2; pass++)   /* the previous chunk's payloads again, then this chunk's */
            for (uint32_t k = 0; k < CHUNK; k++) {
                if (!pass && !f) continue;
                uint32_t b[16];
                const uint32_t id = (uint32_t)(f - !pass) * CHUNK + k;
                for (int i = 0; i < 16; i++) b[i] = id * 2654435761u + (uint32_t)i;
                rw_begin_record(&w, 9, 0);
                rw_blob(&w, b, sizeof b, 1, RW_CAT_INDICES);
                rw_end_record(&w);
                repeats += !pass;
            }
        const uint8_t *q = w.buf, *end = w.buf + w.n;
        for (int pass = 0; pass < 2; pass++)
            for (uint32_t k = 0; k < CHUNK; k++) {
                if (!pass && !f) continue;
                RwRecordHeader h;
                RwReader r;
                q = next(q, end, &h, &r);
                if (!q) { ok = 0; break; }
                r.dec = d;
                size_t n = 0;
                const uint32_t *g = rw_rblob(&r, &n);
                const uint32_t id = (uint32_t)(f - !pass) * CHUNK + k;
                ok &= !r.bad && g && n == 64 && g[0] == id * 2654435761u && g[15] == id * 2654435761u + 15;
            }
    }
    const RwEncStats *st = rw_enc_stats(e);
    CHECK(ok, "content cache table: %d chunks of %d new + %d repeated payloads read back with their bytes", CHUNKS, CHUNK, CHUNK);
    CHECK(st->cache_hits + st->cache_moves == repeats && st->cache_new == (uint64_t)CHUNKS * CHUNK && st->cache_evicted > 200000 && st->cache_collisions == 0,
          "content cache table: every repeat a hit (%llu), %llu new, %llu evicted at the entry limit, no collision",
          (unsigned long long)st->cache_hits, (unsigned long long)st->cache_new, (unsigned long long)st->cache_evicted);
    rw_free(&w);
    rw_enc_free(e);
    rw_dec_free(d);
    free(arena);
}
/* Skinning-constant deltas from elsewhere (rw_enc_consts_hook, the hash-ahead worker): the same stream, byte
 * for byte, as comparing the registers; a wrong delta in check mode is corrected and counted. */
static float hk_blocks[6][192][4];
static int hk_wrong;
static int consts_hook(const void *cur, const void *prev, uint32_t mask[6]) {
    memset(mask, 0, 6 * sizeof *mask);
    const uint8_t *a = cur, *b = prev;
    for (int i = 0; i < 192; i++)
        if (memcmp(a + i * 16, b + i * 16, 16)) mask[i >> 5] |= 1u << (i & 31);
    if (hk_wrong) mask[0] ^= 1;
    return 1;
}
static void consts_hook_tests(void) {
    for (int b = 0; b < 6; b++)
        for (int i = 0; i < 192; i++)
            for (int c = 0; c < 4; c++) hk_blocks[b][i][c] = (float)((b * 7 + i % (b + 2)) * 4 + c);
    static const int order[] = {0, 1, 1, 2, 0, 3, 3, 3, 4, 5, 2, 5};
    uint8_t *out[3] = {0};
    size_t outn[3] = {0};
    uint64_t wrong[3] = {0}, known[3] = {0};
    for (int mode = 0; mode < 3; mode++) {   /* 0 compared here, 1 the hook, 2 a wrong hook in check mode */
        RwEnc *e = rw_enc_new();
        rw_enc_cache(e, 1u << 20);
        if (mode) rw_enc_consts_hook(e, consts_hook);
        rw_enc_cache_verify(e, mode == 2);
        hk_wrong = mode == 2;
        rw_enc_stream(e);
        RwWriter w = {0};
        w.enc = e;
        for (int f = 0; f < 2; f++) {
            rw_enc_frame(e);
            for (size_t k = 0; k < sizeof order / sizeof *order; k++) {
                rw_begin_record(&w, 9, 0);
                rw_put_skin_consts(&w, (const float (*)[4])hk_blocks[order[k]]);
                rw_end_record(&w);
            }
        }
        out[mode] = w.buf;
        outn[mode] = w.n;
        wrong[mode] = rw_enc_stats(e)->consts_delta_wrong;
        known[mode] = rw_enc_stats(e)->consts_delta_known;
        rw_enc_free(e);
    }
    CHECK(outn[1] == outn[0] && !memcmp(out[1], out[0], outn[0]) && known[1] == 22 && wrong[1] == 0,
          "skinning-constant deltas from the hook: the same stream as comparing (%llu of 24 draws; the chunks' first compare)",
          (unsigned long long)known[1]);
    CHECK(outn[2] == outn[0] && !memcmp(out[2], out[0], outn[0]) && wrong[2] == 22,
          "a wrong delta in check mode: corrected (the same stream), %llu counted", (unsigned long long)wrong[2]);
    for (int m = 0; m < 3; m++) free(out[m]);
}

static void read_records(const uint8_t *p, size_t bytes) {
    const uint8_t *end = p + bytes;
    RwRecordHeader h;
    RwReader r;
    CHECK(((uintptr_t)p & 7) == 0, "buffer 8-aligned");
    p = next(p, end, &h, &r);
    CHECK(p && h.id == 7 && h.flags == 0 && h.len % RW_ALIGN == 0, "record 1: id 7, length %u a multiple of 8", h.len);
    CHECK(rw_ru32(&r) == 0xDEADBEEFu && rw_ri32(&r) == -42 && rw_rf32(&r) == 1.5f && rw_ru64(&r) == 0x0123456789ABCDEFull,
          "scalars round-trip (u32, i32, f32, u64)");
    CHECK(rw_rf64(&r) == 123456789.0625, "double round-trips (renderer_begin's frame time)");
    size_t len = 7;
    CHECK(rw_rblob(&r, &len) == NULL && len == 0, "NULL payload");
    const void *e = rw_rblob(&r, &len);
    CHECK(e != NULL && len == 0, "empty payload is not NULL");
    const char *s5 = rw_rblob(&r, &len);
    CHECK(s5 && len == 5 && !memcmp(s5, "abcde", 5) && ((uintptr_t)s5 & 7) == 0, "5-byte payload, 8-aligned");
    const RVertex *v = rw_rblob(&r, &len);
    CHECK(v && len == sizeof verts && !memcmp(v, verts, sizeof verts) && ((uintptr_t)v & 7) == 0, "RVertex array payload, 8-aligned");
    const RFrameGen *g = rw_rblob(&r, &len);
    CHECK(g && len == sizeof fg && sizeof fg == 20 * 4 && !memcmp(g, &fg, sizeof fg) && g->reproject[2][3] == 1.5f && g->valid == 1,
          "RFrameGen payload (%zu bytes, the same layout in both ABIs)", sizeof fg);
    CHECK(rw_ru32(&r) == 99 && !r.bad && r.end - r.p < (long)RW_ALIGN, "scalar after the payloads, then only padding");
    r.p = r.end;
    rw_ru32(&r);
    CHECK(r.bad, "reading past the record's end is detected");

    p = next(p, end, &h, &r);
    CHECK(p && h.id == 8 && h.flags == RW_FLAG_PREAMBLE, "record 2: flags kept");
    RendererConfig d;
    int surf = 0;
    const uint8_t *cfg_at = r.p;   /* the writer's encoding (its own ABI's struct) */
    CHECK(rw_get_config(&r, &d, &surf) == 1 && surf == 1, "config decoded; the window callback becomes a flag");
    const size_t cfg_len = (size_t)(r.p - cfg_at);
    CHECK(d.create_surface == NULL && d.drawable_size == NULL && d.instance_ext == NULL && d.instance_ext_count == 0,
          "window callbacks and extensions are not carried");
    CHECK(d.pipeline_cache_dir && !strcmp(d.pipeline_cache_dir, "/tmp/pipelines"), "pipeline cache directory string");
    CHECK(d.auto_exposure && !memcmp(d.auto_exposure, &ae, sizeof ae) && d.dof && !memcmp(d.dof, &dof, sizeof dof),
          "auto exposure and depth of field structs");
    CHECK(d.filmic && d.filmic->exposure == 1.1f && d.filmic->vignette == 0.9f && d.filmic->lut_size == 2 && d.filmic->lut &&
              !memcmp(d.filmic->lut, lut, sizeof lut),
          "filmic parameters and LUT");
    CHECK(d.taa_lite == 2 && d.vrs == 3 && d.taa_upscaler == 1, "--taa-lite, --vrs and --upscaler travel in the configuration");
    /* every field: d encodes to the same bytes as cfg once the two things the format drops are put back */
    d.create_surface = fake_surface;
    d.instance_ext_count = 1;
    RwWriter re = {0};
    rw_put_config(&re, &d);
    CHECK(re.n == cfg_len && !memcmp(re.buf, cfg_at, re.n), "every field of RendererConfig round-trips (%zu bytes encoded)", re.n);
    rw_free(&re);
    CHECK(d.taa_upscaler == 1 && d.frame_gen == 1, "the upscaler choice (FSR 3.1) and frame generation travel");
    CHECK(rw_get_config(&r, &d, &surf) == 0 && !r.bad, "NULL config");

    p = next(p, end, &h, &r);
    RMaterialMap m;
    CHECK(p && rw_get_map(&r, &m) && m.w == 3 && m.h == 2 && m.cell == 0.5f && m.zstep == 0.25f && m.texels &&
              !memcmp(m.texels, texels, sizeof texels),
          "map round-trips (w x h x 2 texels)");
    CHECK(!rw_get_map(&r, &m) && m.texels == NULL && !r.bad, "NULL map");
    RSkinNormals n;
    CHECK(rw_get_skin_normals(&r, &n) && n.ng == 3 && n.ncorner == 5 && n.px == 1.25f && !memcmp(n.group, group, sizeof group) &&
              !memcmp(n.start, start, sizeof start) && !memcmp(n.corner, corner, sizeof corner),
          "GPU-skinning normals tables round-trip");
    CHECK(!rw_get_skin_normals(&r, &n) && !r.bad && r.end - r.p < (long)RW_ALIGN, "NULL normals, record fully read");
    p = read_stream(p, end);
    CHECK(p == end, "six records, nothing after them");
}

/* GPU-skinning normals tables whose sizes disagree with their counts (a start table of ng words
 * instead of ng + 1, a corner table one word short): no normals, the draw goes unlit, instead of the
 * renderer copying past the blobs into the tables the GPU walks. The
 * record is still read to its end. */
static void skin_normals_sizes(void) {
    for (int k = 0; k < 2; k++) {
        RwWriter w = {0};
        rw_u32(&w, 1);
        rw_u32(&w, 3);   /* ng */
        rw_u32(&w, 5);   /* ncorner */
        rw_f32(&w, 1);
        rw_f32(&w, 1);
        rw_blob(&w, group, sizeof group, 1, RW_CAT_NORMALS);
        rw_blob(&w, start, k == 0 ? 3 * 4 : sizeof start, 1, RW_CAT_NORMALS);
        rw_blob(&w, corner, k == 1 ? 4 * 4 : sizeof corner, 1, RW_CAT_NORMALS);
        RwReader r;
        rw_reader(&r, w.buf, w.n);
        RSkinNormals n;
        CHECK(!rw_get_skin_normals(&r, &n) && !r.bad && r.p == r.end && !n.group,
              "GPU-skinning normals: a %s table of the wrong size gives no normals, the record read to its end", k ? "corner" : "start");
        rw_free(&w);
    }
}

int main(int argc, char **argv) {
    fill();
    skin_normals_sizes();
    RwWriter w = {0};
    write_records(&w);
    const uint64_t verts_counted = w.cat[RW_CAT_VERTS];
    write_stream(&w);
    CHECK(!w.bad && w.n % RW_ALIGN == 0, "writer: %zu bytes, a multiple of 8", w.n);
    CHECK(stream_st.refs == 1 && stream_st.stale == 2 && stream_st.ref_bytes[RW_CAT_VERTS] == sizeof pa && stream_st.prog_refs == 1 &&
              stream_st.prog_stale == 1 && stream_st.consts_draws == 4 && stream_st.consts_regs == 12 && stream_st.consts_same_ptr == 1 &&
              stream_st.base_sent == 2 && stream_st.base_same == 2,
          "encoder: 1 reference, 2 whole by a known pointer with other bytes, 1 program reference, 12 of 4 x 192 registers sent, base image sent twice of 4 "
          "(%llu %llu %llu %llu %llu %llu %llu %llu %llu)", (unsigned long long)stream_st.refs, (unsigned long long)stream_st.stale,
          (unsigned long long)stream_st.prog_refs, (unsigned long long)stream_st.prog_stale, (unsigned long long)stream_st.consts_draws,
          (unsigned long long)stream_st.consts_regs, (unsigned long long)stream_st.consts_same_ptr, (unsigned long long)stream_st.base_sent,
          (unsigned long long)stream_st.base_same);
    w.cat[RW_CAT_VERTS] = verts_counted;
    CHECK(w.cat[RW_CAT_VERTS] == sizeof verts && w.cat[RW_CAT_MAPS] == sizeof texels && w.cat[RW_CAT_NORMALS] == sizeof group + sizeof start + sizeof corner,
          "payload bytes counted by category");
    if (argc == 3 && !strcmp(argv[1], "--write")) {
        FILE *f = fopen(argv[2], "wb");
        RwFileHeader h;
        rw_file_header(&h);
        const int ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(w.buf, 1, w.n, f) == w.n && !fclose(f);
        CHECK(ok, "wrote %s (%d-bit)", argv[2], (int)(sizeof(void *) * 8));
    } else if (argc == 3 && !strcmp(argv[1], "--read")) {
        FILE *f = fopen(argv[2], "rb");
        RwFileHeader h;
        char why[160] = "";
        uint8_t *buf = malloc(1 << 20);
        const size_t n = f && fread(&h, sizeof h, 1, f) == 1 ? fread(buf, 1, 1 << 20, f) : 0;
        if (f) fclose(f);
        CHECK(n && !rw_check_header(&h, why, sizeof why), "%s: header of a %u-bit writer accepted by a %d-bit reader %s", argv[2], h.ptr_bytes * 8,
              (int)(sizeof(void *) * 8), why);
        if (n) read_records(buf, n);
        free(buf);
    } else {
        uint8_t *copy = malloc(w.n);
        memcpy(copy, w.buf, w.n);
        read_records(copy, w.n);
        free(copy);
        /* a record cut short */
        RwRecordHeader h;
        memcpy(&h, w.buf, sizeof h);
        RwReader r;
        rw_reader(&r, w.buf + sizeof h, 20);
        rw_ru32(&r), rw_ri32(&r), rw_rf32(&r), rw_ru64(&r), rw_rblob(&r, NULL);
        CHECK(rw_rblob(&r, NULL) == NULL && r.bad, "a truncated record is detected");
        /* draw-state slots */
        int slots_ok = rw_draw_state_slot(RW_ID_SET_STAGE_TEXTURE, 2) == 2 && rw_draw_state_slot(RW_ID_SET_STAGE_TEXTURE, 4) == -1 &&
                       rw_draw_state_slot(RW_ID_DRAW, 0) == -1 && rw_draw_state_slot(RW_ID_SET_DRAW_IDENTITY, 0) == -1;
        for (int s = 0; s < RW_DS_SLOTS; s++) slots_ok &= rw_draw_state_slot(rw_draw_state_id(s), (uint32_t)s) == s;
        CHECK(slots_ok, "draw-state slots: one per stage texture and per setter; draws and identities are not setters");
        /* header */
        RwFileHeader fh;
        char why[160];
        rw_file_header(&fh);
        CHECK(!rw_check_header(&fh, why, sizeof why), "own header accepted");
        fh.schema ^= 1;
        CHECK(rw_check_header(&fh, why, sizeof why) && strstr(why, "schema"), "another schema refused: %s", why);
        rw_file_header(&fh);
        fh.magic[0] = 'X';
        CHECK(rw_check_header(&fh, why, sizeof why), "another magic refused: %s", why);
        /* payload sizes */
        CHECK(rw_texture_bytes(RENDERER_TEX_BGRA8, 4, 4, 2) == 64 + 16, "BGRA8 4x4, 2 levels: 80 bytes");
        CHECK(rw_texture_bytes(RENDERER_TEX_BC1, 8, 8, 4) == 32 + 8 + 8 + 8, "BC1 8x8, 4 levels: 56 bytes (a block at least)");
        CHECK(rw_texture_bytes(RENDERER_TEX_BC7, 5, 3, 1) == 2 * 16, "BC7 5x3: 2 blocks");
        CHECK(rw_texture_bytes(RENDERER_TEX_L8, 3, 3, 1) == 9 && rw_texture_bytes(RENDERER_TEX_BC3, 4, 4, 1) == 16, "L8 3x3 9 bytes, BC3 4x4 16");
        CHECK(rw_str_bytes("abc") == 4 && rw_str_bytes(NULL) == 0, "strings carry their terminator");
        /* frame hash: pitch padding ignored, one pixel changes it */
        uint32_t a[3 * 4], b[3 * 4];
        for (int i = 0; i < 12; i++) a[i] = b[i] = 0x00102030u * (uint32_t)i;
        b[3] = b[7] = b[11] = 0xFFFFFFFFu;   /* padding column of a 3-pixel row with a 4-pixel pitch */
        CHECK(rw_frame_hash(a, 3, 3, 16) == rw_frame_hash(b, 3, 3, 16), "frame hash ignores the pitch padding");
        b[5] ^= 1;
        CHECK(rw_frame_hash(a, 3, 3, 16) != rw_frame_hash(b, 3, 3, 16), "frame hash changes with one pixel");
        hash_tests();
        cache_tests();
        cache_table_tests();
        consts_hook_tests();
    }
    rw_free(&w);
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
