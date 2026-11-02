/* --bloom options and the scene/HUD split rule (bloom.h). */
#include "bloom.h"
#include <stdlib.h>
#include <string.h>

static int parse_float(const char *v, float lo, float hi, float *out) {
    char *end;
    if (!v || !*v) return -1;
    float f = (float)strtod(v, &end);   /* compared as float: "0.1" is exactly the 0.1f bound */
    if (*end || !(f >= lo && f <= hi)) return -1;   /* NaN fails the range check */
    *out = f;
    return 0;
}

int bloom_arg(BloomConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--bloom") || !strcmp(opt, "--no-bloom")) {
        c->on = opt[2] == 'b';
        return 1;
    }
    if (!strcmp(opt, "--bloom-strength")) return parse_float(val, 0.1f, 3.0f, &c->strength) ? -1 : 2;
    if (!strcmp(opt, "--bloom-threshold")) return parse_float(val, 0.0f, 1.0f, &c->threshold) ? -1 : 2;
    return 0;
}

int bloom_scene_draw(int offscreen, uint32_t draw_flags) { return !offscreen && (draw_flags & BLOOM_DRAW_DEPTH_TEST); }

int bloom_halo_draw(const BloomDraw *d) {
    const int add = !d->blend_eq || d->blend_eq == 0x8006 || d->blend_eq == 0xF006;   /* unset: ADD (the NV2A reset value) */
    return d->blend_enable && d->blend_src == 0x302 && d->blend_dst == 0x303 && add && d->depth_test && !d->depth_write &&
           d->fvf == BLOOM_HALO_FVF && d->prim == 5;
}

uint32_t bloom_order(const uint8_t *kind, uint32_t n, uint32_t *order) {
    uint32_t split = n, clear = 0, k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (kind[i] == BLOOM_OP_SCENE) split = i;
    if (split == n) {
        for (uint32_t i = 0; i < n; i++) order[i] = i;
        return 0;
    }
    for (uint32_t i = 0; i < split; i++)
        if (kind[i] == BLOOM_OP_CLEAR) clear = i + 1;   /* halos before it test against an earlier depth */
    for (uint32_t i = 0; i <= split; i++)
        if (kind[i] != BLOOM_OP_HALO || i < clear) order[k++] = i;
    const uint32_t at = k;
    for (uint32_t i = clear; i < split; i++)
        if (kind[i] == BLOOM_OP_HALO) order[k++] = i;
    for (uint32_t i = split + 1; i < n; i++) order[k++] = i;
    return at;
}
