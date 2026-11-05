/* Game-independent check of the --sim60 rate entries for counters that count world updates:
 * the litter (both pools), the airborne fail-safe and the combat
 * counters. A host buffer stands in for the guest image; every table entry is planted with its
 * own original bytes, the whole table is applied, and the sites must then hold the values that
 * keep each duration in game time with twice as many updates. */
#include "../src/hle_sim60.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

enum { IMG_SIZE = 0x005d5000 };
static uint8_t img[IMG_SIZE];

static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, img + va, 4); return v; }
static uint16_t rd16(uint32_t va) { uint16_t v; memcpy(&v, img + va, 2); return v; }
static float rdf(uint32_t va) { float v; memcpy(&v, img + va, 4); return v; }
static int has(uint32_t va, const uint8_t *b, size_t n) { return !memcmp(img + va, b, n); }
#define HAS(va, ...) has(va, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

/* The table entry that starts at va, or NULL. */
static const Sim60Patch *entry_at(uint32_t va) {
    for (int i = 0; i < sim60_count; i++)
        if (sim60_table[i].va == va) return &sim60_table[i];
    return NULL;
}

int main(void) {
    for (int i = 0; i < sim60_count; i++) memcpy(img + sim60_table[i].va, sim60_table[i].orig, sim60_table[i].len);
    char report[16384];
    int n = sim60_apply(sim60_table, sim60_count, 1, (intptr_t)(uintptr_t)img, report, sizeof report);
    CHECK(n == sim60_count && strstr(report, "skipped: none"), "title table applies to a planted image (%d entries)", n);

    /* Litter pool 1 (FUN_0008bc00, 64 pieces): every duration is counted in updates. */
    CHECK(img[0x8bd70] == 40, "litter: ground probe every 40 updates (%u)", img[0x8bd70]);
    CHECK(rd32(0x8ae05) == 1800 && rd32(0x8ae0a) == 1200, "litter: settle time random(1200, 1800) updates (%u, %u)",
          rd32(0x8ae0a), rd32(0x8ae05));
    CHECK(rd16(0x8be93) == 120 && rd16(0x8bf6f) == 120, "litter: lifetime 120 updates at both sites (%u, %u)",
          rd16(0x8be93), rd16(0x8bf6f));
    CHECK(HAS(0x8adc8, 0x6a, 60, 0x6a, 2) && HAS(0x8bf80, 0x6a, 60, 0x6a, 2), "litter: wind kick every random(2, 60) updates (spawn, refresh)");
    CHECK(HAS(0x8b79b, 0x6a, 120, 0x89, 0x48, 0x0c, 0x6a, 60), "litter: tumble blend random(60, 120) updates");
    CHECK(rd32(0x8bfd4) == 0x420774u && rdf(rd32(0x8bfda)) == 0.16333334f / 2,
          "litter: gravity per update halved (x %08x, + %g)", rd32(0x8bfd4), rdf(rd32(0x8bfda)));
    /* The kick impulses are velocities (units/s): with the kick interval doubled they must keep
     * the title's 1/30, so the old halving entries are gone. */
    CHECK(!entry_at(0x8b925) && !entry_at(0x8ba36), "litter: kick velocities keep 1/30 (no entry at 0x8b925 / 0x8ba36)");
    CHECK(entry_at(0x8c390) != NULL, "litter: position step stays 1/60 per update");

    /* Litter pool 2 (FUN_0008d0c0, 36 pieces). */
    CHECK(HAS(0x8cd6f, 0x6a, 120, 0x6a, 40) && HAS(0x8d1fa, 0x6a, 120, 0x6a, 40), "litter 2: kick every random(40, 120) updates");
    CHECK(HAS(0x8d094, 0x6a, 120, 0x89, 0x48, 0x0c, 0x6a, 60), "litter 2: sway blend random(60, 120) updates");
    CHECK(rd16(0x8d1eb) == 120, "litter 2: lifetime 120 updates (%u)", rd16(0x8d1eb));

    /* Airborne fail-safe (human update, 0x16b289): +0x344 counts airborne updates and the mover's
     * +0x68 (FUN_002503a0) counts stuck moves; both must reach 2 s. */
    CHECK(img[0x16b294] == 120 && img[0x16b2aa] == 120, "airborne fail-safe: 120 airborne and 120 stuck updates (%u, %u)",
          img[0x16b294], img[0x16b2aa]);

    if (failures) printf("%d FAILED\n", failures);
    else printf("ALL OK\n");
    return failures != 0;
}
