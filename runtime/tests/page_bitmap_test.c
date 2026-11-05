/* Game-independent test of plat_is_mapped (src/platform/posix.c): pages mapped by plat_map_fixed
 * answer from a page bitmap (no system call), pages freed by plat_unmap (also part of a mapping)
 * answer 0, and memory mapped by other code still answers through the mincore fallback. Timing:
 * a million checks of guest pages take well under the cost of a million system calls. */
#define _GNU_SOURCE
#include "../src/platform.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define PAGE 0x1000u

static uint32_t free_range(uint32_t size) {   /* an address range nobody holds right now */
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    munmap(p, size);
    return (uint32_t)(uintptr_t)p;
}
static int mapped_count(uint32_t a, int pages) {
    int n = 0;
    for (int i = 0; i < pages; i++) n += plat_is_mapped(a + i * PAGE) != 0;
    return n;
}
static double thread_cpu_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void) {
    /* 18 free pages: guard, 16 guest pages, guard. */
    uint32_t w = free_range(18 * PAGE), g = w + PAGE;
    CHECK(plat_map_fixed(g, 16 * PAGE) == (void *)(uintptr_t)g, "plat_map_fixed 16 pages at %08x", g);
    CHECK(mapped_count(g, 16) == 16, "all 16 guest pages mapped (%d)", mapped_count(g, 16));
    CHECK(plat_is_mapped(g + 5 * PAGE + 123) && plat_is_mapped(g + 16 * PAGE - 1), "addresses inside pages, the last byte");
    CHECK(!plat_is_mapped(w) && !plat_is_mapped(g + 16 * PAGE), "the guard pages before and after are not mapped");
    plat_unmap((void *)(uintptr_t)(g + 4 * PAGE), 4 * PAGE);
    CHECK(mapped_count(g, 16) == 12 && !plat_is_mapped(g + 4 * PAGE) && !plat_is_mapped(g + 7 * PAGE) && plat_is_mapped(g + 3 * PAGE) &&
              plat_is_mapped(g + 8 * PAGE),
          "plat_unmap of pages 4-7: those answer 0, the rest 1 (%d mapped)", mapped_count(g, 16));
    CHECK(plat_map_fixed(g + 4 * PAGE, 4 * PAGE) != NULL && mapped_count(g, 16) == 16, "mapped again: all 16 (%d)", mapped_count(g, 16));

    /* Memory mapped by other code (not through plat_map_fixed): the mincore fallback. */
    uint8_t *f = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t fa = (uint32_t)(uintptr_t)f;
    CHECK(f != MAP_FAILED && plat_is_mapped(fa) && plat_is_mapped(fa + PAGE), "a foreign mapping answers 1 (fallback)");
    munmap(f, 2 * PAGE);
    CHECK(!plat_is_mapped(fa) && !plat_is_mapped(fa + PAGE), "a freed foreign mapping answers 0");

    /* Guest page checks need no system call: a mincore call costs a few hundred ns or more. */
    enum { N = 1000000 };
    double t0 = thread_cpu_ms();
    int hits = 0;
    for (int i = 0; i < N; i++) hits += plat_is_mapped(g + (i & 15) * PAGE);
    double ms = thread_cpu_ms() - t0;
    CHECK(hits == N && ms < 40, "%d guest page checks in %.1f ms of thread CPU (under 40)", N, ms);

    plat_unmap((void *)(uintptr_t)g, 16 * PAGE);
    CHECK(mapped_count(g, 16) == 0, "all unmapped: none mapped");
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
