#define _GNU_SOURCE
#include "mem.h"
#include "diaglog.h"
#include "platform.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000u
#define MAX_REGIONS 8192

typedef struct { uint32_t base, size; } Region;
static Region regions[MAX_REGIONS];
static int nregions;
static uint32_t arena_next = GUEST_ARENA_START;
static uint32_t contig_next = CONTIG_BASE + 0x10000; /* keep page 0 of "physical" RAM unused */
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
#define MAX_CONTIG 4096
static Region contig[MAX_CONTIG];
static int ncontig;

void mem_init(void) {
    if (!plat_map_fixed(CONTIG_BASE, CONTIG_SIZE)) { fprintf(stderr, "cannot map the contiguous window at %08x\n", CONTIG_BASE); exit(1); }
    uint32_t r = plat_reserve_guest_range(GUEST_ARENA_START, GUEST_ARENA_RESERVED_END);
    if (r) printf("[mem] guest arena %08x-%08x: %u MB kept free of host allocations\n", GUEST_ARENA_START, GUEST_ARENA_RESERVED_END, r >> 20);
}

static uint64_t arena_used;   /* bytes of the tracked regions inside the guest arena */
static int in_arena(uint32_t base) { return base >= GUEST_ARENA_START && base < GUEST_ARENA_END; }
uint64_t mem_arena_used(void) { return __atomic_load_n(&arena_used, __ATOMIC_RELAXED); }
uint64_t mem_arena_total(void) { return (uint64_t)GUEST_ARENA_END - GUEST_ARENA_START; }

static void track_locked(uint32_t base, uint32_t size) {
    static int full_logged;
    if (in_arena(base)) __atomic_add_fetch(&arena_used, size, __ATOMIC_RELAXED);
    if (nregions < MAX_REGIONS) regions[nregions++] = (Region){base, size};
    else if (!full_logged) {
        full_logged = 1;
        fprintf(stderr, "[mem] guest region table full (%d entries): %08x+%x is not tracked\n", MAX_REGIONS, base, size);
    }
}

void guest_track(uint32_t base, uint32_t size) {
    pthread_mutex_lock(&mem_lock);
    track_locked(base, size);
    pthread_mutex_unlock(&mem_lock);
}

static void *map_fixed(uint32_t addr, size_t size) { return plat_map_fixed(addr, size); }

void *guest_alloc_at(uint32_t addr, size_t size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    void *p = map_fixed(addr, size);
    if (p) guest_track(addr, size);
    return p;
}

static int region_cmp(const void *x, const void *y) {
    uint32_t a = ((const Region *)x)->base, b = ((const Region *)y)->base;
    return a < b ? -1 : a > b;
}

/* Lowest arena address that fits size between the tracked regions (sorted by base), a guard page
 * after every region and after the new one, aligned to align, at or above from; 0 if none. */
static uint32_t mem_first_fit(const Region *sorted, int n, uint32_t from, uint32_t size, uint32_t align) {
    uint64_t a = ((uint64_t)from + align - 1) & ~(uint64_t)(align - 1);
    for (int i = 0; i < n; i++) {
        uint64_t rb = sorted[i].base, re = rb + sorted[i].size + PAGE;
        if (re <= a) continue;                      /* ends (with its guard page) below the candidate */
        if (rb >= a + size + PAGE) break;           /* the gap before this region fits */
        a = (re + align - 1) & ~(uint64_t)(align - 1);
    }
    return a + size <= GUEST_ARENA_END ? (uint32_t)a : 0;
}

/* The bump pointer never comes back, so once it reaches the arena end, an allocation looks
 * for the lowest gap left by freed guest regions (or skipped over host mappings). Called with
 * mem_lock held. */
static Region sorted_scratch[MAX_REGIONS];
static void *reuse_gap(uint32_t size, uint32_t align) {
    int n = nregions;
    memcpy(sorted_scratch, regions, (size_t)n * sizeof *regions);
    qsort(sorted_scratch, (size_t)n, sizeof *sorted_scratch, region_cmp);
    uint32_t step = align > 0x10000 ? align : 0x10000;
    for (uint32_t from = GUEST_ARENA_START, a; (a = mem_first_fit(sorted_scratch, n, from, size, align)) != 0; from = a + step) {
        void *p = map_fixed(a, size);
        if (p) return p;
        if (a + step < a) break;
    }
    return NULL;
}

static unsigned arena_reused;
void *guest_alloc(size_t size) {
    if (!size || size > GUEST_ARENA_END - GUEST_ARENA_START) return NULL;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    pthread_mutex_lock(&mem_lock);
    void *p = NULL;
    uint32_t gran = plat_alloc_granularity() - 1;
    while (!p && arena_next + size <= GUEST_ARENA_END) {
        p = map_fixed(arena_next, size);
        arena_next += p ? size + PAGE : 0x100000;   /* leave a guard gap */
        arena_next = (arena_next + gran) & ~gran;   /* Windows places allocations on 64 KB */
    }
    if (!p && (p = reuse_gap((uint32_t)size, gran + 1)) != NULL && !arena_reused++)
        fprintf(stderr, "[mem] guest arena end reached: allocations now reuse freed space\n");
    if (p) track_locked((uint32_t)(uintptr_t)p, (uint32_t)size);
    uint32_t live = 0;
    if (!p)
        for (int i = 0; i < nregions; i++)
            if (regions[i].base >= GUEST_ARENA_START && regions[i].base < GUEST_ARENA_END) live += regions[i].size >> 10;
    int nr = nregions;
    pthread_mutex_unlock(&mem_lock);
    if (p && size >= MEM_LARGE_ALLOC)
        dlog_crumb("large alloc %zu KiB at %08x (arena %llu MiB used)", size >> 10, (uint32_t)(uintptr_t)p,
                   (unsigned long long)(mem_arena_used() >> 20));
    if (!p) {
        dlog_crumb("guest arena exhausted: %zu KiB refused, %u MB live in %d regions", size >> 10, live >> 10, nr);
        fprintf(stderr, "guest arena exhausted (%zu bytes): %u MB live in %d guest regions\n", size, live >> 10, nr);
        plat_report_memory("guest arena exhausted");
    }
    return p;
}

uint32_t guest_region(uint32_t addr, uint32_t *size_out) {
    uint32_t base = 0;
    pthread_mutex_lock(&mem_lock);
    for (int i = 0; i < nregions; i++)
        if (addr >= regions[i].base && addr - regions[i].base < regions[i].size) {
            base = regions[i].base;
            if (size_out) *size_out = regions[i].size;
            break;
        }
    pthread_mutex_unlock(&mem_lock);
    return base;
}

int guest_free(uint32_t addr, uint32_t *size_out) {
    pthread_mutex_lock(&mem_lock);
    for (int i = 0; i < nregions; i++)
        if (regions[i].base == addr) {
            plat_unmap((void *)(uintptr_t)addr, regions[i].size);
            if (in_arena(addr)) __atomic_sub_fetch(&arena_used, regions[i].size, __ATOMIC_RELAXED);
            if (size_out) *size_out = regions[i].size;
            regions[i] = regions[--nregions];
            pthread_mutex_unlock(&mem_lock);
            return 0;
        }
    pthread_mutex_unlock(&mem_lock);
    return -1;
}

uint32_t contig_alloc(uint32_t size, uint32_t align, uint32_t lowest, uint32_t highest) {
    if (align < PAGE) align = PAGE;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    pthread_mutex_lock(&mem_lock);
    uint32_t a = (contig_next + align - 1) & ~(align - 1);
    if (a < CONTIG_BASE + lowest) a = (CONTIG_BASE + lowest + align - 1) & ~(align - 1);
    uint32_t r = 0;
    if (a + size <= CONTIG_BASE + CONTIG_SIZE && (highest == 0 || a + size - 1 - CONTIG_BASE <= highest)) {
        r = a;
        contig_next = a + size;
        if (ncontig < MAX_CONTIG) contig[ncontig++] = (Region){a, size};
    }
    pthread_mutex_unlock(&mem_lock);
    if (r) memset((void *)(uintptr_t)r, 0, size);
    if (r && size >= MEM_LARGE_ALLOC) dlog_crumb("large contiguous alloc %u KiB at %08x", size >> 10, r);
    /* The title probes with ever smaller sizes until one fits: one breadcrumb per run of failures. */
    static int failing;
    if (!r && !__atomic_exchange_n(&failing, 1, __ATOMIC_RELAXED))
        dlog_crumb("contiguous alloc of %u KiB failed (%u KiB of %u used)", size >> 10, contig_top() >> 10, CONTIG_SIZE >> 10);
    if (r) __atomic_store_n(&failing, 0, __ATOMIC_RELAXED);
    return r;
}

uint32_t contig_size(uint32_t addr) {
    uint32_t s = 0;
    pthread_mutex_lock(&mem_lock);
    for (int i = 0; i < ncontig; i++)
        if (addr >= contig[i].base && addr - contig[i].base < contig[i].size) s = contig[i].size;
    pthread_mutex_unlock(&mem_lock);
    return s;
}

uint32_t contig_top(void) { return __atomic_load_n(&contig_next, __ATOMIC_RELAXED) - CONTIG_BASE; }

int mem_is_mapped(uint32_t addr) { return plat_is_mapped(addr); }
