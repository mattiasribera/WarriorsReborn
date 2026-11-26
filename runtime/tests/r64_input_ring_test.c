/* Unit tests of the render64 input region (src/render_ipc.h r64_in_*):
 * format and version checks, FIFO order and payloads, wraparound of the slots and of the uint32
 * positions, the overflow policy (drop, then one RESYNC record) and a two-thread SPSC stress run.
 * Built for i386 (build/) and x86_64 (build64/); prints ALL OK. */
#include "../src/render_ipc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static _Alignas(64) uint8_t region[64 << 10];
static _Alignas(64) uint8_t small[8 << 10];

static void test_format(void) {
    R64InConfig c;
    memset(&c, 0, sizeof c);
    c.flags = R64_IN_CFG_WINDOW | R64_IN_CFG_FULLSCREEN;
    c.w = 1280;
    c.h = 720;
    snprintf(c.title, sizeof c.title, "WarriorsReborn");
    CHECK(r64_in_init(region, sizeof region, &c) == R64_OK, "format 64 KB");
    const R64InHeader *h = (const R64InHeader *)region;
    CHECK(h->slots[R64_IN_EVENTS] == 512 && h->slots[R64_IN_CMDS] == 256, "64 KB: %u events, %u commands", h->slots[0], h->slots[1]);
    CHECK(h->cfg.w == 1280 && h->cfg.h == 720 && h->cfg.flags == c.flags && !strcmp(h->cfg.title, "WarriorsReborn"), "window config kept");
    char why[160] = "";
    CHECK(r64_in_check(region, sizeof region, why, sizeof why) == R64_OK, "check accepts its own format");
    CHECK(r64_in_init(region, 2048, NULL) == R64_ERR_ARGS, "a region under 4 KB is refused");
    CHECK(r64_in_init(small, sizeof small, NULL) == R64_OK && ((R64InHeader *)small)->slots[0] == 64 && ((R64InHeader *)small)->slots[1] == 32,
          "8 KB: 64 events, 32 commands");
}

static void test_versions(void) {
    char why[160];
    R64InHeader *h = (R64InHeader *)small;
    r64_in_init(small, sizeof small, NULL);
    h->version = R64_IN_VERSION + 1;
    why[0] = 0;
    CHECK(r64_in_check(small, sizeof small, why, sizeof why) == R64_ERR_VERSION && strstr(why, "version"), "other version refused: %s", why);
    r64_in_init(small, sizeof small, NULL);
    h->magic = 0x12345678;
    CHECK(r64_in_check(small, sizeof small, why, sizeof why) == R64_ERR_VERSION && strstr(why, "magic"), "other magic refused: %s", why);
    r64_in_init(small, sizeof small, NULL);
    h->record_bytes = 32;
    CHECK(r64_in_check(small, sizeof small, why, sizeof why) == R64_ERR_VERSION && strstr(why, "records"), "other record size refused: %s", why);
    r64_in_init(small, sizeof small, NULL);
    h->slots[R64_IN_CMDS] = 48;
    CHECK(r64_in_check(small, sizeof small, why, sizeof why) == R64_ERR_VERSION && strstr(why, "ring 1"), "slots not a power of two refused: %s", why);
    r64_in_init(small, sizeof small, NULL);
    CHECK(r64_in_check(small, sizeof small + 4096, why, sizeof why) == R64_ERR_VERSION, "a region of another size refused: %s", why);
    memset(small, 0, sizeof small);
    CHECK(r64_in_check(small, sizeof small, why, sizeof why) == R64_ERR_VERSION, "an unformatted region refused: %s", why);
}

/* FIFO order, payloads and timestamps over many wraps of a 64-slot ring, starting the positions just
 * below 2^32 so they also wrap. */
static void test_wrap(void) {
    r64_in_init(small, sizeof small, NULL);
    R64InHeader *h = (R64InHeader *)small;
    h->ring[R64_IN_EVENTS].head = h->ring[R64_IN_EVENTS].tail = 0xFFFFFF00u;
    uint32_t sent = 0, got = 0, bad = 0, batch_max = 0;
    uint64_t last_t = 0;
    for (int round = 0; round < 3000; round++) {
        const uint32_t batch = 1 + (uint32_t)(round * 7919u) % 64;   /* 1..64: up to a full ring */
        if (batch > batch_max) batch_max = batch;
        for (uint32_t i = 0; i < batch; i++) {
            const int32_t v[10] = {(int32_t)(sent + i), (int32_t)~(sent + i), 3, 4, 5, 6, 7, 8, 9, 10};
            if (!r64_in_put(small, R64_IN_EVENTS, R64_IN_KEY, (uint16_t)(sent + i), v, sizeof v, 1000u + sent + i)) bad++;
        }
        sent += batch;
        R64InEvent e;
        while (r64_in_get(small, R64_IN_EVENTS, &e)) {
            const uint64_t t = (uint64_t)e.t_hi << 32 | e.t_lo, o = (uint64_t)e.o_hi << 32 | e.o_lo;
            if (e.seq != got || e.type != R64_IN_KEY || e.arg != (uint16_t)got || e.v[0] != (int32_t)got || e.v[1] != (int32_t)~got ||
                e.v[9] != 10 || o != 1000u + got || t < last_t)
                bad++;
            last_t = t;
            got++;
        }
    }
    CHECK(!bad && got == sent && r64_in_dropped(small, R64_IN_EVENTS) == 0, "%u records in order through %u wraps (batches up to %u, positions past 2^32): %u bad",
          got, sent / 64, batch_max, bad);
    CHECK(h->ring[R64_IN_EVENTS].head < 0x1000000u, "head wrapped to %u", h->ring[R64_IN_EVENTS].head);
    const int32_t big[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    r64_in_put(small, R64_IN_CMDS, R64_IN_RUMBLE, 2, big, sizeof big, 0);
    R64InEvent e;
    CHECK(r64_in_get(small, R64_IN_CMDS, &e) && e.type == R64_IN_RUMBLE && e.arg == 2 && e.v[9] == 10 && !r64_in_get(small, R64_IN_CMDS, &e),
          "payloads are cut at 40 bytes; the command ring is separate");
}

static void test_overflow(void) {
    r64_in_init(small, sizeof small, NULL);
    int ok = 1;
    for (int i = 0; i < 64; i++) ok &= r64_in_put(small, R64_IN_EVENTS, R64_IN_MOTION, 0, &i, 4, 0);
    CHECK(ok, "64 records fill the 64-slot ring");
    int k = 64;
    CHECK(!r64_in_put(small, R64_IN_EVENTS, R64_IN_MOTION, 0, &k, 4, 0) && r64_in_dropped(small, R64_IN_EVENTS) == 1, "a full ring drops the new record and counts it");
    R64InEvent e;
    r64_in_get(small, R64_IN_EVENTS, &e);
    k = 65;
    CHECK(!r64_in_put(small, R64_IN_EVENTS, R64_IN_MOTION, 0, &k, 4, 0) && r64_in_dropped(small, R64_IN_EVENTS) == 2,
          "one free slot is not enough after a drop (the resync record needs one)");
    r64_in_get(small, R64_IN_EVENTS, &e);
    k = 66;
    CHECK(r64_in_put(small, R64_IN_EVENTS, R64_IN_KEY, 1, &k, 4, 0), "two free slots: written");
    int n = 0, resync_at = -1, resync_v = -1, last = -1;
    uint32_t seq = 2, seq_ok = 1;
    while (r64_in_get(small, R64_IN_EVENTS, &e)) {
        seq_ok &= e.seq == seq++;
        if (e.type == R64_IN_RESYNC) resync_at = n, resync_v = e.v[0];
        last = e.v[0];
        n++;
    }
    CHECK(n == 64 && resync_at == 62 && resync_v == 2 && last == 66 && seq_ok, "the 62 kept records, one RESYNC (2 dropped), then the new one; seq without gaps");
    k = 67;
    r64_in_put(small, R64_IN_EVENTS, R64_IN_KEY, 1, &k, 4, 0);
    CHECK(r64_in_get(small, R64_IN_EVENTS, &e) && e.type == R64_IN_KEY && !r64_in_get(small, R64_IN_EVENTS, &e), "no second RESYNC");
}

/* Two threads: the producer writes 2 M records as fast as it can (dropping when full), the consumer
 * reads them; everything that arrives is in order and kept + dropped = written. */
#define STRESS_N 2000000u
static void *producer(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < STRESS_N; i++) r64_in_put(small, R64_IN_EVENTS, R64_IN_MOTION, 0, &i, 4, 0);
    r64_in_put(small, R64_IN_CMDS, R64_IN_QUIT, 0, NULL, 0, 0);   /* the end mark */
    return NULL;
}
static void test_stress(void) {
    r64_in_init(small, sizeof small, NULL);
    pthread_t t;
    pthread_create(&t, NULL, producer, NULL);
    uint32_t kept = 0, resyncs = 0, bad = 0, seq = 0;
    int64_t last = -1;
    R64InEvent e;
    for (int done = 0;;) {
        int any = 0;
        while (r64_in_get(small, R64_IN_EVENTS, &e)) {
            any = 1;
            if (e.seq != seq++) bad++;
            if (e.type == R64_IN_RESYNC) { resyncs++; continue; }
            if ((int64_t)(uint32_t)e.v[0] <= last) bad++;
            last = (uint32_t)e.v[0];
            kept++;
        }
        if (done && !any) break;
        if (!done && r64_in_get(small, R64_IN_CMDS, &e)) done = 1;   /* drain what is left, then stop */
    }
    pthread_join(t, NULL);
    const uint32_t dropped = r64_in_dropped(small, R64_IN_EVENTS);
    CHECK(!bad && kept + dropped == STRESS_N && (dropped || !resyncs) && resyncs <= dropped, "SPSC threads: %u kept + %u dropped = %u, %u resyncs, %u out of order",
          kept, dropped, kept + dropped, resyncs, bad);
}

int main(void) {
    printf("r64_input_ring_test (%d-bit)\n", (int)(sizeof(void *) * 8));
    test_format();
    test_versions();
    test_wrap();
    test_overflow();
    test_stress();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
