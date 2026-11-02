/* --ssao: the options, the rule that ends the opaque 3D world for the
 * renderer's ambient occlusion split (renderer_ssao) and the projection scale it needs. */
#ifndef SSAO_H
#define SSAO_H
#include <stdint.h>

#define SSAO_STRENGTH_DEFAULT 0.8f
#define SSAO_STRENGTH_MIN 0.1f
#define SSAO_STRENGTH_MAX 2.0f
#define SSAO_RADIUS_DEFAULT 0.8f
#define SSAO_RADIUS_MIN 0.1f
#define SSAO_RADIUS_MAX 8.0f
typedef struct {
    int on;           /* --ssao / --no-ssao */
    float strength;   /* --ssao-strength 0.1-2 */
    float radius;     /* --ssao-radius 0.1-8 (view units: the title's world units) */
} SsaoConfig;
#define SSAO_DEFAULTS {0, SSAO_STRENGTH_DEFAULT, SSAO_RADIUS_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not an ssao option, -1 for a missing or out-of-range value (c unchanged). */
int ssao_arg(SsaoConfig *c, const char *opt, const char *val);

/* The split (measured in the hideout, a level51 street and its in-engine cutscene):
 * the title draws its opaque world (blending SRC_ALPHA / ONE_MINUS_SRC_ALPHA
 * enabled but depth written) in one run on the back buffer, then the depth-write-off effects
 * (particles, smoke, fire, lamp halos and cones), then the HUD, text and rain layers without depth
 * test. So the opaque world ends with the frame's last depth-tested, depth-writing back-buffer
 * draw: SSAO_OP_OPAQUE. kind[i] per operation: SSAO_OP_OTHER, SSAO_OP_OPAQUE or SSAO_OP_CLEAR (a
 * back-buffer clear of the depth). Returns the index of the operation after which the occlusion
 * runs (the last opaque draw), or n when the frame has none after its last depth clear (menus,
 * movies, a frame that clears the depth after its 3D: what is left in the depth is not the world). */
#define SSAO_DRAW_DEPTH_TEST 1u
enum { SSAO_OP_OTHER, SSAO_OP_OPAQUE, SSAO_OP_CLEAR };
int ssao_opaque_draw(int offscreen, uint32_t draw_flags, uint32_t depth_write);
uint32_t ssao_split(const uint8_t *kind, uint32_t n);

/* Projection scale (clip x = px * view x, clip y = py * view y) of a draw whose world-view-projection
 * matrix has the columns c0, c1 and c3 (clip x, y and w as dot products with the vertex, as the
 * title's vertex programs and the fixed-function transform compute them): with an orthonormal
 * (or uniformly scaled) world-view part, px = |c0.xyz| / |c3.xyz| and py = |c1.xyz| / |c3.xyz|,
 * matching the title's SetTransform projection. Returns 0 and out = {px, py} when both lie in
 * 0.2-20; -1 otherwise (out unchanged). */
int ssao_proj_scale(const float c0[4], const float c1[4], const float c3[4], float out[2]);
#define SSAO_PX_DEFAULT 1.3971f   /* the title's usual camera (hideout, level51) */
#define SSAO_PY_DEFAULT 1.9955f
#endif
