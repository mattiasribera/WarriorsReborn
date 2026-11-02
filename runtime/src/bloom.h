/* --bloom: the options and the rule that ends the 3D scene for the renderer's
 * bloom split (renderer_bloom). */
#ifndef BLOOM_H
#define BLOOM_H
#include <stdint.h>

#define BLOOM_STRENGTH_DEFAULT 0.6f
#define BLOOM_THRESHOLD_DEFAULT 0.75f
typedef struct {
    int on;            /* --bloom / --no-bloom */
    float strength;    /* --bloom-strength 0.1-3 */
    float threshold;   /* --bloom-threshold 0-1 */
} BloomConfig;
#define BLOOM_DEFAULTS {0, BLOOM_STRENGTH_DEFAULT, BLOOM_THRESHOLD_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a bloom option, -1 for a missing or out-of-range value (c unchanged). */
int bloom_arg(BloomConfig *c, const char *opt, const char *val);

/* A draw that belongs to the 3D scene: on the back buffer (not offscreen) with the depth test on
 * (BLOOM_DRAW_DEPTH_TEST = RENDERER_DEPTH_TEST). The split goes after the frame's last scene draw:
 * the title draws its HUD, text and menu overlays after the scene and without depth test. */
#define BLOOM_DRAW_DEPTH_TEST 1u
int bloom_scene_draw(int offscreen, uint32_t draw_flags);

/* Halo draws: the title fakes lamp halos and light cones with world-space
 * particle quads, centred on the lamp shade's rim; the shade depth-hides their upper half, and the
 * bright pass bloomed the visible lower half into a ball hanging under each lamp. They already are
 * the glow, so with bloom on they are drawn after the split (bloom_order): the bright pass never
 * sees them and they look as without bloom. Signature (render state, not code addresses):
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending with the ADD equation, depth test on, depth write off,
 * fixed-function FVF XYZ | DIFFUSE | TEX1 (0x142), triangle list. Measured in the hideout (level95)
 * and the level51 street: it matches the particle emitters' draws only (lamp halos and cones; in
 * level51 also haze plumes and light shafts, which hold no light of their own); the other
 * alpha-blended, depth-write-off draws there use FVF 0x152, a vertex program or triangle fans. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq;   /* RState values (NV2A enums) */
    uint32_t depth_test, depth_write;
    uint32_t fvf;    /* fixed-function FVF; 0 for a vertex program */
    uint32_t prim;   /* D3DPRIMITIVETYPE (Xbox): 5 = triangle list */
} BloomDraw;
#define BLOOM_HALO_FVF 0x142u
int bloom_halo_draw(const BloomDraw *d);

/* Replay order of a frame with the bloom split. kind[i] per operation: BLOOM_OP_OTHER, BLOOM_OP_SCENE
 * (bloom_scene_draw), BLOOM_OP_HALO (a scene draw that is also bloom_halo_draw) or BLOOM_OP_CLEAR (a
 * back-buffer clear). Writes the n operation indices to order and returns how many of them come
 * before the split (renderer_bloom after order[ret - 1]), 0 when the frame has no split. The split
 * follows the last non-halo scene draw; the halo draws before it (and after the frame's last
 * back-buffer clear before it, so they test against the same depth) move to right after the split,
 * in their order; everything else keeps its place. Without a scene draw the order is unchanged. */
enum { BLOOM_OP_OTHER, BLOOM_OP_SCENE, BLOOM_OP_HALO, BLOOM_OP_CLEAR };
uint32_t bloom_order(const uint8_t *kind, uint32_t n, uint32_t *order);
#endif
