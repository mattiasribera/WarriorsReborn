/* Game-independent test of the adaptive texture-pack cache cap (src/vram_budget.c): the target
 * from the driver's budget and usage, the 1 GiB / --texpack-budget maximum and the floor, no
 * samples without the extension, the cache's own bytes not lowering its cap, shrinking at once
 * under pressure, hysteresis against jitter, the cooldown after a dropping shrink (overridden
 * when usage is over budget), and growth only after several samples in a row. */
#include "../src/vram_budget.h"
#include <stdio.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define MB VB_MIB
#define GB (1024 * MB)

int main(void) {
    VramBudget b;

    /* Maximum and floor. */
    vb_init(&b, 0);
    CHECK(b.cap == GB && b.max_cap == GB && b.floor == VB_FLOOR, "default maximum 1 GiB (cap %llu MiB)", (unsigned long long)(b.cap / MB));
    vb_init(&b, 256 * MB);
    CHECK(b.cap == 256 * MB && b.floor == VB_FLOOR, "--texpack-budget 256: cap 256 MiB");
    vb_init(&b, 64 * MB);
    CHECK(b.cap == 64 * MB && b.floor == 64 * MB, "a maximum under the floor is the floor too");

    /* Target. */
    CHECK(vb_target(2048 * MB, 600 * MB, 100 * MB) == 1161 * MB, "target: avail 1548 - 387 = %llu MiB",
          (unsigned long long)(vb_target(2048 * MB, 600 * MB, 100 * MB) / MB));
    CHECK(vb_target(1000 * MB, 700 * MB, 0) == 172 * MB, "target: at least 128 MiB stays free");
    CHECK(vb_target(1000 * MB, 1100 * MB, 50 * MB) == 0, "target: other usage over the budget leaves 0");
    CHECK(vb_target(1000 * MB, 100 * MB, 200 * MB) == 750 * MB, "target: usage under the cache's own bytes counts as no other usage");

    /* No extension: nothing changes. */
    vb_init(&b, 0);
    CHECK(vb_update(&b, 0, 0, 0) == GB && b.samples == 0, "no budget numbers: cap stays 1 GiB, no sample");

    /* A 6 GB card: the cap stays at the maximum. */
    vb_init(&b, 0);
    for (int i = 0; i < 10; i++) vb_update(&b, 5800 * MB, (900 + i * 50) * MB, (300 + i * 50) * MB);
    CHECK(b.cap == GB && b.shrinks == 0 && b.grows == 0 && b.samples == 10, "large budget: 1 GiB, no changes (%u samples)", b.samples);
    CHECK(b.last_budget == 5800 * MB && b.last_usage == 1350 * MB, "the last sample's numbers are kept");

    /* The cache filling up (usage grows with it) does not lower its own cap. */
    vb_init(&b, 0);
    vb_update(&b, 1536 * MB, 400 * MB, 0);   /* other 400: avail 1136, target 852 -> 832 */
    CHECK(b.cap == 832 * MB && b.shrinks == 1, "pressure: shrinks at once, 32 MiB steps (%llu MiB)", (unsigned long long)(b.cap / MB));
    CHECK(b.drop_shrinks == 0, "a shrink above the cache's bytes is not a drop");
    for (uint64_t c = 0; c <= 800 * MB; c += 100 * MB) vb_update(&b, 1536 * MB, 400 * MB + c, c);
    CHECK(b.cap == 832 * MB && b.shrinks == 1 && b.grows == 0, "cache growing to 800 MiB keeps the cap");

    /* Jitter: other usage +-40 MiB does not move the cap. */
    vb_init(&b, 0);
    vb_update(&b, 1536 * MB, 400 * MB, 0);
    for (int i = 0; i < 20; i++) vb_update(&b, 1536 * MB, (i & 1 ? 440 : 360) * MB + 500 * MB, 500 * MB);
    CHECK(b.cap == 832 * MB && b.shrinks == 1 && b.grows == 0, "jitter: no shrink or grow (cap %llu MiB, %u shrinks, %u grows)",
          (unsigned long long)(b.cap / MB), b.shrinks, b.grows);

    /* A dropping shrink, then the cooldown. */
    vb_init(&b, 0);
    vb_update(&b, 1536 * MB, 400 * MB + 800 * MB, 800 * MB);   /* cap 832, cache 800 */
    vb_update(&b, 1536 * MB, 500 * MB + 800 * MB, 800 * MB);   /* other 500: avail 1036, target 777 -> 768 */
    CHECK(b.cap == 768 * MB && b.shrinks == 2 && b.drop_shrinks == 1, "shrink under the cache's bytes counts as a drop (cap %llu)",
          (unsigned long long)(b.cap / MB));
    for (int i = 0; i < VB_SHRINK_COOLDOWN - 1; i++) vb_update(&b, 1536 * MB, 600 * MB, 0);   /* target 702 */
    CHECK(b.cap == 768 * MB && b.shrinks == 2, "cooldown: no shrink for %d samples after a drop", VB_SHRINK_COOLDOWN);
    vb_update(&b, 1536 * MB, 600 * MB, 0);
    CHECK(b.cap == 672 * MB && b.shrinks == 3, "after the cooldown it shrinks (cap %llu)", (unsigned long long)(b.cap / MB));

    /* Over budget overrides the cooldown. */
    vb_init(&b, 0);
    vb_update(&b, 1536 * MB, 1200 * MB, 800 * MB);   /* 832 */
    vb_update(&b, 1536 * MB, 1300 * MB, 800 * MB);   /* 768, drop */
    vb_update(&b, 1536 * MB, 1600 * MB, 400 * MB);   /* other 1200: avail 336, target 208 -> 192; over budget */
    CHECK(b.cap == 192 * MB && b.shrinks == 3, "usage over budget shrinks during the cooldown (cap %llu)", (unsigned long long)(b.cap / MB));

    /* Floor. */
    vb_init(&b, 0);
    vb_update(&b, 600 * MB, 550 * MB, 0);
    CHECK(b.cap == VB_FLOOR && b.min_cap_seen == VB_FLOOR, "a tiny budget stops at the 128 MiB floor");

    /* Growth needs VB_GROW_SAMPLES samples in a row. */
    vb_init(&b, 0);
    vb_update(&b, 1536 * MB, 400 * MB, 0);   /* 832 */
    vb_update(&b, 3000 * MB, 400 * MB, 0);
    vb_update(&b, 3000 * MB, 400 * MB, 0);
    vb_update(&b, 1536 * MB, 400 * MB, 0);   /* streak broken */
    vb_update(&b, 3000 * MB, 400 * MB, 0);
    vb_update(&b, 3000 * MB, 400 * MB, 0);
    CHECK(b.cap == 832 * MB && b.grows == 0, "an interrupted streak does not grow");
    vb_update(&b, 3000 * MB, 400 * MB, 0);
    CHECK(b.cap == GB && b.grows == 1, "%d samples in a row grow, up to the maximum (cap %llu)", VB_GROW_SAMPLES,
          (unsigned long long)(b.cap / MB));
    CHECK(b.min_cap_seen == 832 * MB, "smallest cap seen 832 MiB");

    /* A small rise (under a step) never grows. */
    vb_init(&b, 0);
    vb_update(&b, 1536 * MB, 400 * MB, 0);   /* target 852, cap 832 */
    for (int i = 0; i < 10; i++) vb_update(&b, 1580 * MB, 400 * MB, 0);   /* target 885 */
    CHECK(b.cap == 832 * MB && b.grows == 0, "a rise under the grow step keeps the cap");

    /* The --texpack-budget maximum holds when there is room. */
    vb_init(&b, 256 * MB);
    for (int i = 0; i < 5; i++) vb_update(&b, 5800 * MB, 900 * MB, 200 * MB);
    CHECK(b.cap == 256 * MB && b.shrinks == 0 && b.grows == 0, "--texpack-budget 256 on a large card stays 256");

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
