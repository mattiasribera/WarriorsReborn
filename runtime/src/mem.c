#define _GNU_SOURCE
#include "mem.h"
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
}

void guest_track(uint32_t base, uint32_t size) {
    pthread_mutex_lock(&mem_lock);
    if (nregions < MAX_REGIONS) regions[nregions++] = (Region){base, size};
    pthread_mutex_unlock(&mem_lock);
}

static void *map_fixed(uint32_t addr, size_t size) { return plat_map_fixed(addr, size); }

void *guest_alloc_at(uint32_t addr, size_t size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    void *p = map_fixed(addr, size);
    if (p) guest_track(addr, size);
    return p;
}

void *guest_alloc(size_t size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    pthread_mutex_lock(&mem_lock);
    void *p = NULL;
    uint32_t gran = plat_alloc_granularity() - 1;
    while (!p && arena_next + size <= GUEST_ARENA_END) {
        p = map_fixed(arena_next, size);
        arena_next += p ? size + PAGE : 0x100000;   /* leave a guard gap */
        arena_next = (arena_next + gran) & ~gran;   /* Windows places allocations on 64 KB */
    }
    pthread_mutex_unlock(&mem_lock);
    if (!p) { fprintf(stderr, "guest arena exhausted (%zu bytes)\n", size); return NULL; }
    guest_track((uint32_t)(uintptr_t)p, size);
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
