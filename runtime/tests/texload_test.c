/* Game-independent test of the texture pack load queue (src/texload.c): one slot per (hash,
 * kind), demand before prefetch, promotion, the staged-bytes cap, dropping images nobody takes,
 * the per-frame upload budget (bytes and ms, first upload always allowed), failures, and the
 * races the render thread meets: a slot being loaded is never dropped, a texture requested
 * again while it loads shares the slot, and a staged image survives until taken or reclaimed. */
#include "../src/texload.h"
#include "../src/renderer/tex_lru.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define MB (1u << 20)
static TlSlot slots[8];
static TexLoadQueue q;

static void reset(size_t cap, uint32_t keep, size_t budget, double ms) {
    for (int i = 0; i < 8; i++) {
        free(slots[i].path);
        free(slots[i].img.data);
    }
    memset(slots, 0, sizeof slots);
    tl_init(&q, slots, 8, cap, keep, budget, ms);
}

static int req(uint64_t hash, int kind, int prio, uint32_t frame) {
    int created = 0, i = tl_request(&q, hash, kind, prio, frame, &created);
    if (i >= 0 && created) {
        q.slot[i].path = malloc(8);
        strcpy(q.slot[i].path, "f");
    }
    return i;
}

/* A loader: takes the next slot and finishes it with `bytes` bytes. */
static int load(size_t bytes, int fail, uint32_t frame) {
    int i = tl_next(&q);
    if (i < 0) return -1;
    free(q.slot[i].path);
    q.slot[i].path = NULL;
    TlImage img = {.w = 4, .h = 4, .levels = 1, .bytes = bytes, .data = fail ? NULL : malloc(bytes ? bytes : 1), .why = fail ? "bad" : NULL};
    tl_done(&q, i, &img, fail, frame);
    return i;
}

static void test_dedupe(void) {
    reset(64 * MB, 120, 16 * MB, 4);
    int a = req(0x11, TL_BASE, TL_DEMAND, 1), b = req(0x11, TL_BASE, TL_DEMAND, 2);
    int m = req(0x11, TL_MASK, TL_DEMAND, 2);
    CHECK(a >= 0 && a == b, "same hash and kind share a slot (%d, %d)", a, b);
    CHECK(m >= 0 && m != a, "the mask of a hash has its own slot (%d)", m);
    CHECK(tl_find(&q, 0x11, TL_BASE) == a && tl_find(&q, 0x12, TL_BASE) == -1, "find");
    CHECK(q.slot[a].touched == 2, "a repeated request touches the slot (frame %u)", q.slot[a].touched);
    int l = tl_next(&q);
    CHECK(l == a && q.slot[a].state == TL_LOADING, "the first request loads first");
    CHECK(req(0x11, TL_BASE, TL_DEMAND, 3) == a && tl_count(&q, TL_QUEUED) == 1, "a request while loading shares the slot");
}

static void test_priority(void) {
    reset(64 * MB, 120, 16 * MB, 4);
    int p1 = req(0x21, TL_BASE, TL_PREFETCH, 1), p2 = req(0x22, TL_BASE, TL_PREFETCH, 1);
    int d1 = req(0x23, TL_BASE, TL_DEMAND, 1);
    CHECK(tl_next(&q) == d1, "demand loads before older prefetches");
    CHECK(tl_next(&q) == p1, "then prefetches in request order");
    req(0x22, TL_BASE, TL_DEMAND, 2);
    CHECK(q.slot[p2].prio == TL_DEMAND, "a drawn prefetch is promoted to demand");
    CHECK(q.promoted == 1 && q.prefetch_hits == 0, "one promotion, not loaded yet");
    int p3 = req(0x24, TL_BASE, TL_PREFETCH, 2);
    CHECK(tl_next(&q) == p2 && tl_next(&q) == -1, "promoted first; p3 waits while prefetch p1 loads");
    TlImage p1img = {.bytes = MB, .data = malloc(1)};
    tl_done(&q, p1, &p1img, 0, 2);
    CHECK(tl_next(&q) == p3 && tl_next(&q) == -1, "then the rest, then nothing");

    reset(64 * MB, 120, 16 * MB, 4);
    int f1 = req(0x31, TL_BASE, TL_PREFETCH, 1), f2 = req(0x32, TL_BASE, TL_PREFETCH, 1);
    CHECK(tl_next(&q) == f1 && tl_next(&q) == -1, "one prefetch load at a time (the disk stays free for the game)");
    int dd = req(0x33, TL_BASE, TL_DEMAND, 1);
    CHECK(tl_next(&q) == dd, "a demand load still starts beside it");
    load(MB, 0, 1);   /* nothing left to start: returns -1, f1 stays loading */
    TlImage done = {.bytes = MB, .data = malloc(1)};
    tl_done(&q, f1, &done, 0, 1);
    CHECK(tl_next(&q) == f2, "the next prefetch starts when the first is done");
    CHECK(req(0x24, TL_BASE, TL_PREFETCH, 3) == p3 && q.slot[p3].prio == TL_PREFETCH, "a prefetch never demotes");
}

static void test_budget(void) {
    reset(256 * MB, 120, 10 * MB, 4);
    int a = req(1, TL_BASE, TL_DEMAND, 1), b = req(2, TL_BASE, TL_DEMAND, 1), c = req(3, TL_BASE, TL_DEMAND, 1);
    load(6 * MB, 0, 1);
    load(6 * MB, 0, 1);
    load(30 * MB, 0, 1);
    CHECK(q.staged == 42 * MB, "staged bytes counted (%zu MB)", q.staged / MB);
    TlImage img = {0};
    CHECK(tl_take(&q, a, 5, &img) == 1 && img.bytes == 6 * MB, "the first upload of a frame goes");
    free(img.data);
    CHECK(tl_take(&q, b, 5, &img) == 0 && q.slot[b].state == TL_READY, "a second one past the byte budget waits staged");
    CHECK(tl_take(&q, c, 6, &img) == 1 && img.bytes == 30 * MB, "next frame: an image larger than the budget still goes first");
    free(img.data);
    CHECK(tl_take(&q, b, 6, &img) == 0, "and fills that frame");
    CHECK(tl_take(&q, b, 7, &img) == 1, "the frame after takes the waiting one");
    free(img.data);
    CHECK(q.staged == 0 && tl_count(&q, TL_FREE) == 8, "everything taken, nothing staged");
    CHECK(q.deferred == 1, "one image waited for a later frame, counted once (%u)", q.deferred);

    reset(256 * MB, 120, 100 * MB, 4);
    a = req(1, TL_BASE, TL_DEMAND, 1);
    b = req(2, TL_BASE, TL_DEMAND, 1);
    c = req(3, TL_BASE, TL_DEMAND, 1);
    load(MB, 0, 1);
    load(MB, 0, 1);
    load(MB, 0, 1);
    CHECK(tl_take(&q, a, 9, &img) == 1, "first upload");
    free(img.data);
    tl_charge_ms(&q, 9, 3.0);
    CHECK(tl_take(&q, b, 9, &img) == 1, "under the ms budget: another");
    free(img.data);
    tl_charge_ms(&q, 9, 1.5);
    CHECK(tl_take(&q, c, 9, &img) == 0, "4.5 ms spent of 4: the third waits");
    CHECK(tl_take(&q, c, 10, &img) == 1, "a new frame resets the budget");
    free(img.data);
}

static void test_reclaim(void) {
    reset(10 * MB, 100, 64 * MB, 4);
    int d = req(1, TL_BASE, TL_DEMAND, 1);
    load(4 * MB, 0, 1);
    tl_reclaim(&q, 50);
    CHECK(q.slot[d].state == TL_READY, "a staged demand image stays while recently asked for");
    int pf = req(5, TL_BASE, TL_PREFETCH, 50);
    load(MB, 0, 50);
    req(5, TL_BASE, TL_DEMAND, 51);
    CHECK(q.prefetch_hits == 1 && q.slot[pf].prio == TL_DEMAND, "a prefetch drawn after it loaded is a hit");
    TlImage hit = {0};
    CHECK(tl_take(&q, pf, 51, &hit) == 1, "and is uploaded");
    free(hit.data);
    req(1, TL_BASE, TL_DEMAND, 80);
    tl_reclaim(&q, 150);
    CHECK(q.slot[d].state == TL_READY, "a request refreshes it");
    tl_reclaim(&q, 181);
    CHECK(q.slot[d].state == TL_FREE && q.staged == 0 && q.dropped_stale == 1,
          "nobody asked for 100 frames (texture destroyed): dropped, bytes released");

    reset(8 * MB, 100, 64 * MB, 4);
    int p1 = req(1, TL_BASE, TL_PREFETCH, 1), p2 = req(2, TL_BASE, TL_PREFETCH, 2), p3 = req(3, TL_BASE, TL_PREFETCH, 3);
    load(4 * MB, 0, 1);
    load(4 * MB, 0, 2);
    CHECK(tl_next(&q) == -1 && q.slot[p3].state == TL_QUEUED, "prefetch waits at the staged cap (8 of 8 MB staged)");
    int d2 = req(4, TL_BASE, TL_DEMAND, 3);
    CHECK(tl_next(&q) == d2, "a demand load starts past the cap");
    TlImage img = {.bytes = 8 * MB, .data = malloc(1)};
    tl_done(&q, d2, &img, 0, 3);
    tl_reclaim(&q, 4);
    CHECK(q.slot[p1].state == TL_FREE && q.slot[p2].state == TL_FREE && q.slot[d2].state == TL_READY && q.staged == 8 * MB,
          "over the cap the oldest staged prefetches go, the demand image stays");
    CHECK(q.dropped_cap == 2, "two cap drops (%u)", q.dropped_cap);

    /* A staged prefetch nobody draws (registered, never shown) cannot hold the cap forever. */
    reset(8 * MB, 100, 64 * MB, 4);
    int s1 = req(1, TL_BASE, TL_PREFETCH, 10), s2 = req(2, TL_BASE, TL_PREFETCH, 10);
    load(4 * MB, 0, 10);
    load(4 * MB, 0, 10);
    int s3 = req(3, TL_BASE, TL_PREFETCH, 20);
    tl_reclaim(&q, 400);
    CHECK(q.slot[s1].state == TL_READY && tl_next(&q) == -1, "a staged prefetch is kept 4x longer than a demand image");
    tl_reclaim(&q, 411);
    CHECK(q.slot[s1].state == TL_FREE && q.slot[s2].state == TL_FREE && q.dropped_stale == 2, "then dropped");
    CHECK(tl_next(&q) == s3, "and the waiting prefetch loads");
    (void)p2;
}

static void test_full(void) {
    reset(256 * MB, 100, 64 * MB, 4);
    for (int i = 0; i < 8; i++) req(0x100 + i, TL_BASE, TL_PREFETCH, 1);
    CHECK(req(0x200, TL_BASE, TL_PREFETCH, 1) == -1, "a full queue refuses another prefetch");
    int d = req(0x201, TL_BASE, TL_DEMAND, 2);
    CHECK(d >= 0 && tl_find(&q, 0x100, TL_BASE) == -1, "a demand request replaces the oldest queued prefetch");
    reset(256 * MB, 100, 64 * MB, 4);
    for (int i = 0; i < 8; i++) req(0x100 + i, TL_BASE, TL_DEMAND, 1);
    for (int i = 0; i < 8; i++) tl_next(&q);
    CHECK(req(0x201, TL_BASE, TL_DEMAND, 2) == -1, "all slots loading: refused, never dropped while a loader writes them");
    CHECK(tl_count(&q, TL_LOADING) == 8, "still 8 loading");
}

static void test_failure(void) {
    reset(256 * MB, 100, 64 * MB, 4);
    int a = req(7, TL_BASE, TL_DEMAND, 1);
    load(0, 1, 1);
    TlImage img = {0};
    CHECK(tl_take(&q, a, 2, &img) == -1 && img.why && !strcmp(img.why, "bad"), "a failure is reported with its reason");
    CHECK(q.slot[a].state == TL_FREE, "and frees the slot");
    a = req(8, TL_BASE, TL_DEMAND, 1);
    load(0, 1, 1);
    tl_reclaim(&q, 200);
    CHECK(q.slot[a].state == TL_FREE, "an untaken failure is reclaimed too");
    int b = req(9, TL_BASE, TL_DEMAND, 300);
    tl_next(&q);
    tl_reclaim(&q, 1000);
    CHECK(q.slot[b].state == TL_LOADING, "a loading slot is never reclaimed");
}

/* --level-prefetch: prefetch loads beside each other (prefetch_par), pre-uploads of ready prefetch
 * images within the frame's budget (never the first-upload exception), oldest first. */
static void test_prefetch_par(void) {
    reset(256 * MB, 120, 10 * MB, 4);
    q.prefetch_par = 3;
    int a = req(0x41, TL_BASE, TL_PREFETCH, 1), b = req(0x42, TL_BASE, TL_PREFETCH, 1), c = req(0x43, TL_BASE, TL_PREFETCH, 1);
    req(0x44, TL_BASE, TL_PREFETCH, 1);
    CHECK(tl_next(&q) == a && tl_next(&q) == b && tl_next(&q) == c, "prefetch_par 3: three prefetches load together");
    CHECK(tl_next(&q) == -1, "not a fourth");
    int d = req(0x45, TL_BASE, TL_DEMAND, 1);
    CHECK(tl_next(&q) == d, "a demand load still starts beside them");
    q.prefetch_par = 0;
    TlImage done = {.bytes = MB, .data = malloc(1)};
    tl_done(&q, a, &done, 0, 1);
    CHECK(tl_next(&q) == -1, "prefetch_par 0 = one at a time (two still loading)");
}

static void test_take_prefetch(void) {
    reset(256 * MB, 120, 10 * MB, 4);
    int a = req(0x51, TL_BASE, TL_PREFETCH, 1), m = req(0x51, TL_MASK, TL_PREFETCH, 1);
    int b = req(0x52, TL_BASE, TL_PREFETCH, 1), dm = req(0x53, TL_BASE, TL_DEMAND, 1);
    q.prefetch_par = 8;
    for (int k = 0; k < 4; k++) load(k == 2 ? 30 * MB : 4 * MB, 0, 1);   /* dm, a, m (30 MB), b */
    (void)m;
    TlImage img = {0};
    uint64_t h = 0;
    CHECK(tl_take_prefetch(&q, TL_BASE, 5, &img, &h) == 1 && h == 0x51 && img.bytes == 4 * MB, "the oldest ready base prefetch goes first");
    free(img.data);
    CHECK(q.slot[a].state == TL_FREE && q.staged == 38 * MB, "its slot is freed and its bytes unstaged (%zu MB)", q.staged / MB);
    CHECK(tl_take_prefetch(&q, TL_BASE, 5, &img, &h) == 1 && h == 0x52, "then the next, within the 10 MB budget");
    free(img.data);
    CHECK(tl_take_prefetch(&q, TL_BASE, 5, &img, &h) == 0, "no more ready base prefetches (the demand image is not one)");
    CHECK(q.slot[dm].state == TL_READY, "the demand image stays for its draw");
    CHECK(tl_take_prefetch(&q, TL_MASK, 6, &img, &h) == 0, "a 30 MB image over the 10 MB budget never pre-uploads");
    q.budget_bytes = 64 * MB;
    CHECK(tl_take_prefetch(&q, TL_MASK, 6, &img, &h) == 1 && h == 0x51, "with a larger (loading) budget it does");
    free(img.data);
    reset(256 * MB, 120, 10 * MB, 4);
    b = req(0x61, TL_BASE, TL_PREFETCH, 1);
    load(MB, 0, 1);
    tl_charge_ms(&q, 9, 5.0);
    CHECK(tl_take_prefetch(&q, TL_BASE, 9, &img, &h) == 0 && q.slot[b].state == TL_READY, "time already charged to the frame (5 of 4 ms) stops pre-uploads");
    CHECK(tl_take_prefetch(&q, TL_BASE, 10, &img, &h) == 1, "the next frame takes it");
    free(img.data);
}

/* Level texture lists: a set in first-seen order, an early sighting wins over a late one,
 * parse/format round trip, bad lines ignored, capacity. */
static TlList list, list2;
static void test_list(void) {
    tl_list_clear(&list);
    CHECK(tl_list_add(&list, 0xaa, 1) == 1 && tl_list_add(&list, 0xbb, 0) == 1, "two new hashes");
    CHECK(tl_list_add(&list, 0xaa, 1) == 0 && list.n == 2, "a repeat is not added again");
    CHECK(tl_list_add(&list, 0xaa, 0) == 0 && !list.late[tl_list_find(&list, 0xaa)], "drawn early on a later visit: early from now on");
    CHECK(tl_list_add(&list, 0xbb, 1) == 0 && !list.late[tl_list_find(&list, 0xbb)], "an early entry never becomes late");
    CHECK(tl_list_find(&list, 0xcc) == -1 && tl_list_find(&list, 0xbb) == 1, "find");
    tl_list_add(&list, 0xdeadbeef00000001ull, 1);
    char buf[512];
    const size_t n = tl_list_format(&list, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf && strstr(buf, "f 00000000000000aa\n") && strstr(buf, "l deadbeef00000001\n"), "format: one line per hash, f/l");
    CHECK(buf[0] == '#', "with a comment header");
    CHECK(tl_list_format(&list, buf, 8) == n, "a short buffer reports the size needed");
    tl_list_clear(&list2);
    tl_list_format(&list, buf, sizeof buf);
    CHECK(tl_list_parse(&list2, buf) == 3 && list2.n == 3, "parse reads it back (%d)", list2.n);
    CHECK(list2.hash[0] == 0xaa && !list2.late[0] && list2.hash[2] == 0xdeadbeef00000001ull && list2.late[2], "same order and tags");
    tl_list_clear(&list2);
    CHECK(tl_list_parse(&list2, "x 12\nf zz\nf 0123456789abcdef0\n\nl 0000000000000010\r\nf 0000000000000011") == 2 && list2.n == 2, "bad lines skipped, CRLF and a missing last newline accepted");
    tl_list_clear(&list);
    int added = 0;
    for (uint64_t k = 1; k <= TL_LIST_MAX + 10; k++) added += tl_list_add(&list, k * 0x9e3779b97f4a7c15ull, 0) == 1;
    CHECK(added == TL_LIST_MAX && tl_list_add(&list, 7, 0) == -1, "full at TL_LIST_MAX (%d)", added);
    CHECK(tl_list_find(&list, 5 * 0x9e3779b97f4a7c15ull) == 4, "still found when full");
}

/* Texture cache LRU (src/renderer/tex_lru.h): oldest first until under both targets, recently used
 * and free entries never picked, "not enough" reported. */
static void test_lru(void) {
    TexLruEntry e[6] = {{10, 100}, {3, 100}, {50, 100}, {0, 0}, {7, 300}, {49, 100}};
    TexLruPick scratch[6];
    int out[6];
    uint64_t freed = 0;
    int n = tex_lru_pick(e, 6, 700, 400, 5, 5, 45, scratch, out, &freed);
    CHECK(n == 2 && out[0] == 1 && out[1] == 4 && freed == 400, "bytes 700 -> 400: the two oldest (%d: %d %d, %llu)", n, out[0], out[1], (unsigned long long)freed);
    n = tex_lru_pick(e, 6, 700, 700, 5, 3, 45, scratch, out, &freed);
    CHECK(n == 2 && out[0] == 1 && out[1] == 4, "live 5 -> 3: two entries");
    n = tex_lru_pick(e, 6, 700, 100, 5, 5, 45, scratch, out, &freed);
    CHECK(n == 3 && freed == 500, "entries used at or after keep_from (50, 49) are kept, the free one is never picked (%d)", n);
    CHECK(tex_lru_enough(700, 100, 5, 5, freed, n) == 0, "and the target was not reached");
    CHECK(tex_lru_enough(700, 400, 5, 5, 400, 2) == 1, "reached");
    n = tex_lru_pick(e, 6, 300, 400, 5, 5, 45, scratch, out, &freed);
    CHECK(n == 0 && freed == 0, "under the targets: nothing");
}

/* A failed upload is tried again on later draws, up to TL_UPLOAD_TRIES failures in a row per hash;
 * a success forgets them; hashes keep their own counts. */
static void test_upload_fails(void) {
    static TlUploadFails f;
    memset(&f, 0, sizeof f);
    int tries = 0;
    while (tl_upload_failed(&f, 0xABC0001) && tries < 10) tries++;
    CHECK(tries == TL_UPLOAD_TRIES - 1, "retried after each of the first %d failures, given up at failure %d (%d retries)", TL_UPLOAD_TRIES - 1,
          TL_UPLOAD_TRIES, tries);
    CHECK(!tl_upload_failed(&f, 0xABC0001), "still given up on");
    CHECK(tl_upload_failed(&f, 0xABC0002), "another hash has its own count");
    tl_upload_ok(&f, 0xABC0001);
    CHECK(tl_upload_failed(&f, 0xABC0001), "a successful upload forgets the failures");
    tl_upload_failed(&f, 0xABC0003);
    tl_upload_ok(&f, 0xABC0003);
    tl_upload_ok(&f, 0xABC0003 + TL_UPLOAD_SLOTS);   /* a colliding hash's success leaves the slot's count alone */
    CHECK(tl_upload_failed(&f, 0xABC0003) && tl_upload_failed(&f, 0xABC0003) && !tl_upload_failed(&f, 0xABC0003),
          "a colliding hash does not touch another's count");
}

/* tl_find's index (2048 slots): random requests, loads, takes, drops and reclaims; every live
 * (hash, kind) is found at its slot and no freed one is. */
static TlSlot big_slots[TL_MAX_SLOTS];
static void test_index(void) {
    static TexLoadQueue bq;
    tl_init(&bq, big_slots, TL_MAX_SLOTS, 64 * MB, 50, 64 * MB, 100);
    uint32_t rnd = 12345;
    int bad = 0;
    for (uint32_t frame = 1; frame < 4000 && !bad; frame++) {
        for (int r = 0; r < 8; r++) {
            rnd = rnd * 1103515245u + 12345u;
            const uint64_t hash = (rnd >> 8) % 3000 + 1;
            const int kind = (int)(rnd >> 4) % 3, prio = (rnd >> 2) & 1;
            int created, i = tl_request(&bq, hash, kind, prio, frame, &created);
            if (i >= 0 && created) bq.slot[i].path = NULL;
        }
        bq.prefetch_par = 8;
        for (int k = 0; k < 6; k++) {
            int i = tl_next(&bq);
            if (i < 0) break;
            TlImage img = {.bytes = 4096, .data = malloc(16)};
            tl_done(&bq, i, &img, (rnd >> 3) % 7 == 0, frame);
        }
        TlImage out;
        uint64_t h;
        for (int k = 0; k < 3; k++)
            if (tl_take_prefetch(&bq, TL_BASE, frame, &out, &h) == 1) free(out.data);
        for (int i = 0; i < bq.n; i += 97)
            if (bq.slot[i].state == TL_READY || bq.slot[i].state == TL_FAILED) {
                const int r = tl_take(&bq, i, frame, &out);
                if (r == 1) free(out.data);
            }
        tl_reclaim(&bq, frame);
        if (frame % 50) continue;
        for (int i = 0; i < bq.n; i++) {
            const TlSlot *sl = &bq.slot[i];
            if (sl->state != TL_FREE && tl_find(&bq, sl->hash, sl->kind) != i) bad++;
        }
        for (uint64_t hh = 1; hh <= 3000; hh += 13)
            for (int kind = 0; kind < 3; kind++) {
                const int i = tl_find(&bq, hh, kind);
                if (i >= 0 && (bq.slot[i].state == TL_FREE || bq.slot[i].hash != hh || bq.slot[i].kind != kind)) bad++;
            }
    }
    CHECK(!bad, "the index agrees with the slots through 4000 frames of random use (%d mismatches)", bad);
    for (int i = 0; i < bq.n; i++) {
        free(big_slots[i].img.data);
        free(big_slots[i].path);
    }
}

int main(void) {
    test_dedupe();
    test_priority();
    test_budget();
    test_reclaim();
    test_full();
    test_failure();
    test_prefetch_par();
    test_take_prefetch();
    test_list();
    test_lru();
    test_upload_fails();
    test_index();
    reset(0, 0, 0, 0);
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
