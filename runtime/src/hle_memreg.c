/* Memory registry size (see hle_memreg.h). */
#include "hle_memreg.h"
#include <stdio.h>
#include <string.h>

/* 0x2460a1-0x2460dd as shipped: allocate the pool object, construct it with 0x200 nodes
 * (0x246190), store it at [esi+0x14]. */
#define MEMREG_SIG_VA 0x002460a1u
static const uint8_t memreg_sig[] = {
    0x8b, 0x0d, 0x9c, 0x71, 0x5a, 0x00, 0x8b, 0x11, 0x6a, 0x00, 0x6a, 0x00, 0x68, 0x86, 0x00, 0x00,
    0x00, 0x68, 0x18, 0x6f, 0x43, 0x00, 0x6a, 0x10, 0x6a, 0x10, 0x68, 0x40, 0x6f, 0x43, 0x00, 0xff,
    0x52, 0x24, 0x85, 0xc0, 0x74, 0x0e, 0x68, 0x00, 0x02, 0x00, 0x00, 0x8b, 0xc8, 0xe8, 0xbd, 0x00,
    0x00, 0x00, 0xeb, 0x02, 0x33, 0xc0, 0x89, 0x46, 0x14};

int memreg_apply(intptr_t bias, char *report, size_t cap) {
    if (!cap) return 0;
    report[0] = 0;
    if (memcmp((const void *)(uintptr_t)(MEMREG_SIG_VA + bias), memreg_sig, sizeof memreg_sig)) {
        snprintf(report, cap, "[memreg] not patched: the registry set-up at %08x is not the shipped one; %d nodes",
                 MEMREG_SITE, MEMREG_NODES_SHIPPED);
        return 0;
    }
    uint32_t v = MEMREG_NODES;
    memcpy((void *)(uintptr_t)(MEMREG_IMM + bias), &v, 4);
    snprintf(report, cap, "[memreg] memory registry nodes at %08x: %d -> %d", MEMREG_SITE, MEMREG_NODES_SHIPPED,
             MEMREG_NODES);
    return 1;
}

int memreg_read(intptr_t bias, uint32_t *used, uint32_t *cap) {
    *used = *cap = 0;
    uint32_t pool = *(const volatile uint32_t *)(uintptr_t)(MEMREG_MANAGER_POOL + bias);
    if (!pool) return 0;
    *cap = *(const volatile uint32_t *)(uintptr_t)(pool + 8 + bias);
    *used = *(const volatile uint32_t *)(uintptr_t)(pool + 0xc + bias);
    return 1;
}

int hle_memreg_install(void) {
    char report[192];
    int n = memreg_apply(0, report, sizeof report);
    printf("%s\n", report);
    return n;
}
