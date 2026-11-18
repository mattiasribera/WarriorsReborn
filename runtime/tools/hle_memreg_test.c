/* Game-independent memory-registry test: a host buffer stands in for the title's memory
 * manager set-up (MemoryPriv.cpp, 0x245f10). Checks that the node count pushed for the
 * registry's node pool (push 0x200 at 0x2460c7) becomes MEMREG_NODES with every other byte
 * untouched, that a second pass or a changed byte around the site writes nothing, and that the
 * used / capacity read-back follows the pool object behind [0x45f084]. */
#include "../src/hle_memreg.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

enum { BASE = 0x00246080u, SIZE = 0x100 };

/* 0x2460a1-0x2460dd as shipped: allocate the pool object (MemoryPriv.cpp line 0x86), construct
 * it with 0x200 nodes (0x246190), store it at [esi+0x14]. */
static const uint8_t shipped[] = {
    0x8b, 0x0d, 0x9c, 0x71, 0x5a, 0x00, 0x8b, 0x11, 0x6a, 0x00, 0x6a, 0x00, 0x68, 0x86, 0x00, 0x00,
    0x00, 0x68, 0x18, 0x6f, 0x43, 0x00, 0x6a, 0x10, 0x6a, 0x10, 0x68, 0x40, 0x6f, 0x43, 0x00, 0xff,
    0x52, 0x24, 0x85, 0xc0, 0x74, 0x0e, 0x68, 0x00, 0x02, 0x00, 0x00, 0x8b, 0xc8, 0xe8, 0xbd, 0x00,
    0x00, 0x00, 0xeb, 0x02, 0x33, 0xc0, 0x89, 0x46, 0x14};
#define SHIPPED_VA 0x002460a1u

static uint8_t mem[SIZE];

static void plant(void) {
    memset(mem, 0xcc, sizeof mem);
    memcpy(mem + (SHIPPED_VA - BASE), shipped, sizeof shipped);
}

static uint32_t imm_at(uint32_t va) {
    uint32_t v;
    memcpy(&v, mem + (va - BASE), 4);
    return v;
}

int main(void) {
    intptr_t bias = (intptr_t)((uintptr_t)mem - BASE);
    uint8_t before[SIZE];
    char report[256];

    CHECK(MEMREG_NODES > MEMREG_NODES_SHIPPED && MEMREG_NODES_SHIPPED == 0x200, "more nodes than the shipped 512 (%d)",
          MEMREG_NODES);
    CHECK(MEMREG_SITE + 1 == MEMREG_IMM && MEMREG_IMM == 0x002460c8u, "operand of push 0x200 at %08x", MEMREG_IMM);

    /* As shipped: only the pushed count changes. */
    plant();
    memcpy(before, mem, sizeof mem);
    int n = memreg_apply(bias, report, sizeof report);
    int changed = 0;
    for (int i = 0; i < SIZE; i++) changed += mem[i] != before[i];
    CHECK(n == 1 && imm_at(MEMREG_IMM) == MEMREG_NODES, "pool constructed with %u nodes (%s)", imm_at(MEMREG_IMM), report);
    CHECK(changed <= 4 && mem[MEMREG_SITE - BASE] == 0x68 && mem[MEMREG_IMM + 4 - BASE] == 0x8b,
          "only the operand changed (%d bytes), push and mov ecx,eax kept", changed);
    CHECK(strstr(report, "[memreg]") && strstr(report, "512") && strstr(report, "002460c7"), "report names site and counts");

    /* Second pass sees the raised count, not the shipped bytes: refused, untouched. */
    memcpy(before, mem, sizeof mem);
    CHECK(memreg_apply(bias, report, sizeof report) == 0 && !memcmp(before, mem, sizeof mem) && strstr(report, "not patched"),
          "second pass refused (%s)", report);

    /* One changed byte anywhere in the checked bytes: nothing written. */
    for (uint32_t off = 0; off < sizeof shipped; off += 7) {
        plant();
        mem[SHIPPED_VA + off - BASE] ^= 0x5a;
        memcpy(before, mem, sizeof mem);
        int r = memreg_apply(bias, report, sizeof report);
        CHECK(r == 0 && !memcmp(before, mem, sizeof mem), "byte %08x differs: untouched", SHIPPED_VA + off);
    }

    /* Read-back: [0x45f084] -> pool {nodes, free stack, capacity, used}. */
    static uint8_t data[0x40];
    static uint32_t pool[4];
    intptr_t dbias = (intptr_t)((uintptr_t)data - 0x0045f070u);
    uint32_t used = 9, cap = 9;
    memset(data, 0, sizeof data);
    CHECK(memreg_read(dbias, &used, &cap) == 0 && used == 0 && cap == 0, "no pool yet: nothing read");
    pool[2] = MEMREG_NODES;
    pool[3] = 731;
    uint32_t p = (uint32_t)((uintptr_t)pool - (uintptr_t)dbias);
    memcpy(data + (MEMREG_MANAGER_POOL - 0x0045f070u), &p, 4);
    CHECK(memreg_read(dbias, &used, &cap) == 1 && used == 731 && cap == MEMREG_NODES, "pool read: %u of %u", used, cap);

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
