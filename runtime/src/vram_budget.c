/* Adaptive texture-pack cache cap (see vram_budget.h). */
#include "vram_budget.h"

#include <string.h>

void vb_init(VramBudget *b, uint64_t max_cap) {
    memset(b, 0, sizeof *b);
    b->max_cap = max_cap ? max_cap : 1024 * VB_MIB;
    b->floor = b->max_cap < VB_FLOOR ? b->max_cap : VB_FLOOR;
    b->cap = b->min_cap_seen = b->max_cap;
}

uint64_t vb_target(uint64_t heap_budget, uint64_t heap_usage, uint64_t cache_bytes) {
    uint64_t other = heap_usage > cache_bytes ? heap_usage - cache_bytes : 0;
    if (other >= heap_budget) return 0;
    uint64_t avail = heap_budget - other, reserve = avail / 4 > VB_MIN_RESERVE ? avail / 4 : VB_MIN_RESERVE;
    return avail > reserve ? avail - reserve : 0;
}

/* The target clamped to [floor, max cap], rounded down to a step (the maximum and the floor as
 * given). */
static uint64_t clamp_cap(const VramBudget *b, uint64_t t) {
    if (t >= b->max_cap) return b->max_cap;
    t -= t % VB_STEP;
    return t > b->floor ? t : b->floor;
}

uint64_t vb_update(VramBudget *b, uint64_t heap_budget, uint64_t heap_usage, uint64_t cache_bytes) {
    if (!heap_budget) return b->cap;
    b->samples++;
    b->last_budget = heap_budget;
    b->last_usage = heap_usage;
    if (b->cooldown) b->cooldown--;
    const uint64_t target = vb_target(heap_budget, heap_usage, cache_bytes);
    const uint64_t shrink_margin = b->cap / 16 > VB_STEP ? b->cap / 16 : VB_STEP;
    const uint64_t grow_step = b->cap / 8 > 2 * VB_STEP ? b->cap / 8 : 2 * VB_STEP;
    const int over = heap_usage > heap_budget;
    if (target + shrink_margin < b->cap && b->cap > b->floor && (!b->cooldown || over)) {
        b->grow_streak = 0;
        uint64_t cap = clamp_cap(b, target);
        if (cap < b->cap) {
            b->cap = cap;
            b->shrinks++;
            if (cache_bytes >= cap) {   /* the renderer drops the cache at its next frame */
                b->drop_shrinks++;
                b->cooldown = VB_SHRINK_COOLDOWN;
            }
        }
    } else if (target > b->cap + grow_step && b->cap < b->max_cap) {
        if (++b->grow_streak >= VB_GROW_SAMPLES) {
            b->grow_streak = 0;
            b->cap = clamp_cap(b, target);
            b->grows++;
        }
    } else
        b->grow_streak = 0;
    if (b->cap < b->min_cap_seen) b->min_cap_seen = b->cap;
    return b->cap;
}
