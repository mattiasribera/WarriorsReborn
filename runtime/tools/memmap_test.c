/* Game-independent checks of guest memory placement (plat_map_fixed / plat_unmap / plat_is_mapped).
 * A range reserved by other code in the process must never take guest pages: on Windows the AMD
 * Vulkan driver (amdvlk32.dll) reserves 32 MB ranges and commits or frees pieces of them later, and
 * guest pages committed inside them were overwritten or released under the title. The "foreign"
 * reservation here is VirtualAlloc(MEM_RESERVE) on Windows and an mmap(PROT_NONE) on Linux. */
#include "../src/platform.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static uint32_t foreign_reserve(uint32_t addr, uint32_t size) {
    return (uint32_t)(uintptr_t)VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE, PAGE_READWRITE);
}
static uint32_t free_range(uint32_t size) {   /* an address nobody holds right now */
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
    VirtualFree(p, 0, MEM_RELEASE);
    return (uint32_t)(uintptr_t)p;
}
#else
#include <sys/mman.h>
static uint32_t foreign_reserve(uint32_t addr, uint32_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? 0 : (uint32_t)(uintptr_t)p;
}
static uint32_t free_range(uint32_t size) {
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    munmap(p, size);
    return (uint32_t)(uintptr_t)p;
}
#endif

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    /* One free 8 MB window: [w, w+1 MB) stays free, a foreign 4 MB reservation at w+1 MB, guest
     * pages at w+6 MB. */
    uint32_t g = plat_alloc_granularity(), w = (free_range(0x900000) + 0xFFFFF) & ~0xFFFFFu;
    uint32_t foreign = foreign_reserve(w + 0x100000, 0x400000);
    CHECK(foreign == w + 0x100000, "foreign 4 MB reservation at %08x", foreign);
    uint32_t inside = foreign + 0x100000;
    CHECK(plat_map_fixed(inside, 0x10000) == NULL, "no guest pages inside a foreign reservation (%08x)", inside);
#ifdef _WIN32   /* Linux plat_is_mapped is mincore, which counts PROT_NONE pages as mapped */
    CHECK(!plat_is_mapped(inside), "the foreign reserved page is not reported as mapped");
#endif

    uint32_t a = w + 0x600000;
    uint8_t *p = plat_map_fixed(a, 0x20000);
    CHECK(p == (uint8_t *)(uintptr_t)a, "map a free range at %08x", a);
    if (p) {
        memset(p, 0xA5, 0x20000);
        CHECK(plat_is_mapped(a) && plat_is_mapped(a + 0x1FFFF), "the mapped range is reported as mapped");
        plat_unmap(p, 0x20000);
        CHECK(!plat_is_mapped(a) && !plat_is_mapped(a + 0x1FFFF), "unmapped pages are not reported as mapped");
        p = plat_map_fixed(a, 0x20000);
        CHECK(p == (uint8_t *)(uintptr_t)a && p[0] == 0 && p[0x1FFFF] == 0, "map the same range again, zero-filled");
        if (p) plat_unmap(p, 0x20000);
    }

    /* A range that starts in free space and runs into the foreign reservation. */
    uint32_t edge = foreign - g;
    void *q = plat_map_fixed(edge, g + 0x10000);
    CHECK(q == NULL, "no range that runs into a foreign reservation (%08x)", edge);
    if (q) plat_unmap(q, g + 0x10000);
    q = plat_map_fixed(edge, g);
    CHECK(q == (void *)(uintptr_t)edge, "the free space just before it still maps (%08x)", edge);
    if (q) plat_unmap(q, g);

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
