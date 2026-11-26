/* Guest arena (mem.c): freed address space is reused once the bump pointer reaches the arena
 * end, so allocate/free churn larger than the arena never exhausts it; a full arena returns NULL
 * and recovers after a free. Game-independent. */
#include "../src/mem.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define A(p) ((uint32_t)(uintptr_t)(p))
#define MB 0x100000u
#define ARENA (GUEST_ARENA_END - GUEST_ARENA_START)

/* 1 MB alloc + free, 3 GB in total: twice the 1.5 GB arena. */
static void test_churn_reuses_freed_space(void) {
    unsigned ok = 0, n = 2 * (ARENA / MB);
    for (unsigned i = 0; i < n; i++) {
        void *p = guest_alloc(MB);
        if (!p) break;
        memset(p, 0xAB, 64);
        CHECK(guest_free(A(p), NULL) == 0, "free of %08x failed", A(p));
        ok++;
    }
    CHECK(ok == n, "churn: %u of %u 1 MB allocations succeeded", ok, n);
}

/* Churn with every 8th block kept: reused blocks never overlap a live one and stay in the arena. */
static void test_reuse_keeps_live_blocks(void) {
    enum { KEEP = 300 };
    uint32_t live[KEEP], sz[KEEP];
    int nlive = 0, bad = 0, nulls = 0;
    for (unsigned i = 0; i < 8 * KEEP; i++) {
        uint32_t size = (i % 3 + 1) * 0x40000;   /* 256 KB .. 768 KB */
        uint8_t *p = guest_alloc(size);
        if (!p) { nulls++; continue; }
        if (A(p) < GUEST_ARENA_START || A(p) + size > GUEST_ARENA_END) bad++;
        for (int j = 0; j < nlive; j++)
            if (A(p) < live[j] + sz[j] && live[j] < A(p) + size) bad++;
        if (i % 8 == 0) { live[nlive] = A(p); sz[nlive++] = size; memset(p, (int)i, size); }
        else guest_free(A(p), NULL);
    }
    CHECK(!nulls, "%d allocations failed", nulls);
    CHECK(!bad, "%d allocations overlapped a live block or left the arena", bad);
    for (int j = 0; j < nlive; j++) {
        uint8_t *q = (uint8_t *)(uintptr_t)live[j];
        if (q[0] != q[sz[j] - 1]) bad++;
        guest_free(live[j], NULL);
    }
    CHECK(!bad, "a live block was overwritten");
}

/* A truly full arena returns NULL (no crash); freeing one block makes room again. */
static void test_full_arena_fails_soft(void) {
    static uint32_t blocks[2048];
    int n = 0;
    void *p;
    while (n < 2048 && (p = guest_alloc(MB))) blocks[n++] = A(p);
    CHECK(n > 1000 && n < 2048, "filled the arena with %d 1 MB blocks", n);
    CHECK(guest_alloc(MB) == NULL, "allocation in a full arena did not fail");
    guest_free(blocks[n / 2], NULL);
    p = guest_alloc(MB);
    CHECK(p != NULL && A(p) == blocks[n / 2], "freed block not reused (got %08x, freed %08x)", A(p), blocks[n / 2]);
    if (p) blocks[n / 2] = A(p);
    for (int i = 0; i < n; i++) guest_free(blocks[i], NULL);
}

/* mem_keep_last_freed(size): a freed region of exactly that size stays mapped with its bytes
 * (the title reads a closed Lua state's strings for a few frames) until the next free of that
 * size; then only the newer one is kept. The guest sees it freed at once; other sizes unmap. */
static void test_keep_last_freed(void) {
    const uint32_t keep = 8 * MB;
    mem_keep_last_freed(keep);
    uint8_t *a = guest_alloc(keep), *b = guest_alloc(keep), *c = guest_alloc(2 * MB);
    CHECK(a && b && c, "allocations failed");
    if (!a || !b || !c) return;
    memcpy(a + 0x85900, "BASEBALL FURIES", 16);
    memcpy(b + 0x85900, "ROGUES", 7);
    CHECK(guest_free(A(a), NULL) == 0, "free of the kept-size region failed");
    CHECK(mem_is_mapped(A(a) + 0x85900), "freed kept-size region was unmapped");
    CHECK(mem_is_mapped(A(a) + 0x85900) && !memcmp(a + 0x85900, "BASEBALL FURIES", 16), "freed kept-size region lost its bytes");
    CHECK(guest_region(A(a), NULL) == 0, "freed kept-size region is still a guest region");
    uint8_t *d = guest_alloc(keep);
    CHECK(d && (A(d) >= A(a) + keep || A(d) + keep <= A(a)), "new allocation %08x overlaps the kept block %08x", A(d), A(a));
    CHECK(guest_free(A(b), NULL) == 0, "second free failed");
    CHECK(!mem_is_mapped(A(a) + 0x85900), "the older kept region was not unmapped by the next free");
    CHECK(mem_is_mapped(A(b) + 0x85900) && !memcmp(b + 0x85900, "ROGUES", 7), "the newer kept region was not kept");
    CHECK(guest_free(A(c), NULL) == 0 && !mem_is_mapped(A(c)), "a region of another size stayed mapped");
    if (d) guest_free(A(d), NULL);
    CHECK(!mem_is_mapped(A(b)), "the kept region outlived the next free of its size");
    mem_keep_last_freed(0);
}

int main(void) {
    test_churn_reuses_freed_space();
    test_reuse_keeps_live_blocks();
    test_full_arena_fails_soft();
    test_keep_last_freed();
    printf(fails ? "mem_test: %d FAILED\n" : "mem_test: all passed\n", fails);
    return fails != 0;
}
