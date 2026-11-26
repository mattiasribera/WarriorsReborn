/* Texture cache LRU eviction: which cached textures to free when the cache is over its byte budget
 * or entry limit, or after a texture could not be created (with or without
 * RendererConfig.texture_streaming). Header-only and game-independent, used by
 * src/renderer/vk_renderer.cpp, tested by tests/texload_test.c. */
#ifndef TEX_LRU_H
#define TEX_LRU_H
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint64_t used;    /* frame serial of the last lookup or creation */
    uint64_t bytes;   /* device memory; 0 = a free slot */
} TexLruEntry;
typedef struct {
    uint64_t used;
    int i;
} TexLruPick;

static int tex_lru_cmp(const void *a, const void *b) {
    const TexLruPick *x = (const TexLruPick *)a, *y = (const TexLruPick *)b;
    return x->used < y->used ? -1 : x->used > y->used ? 1 : x->i - y->i;
}

/* Whether freeing `freed` bytes in `n` entries brings (total, live) to the targets. */
static inline int tex_lru_enough(uint64_t total, uint64_t target_bytes, int live, int target_live, uint64_t freed, int n) {
    return total - freed <= target_bytes && live - n <= target_live;
}

/* Least recently used entries (of the n in e) to free until the cache holds at most target_bytes
 * in at most target_live entries; entries used at or after keep_from are never picked (they may
 * be drawn by frames in flight or the frame being built). Writes the picks to out, oldest first
 * (scratch: n entries), *freed = their bytes; returns how many. Check tex_lru_enough: when even
 * every eligible entry is not enough, the caller drops the whole cache instead. */
static inline int tex_lru_pick(const TexLruEntry *e, int n, uint64_t total, uint64_t target_bytes, int live, int target_live,
                               uint64_t keep_from, TexLruPick *scratch, int *out, uint64_t *freed) {
    int m = 0, k = 0;
    *freed = 0;
    if (tex_lru_enough(total, target_bytes, live, target_live, 0, 0)) return 0;
    for (int i = 0; i < n; i++)
        if (e[i].bytes && e[i].used < keep_from) scratch[m].used = e[i].used, scratch[m++].i = i;
    qsort(scratch, (size_t)m, sizeof *scratch, tex_lru_cmp);
    while (k < m && !tex_lru_enough(total, target_bytes, live, target_live, *freed, k)) {
        out[k] = scratch[k].i;
        *freed += e[scratch[k].i].bytes;
        k++;
    }
    return k;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Test hooks (src/renderer/vk_renderer.cpp, used by tools/renderer_test.c; kept out of renderer.h,
 * the render64 wire API): the cache's entry limit (0 = the real one, MAX_TEXTURES), and the next n
 * renderer_texture_create calls failing at their memory allocation as with
 * VK_ERROR_OUT_OF_DEVICE_MEMORY. */
void renderer_texture_test_limit(uint32_t entries);
void renderer_texture_test_fail(uint32_t n);
#ifdef __cplusplus
}
#endif
#endif
