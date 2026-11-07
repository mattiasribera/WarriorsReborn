/* Adaptive texture-pack cache cap from the driver's memory budget (VK_EXT_memory_budget).
 *
 * The renderer drops its whole texture cache when the cache holds more than its cap
 * (renderer_set_texture_budget). With a texture pack the cap is 1 GiB (or --texpack-budget MB);
 * when the driver reports how much device-local memory this process may use (budget) and uses
 * (usage), the cap follows what is left for textures:
 *
 *   other  = usage - cache bytes          (render targets, effects, buffers, the driver's own)
 *   avail  = budget - other
 *   target = avail - max(avail / 4, 128 MiB)   (a quarter, at least 128 MiB, stays free)
 *   cap    = clamp(target, floor, max cap), in 32 MiB steps
 *
 * Subtracting the cache's own bytes keeps the cap from chasing itself (a full cache does not
 * lower the cap that holds it). Hysteresis, so the cache never thrashes: the cap shrinks only
 * when the target is more than a sixteenth of the cap (at least 32 MiB) below it, and then at
 * once (the renderer drops the cache at the next frame if it no longer fits); it grows only after
 * VB_GROW_SAMPLES samples in a row a step above it (an eighth of the cap, at least 64 MiB). A
 * shrink that leaves the cache over the cap (a drop) is followed by at least VB_SHRINK_COOLDOWN
 * samples without another unless usage is over budget. Without the extension nothing is sampled
 * and the cap stays the maximum. Pure logic, no Vulkan: tests/vram_budget_test.c. */
#ifndef VRAM_BUDGET_H
#define VRAM_BUDGET_H

#include <stdint.h>

#define VB_MIB (1ull << 20)
#define VB_STEP (32 * VB_MIB)          /* cap granularity */
#define VB_MIN_RESERVE (128 * VB_MIB)  /* kept free of the available memory, at least */
#define VB_FLOOR (128 * VB_MIB)        /* the cap never goes below (or the maximum, if smaller) */
#define VB_GROW_SAMPLES 3              /* samples in a row above the cap before it grows */
#define VB_SHRINK_COOLDOWN 5           /* samples after a dropping shrink before the next one */

typedef struct VramBudget {
    uint64_t max_cap, floor, cap;
    uint64_t min_cap_seen;             /* the smallest cap so far */
    uint64_t last_budget, last_usage;  /* the last sample's driver numbers */
    uint32_t samples, shrinks, grows, drop_shrinks;
    uint32_t grow_streak, cooldown;
} VramBudget;

/* Starts at the maximum cap (bytes; 0 = 1 GiB). */
void vb_init(VramBudget *b, uint64_t max_cap);
/* The cache size the numbers leave room for (before clamping and hysteresis). */
uint64_t vb_target(uint64_t heap_budget, uint64_t heap_usage, uint64_t cache_bytes);
/* One sample: the device-local heap's budget and this process's usage of it, and the bytes the
 * texture cache holds now (counted in usage). heap_budget 0 = no numbers: the cap stays. Returns
 * the cap. */
uint64_t vb_update(VramBudget *b, uint64_t heap_budget, uint64_t heap_usage, uint64_t cache_bytes);

#endif
