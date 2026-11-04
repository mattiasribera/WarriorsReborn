/* --soft-particles: the options, the signature of the title's
 * particle draws and the per-quad rule that picks the quads the renderer softens
 * (renderer_soft, RENDERER_SOFT). */
#ifndef SOFT_PARTICLES_H
#define SOFT_PARTICLES_H
#include <stddef.h>
#include <stdint.h>

#define SOFT_DISTANCE_DEFAULT 0.5f
#define SOFT_DISTANCE_MIN 0.05f
#define SOFT_DISTANCE_MAX 4.0f
typedef struct {
    int on;           /* --soft-particles / --no-soft-particles */
    float distance;   /* --soft-particles-distance 0.05-4 (view units: the title's world units) */
} SoftConfig;
#define SOFT_DEFAULTS {0, SOFT_DISTANCE_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a soft-particles option, -1 for a missing or out-of-range value (c unchanged). */
int soft_arg(SoftConfig *c, const char *opt, const char *val);

/* The title's world particle draws (~/.cache/fx-study/FINDINGS.md, the world particle pass
 * 0xa4e50): CPU-built quads, fixed-function FVF XYZ | DIFFUSE | TEX1 (0x142, 24-byte vertices),
 * an indexed triangle list (Xbox D3DPT_TRIANGLELIST = 5), on the back buffer with the depth test
 * on and depth writes off, blended with SRC_ALPHA or ONE onto ONE_MINUS_SRC_ALPHA or ONE (ADD):
 * smoke, dust, fire, sparks, lamp halos and cones. The glow/overlay pass (0xa50b0) draws without
 * depth test and never matches. */
typedef struct {
    int offscreen;
    uint32_t fvf;      /* fixed-function FVF; 0 for a vertex program */
    uint32_t prim;     /* D3DPRIMITIVETYPE (Xbox) */
    uint32_t stride;   /* stream 0 */
    uint32_t indexed;
    uint32_t depth_test, depth_write;
    uint32_t blend_enable, blend_src, blend_dst, blend_eq;   /* RState values (NV2A enums) */
} SoftDraw;
#define SOFT_FVF 0x142u
#define SOFT_STRIDE 24u
#define SOFT_PRIM_TRIANGLELIST 5u
int soft_particle_draw(const SoftDraw *d);

/* The camera of a particle draw: clip x = px * view x, clip y = py * view y, clip w = zsign *
 * view z (zsign = projection m[2][3], +1 or -1), and world up (+z) in view space (has_up; the
 * view matrix's third row). */
typedef struct {
    float px, py, zsign;
    float up[3];
    int has_up;
} SoftView;
/* Soft quads: a quad is softened only when it faces the camera, so the fade along the view ray
 * meets the scene behind it as a crossing, not as a surface it lies on. Its plane's normal (from
 * its first triangle, in view space) must be within acos(SOFT_FACING_MIN) of the view axis (the
 * title's billboards are aligned to the view plane: measured in level51, their tilt from world up
 * is the camera's pitch) or of the ray from the eye to its centre, and, with the world up known,
 * not within acos(SOFT_LEVEL_MAX) of horizontal: ground decals (blood, scorch marks, the rain's
 * ripples) lie on the floor and would fade into it (also when the camera looks steeply down on
 * them). Degenerate quads are not softened. */
#define SOFT_FACING_MIN 0.9f    /* about 26 degrees */
#define SOFT_LEVEL_MAX 0.866f   /* within 30 degrees of horizontal */
/* Classifies the quads of an indexed triangle list (six indices per quad; xyzw: the clip x, y, z,
 * w of vertex k at xyzw + k * stride floats): soft[q] = 1 for a soft quad, else 0. Returns the
 * quad count, or 0 when ni is not a multiple of 6 (not a quad batch: nothing softened). */
uint32_t soft_classify(const float *xyzw, size_t stride, const uint32_t *idx, uint32_t ni, const SoftView *v, uint8_t *soft);
#endif
