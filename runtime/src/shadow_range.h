/* --shadow-range: the options and the fit of the light projection the
 * renderer needs to measure, in the light's view units, how far a shadow receiver lies behind the
 * caster depth stored in the title's character shadow map (RendererConfig.shadow_range,
 * RPixelShader.shadow_depth). */
#ifndef SHADOW_RANGE_H
#define SHADOW_RANGE_H
#include <stddef.h>
#include <stdint.h>

#define SHADOW_RANGE_DIST_DEFAULT 3.0f
#define SHADOW_RANGE_DIST_MIN 0.5f
#define SHADOW_RANGE_DIST_MAX 50.0f
typedef struct {
    int on;       /* --shadow-range / --no-shadow-range */
    float dist;   /* --shadow-range-dist 0.5-50 (light view units: the title's world units) */
} ShadowRangeConfig;
#define SHADOW_RANGE_DEFAULTS {0, SHADOW_RANGE_DIST_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a shadow-range option, -1 for a missing or out-of-range value (c unchanged). */
int shadow_range_arg(ShadowRangeConfig *c, const char *opt, const char *val);

/* Fits r = A q - B to the projective coordinates (r, q) of a draw's depth stage: n vertices, rq
 * pointing at the first vertex's r with q right after it, stride floats from one vertex to the
 * next. A perspective light's projection gives exactly that relation in any object space (its r
 * row is A x its q row minus B x the w row), so A and B convert a stored depth d to the view
 * depth B / (A - d). Returns 0 and ab = {A, B} for at least two vertices whose q spans more than
 * 1e-4 of its size, all q > 0, A and B > 0, and every vertex within 16 depth units of the fit;
 * -1 otherwise (ab unchanged): orthographic lights, flat or degenerate draws. */
int shadow_range_fit(const float *rq, size_t stride, uint32_t n, float ab[2]);
#endif
