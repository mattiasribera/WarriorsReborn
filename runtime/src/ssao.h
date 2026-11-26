/* --ssao: the options, the rule that ends the opaque 3D world for the
 * renderer's ambient occlusion split (renderer_ssao) and the projection scale it needs. */
#ifndef SSAO_H
#define SSAO_H
#include <stdint.h>
#include "renderer.h" /* RContact */

#define SSAO_STRENGTH_DEFAULT 0.8f
#define SSAO_STRENGTH_MIN 0.1f
#define SSAO_STRENGTH_MAX 2.0f
#define SSAO_RADIUS_DEFAULT 0.8f
#define SSAO_RADIUS_MIN 0.1f
#define SSAO_RADIUS_MAX 8.0f
/* --ssao-mode: the classic obscurance passes (ssao_ao.frag) or XeGTAO;
 * --gtao-quality: XeGTAO's preset (slices x steps per side, gtao_slices / gtao_steps). */
enum { SSAO_MODE_CLASSIC, SSAO_MODE_GTAO };
enum { GTAO_QUALITY_LOW, GTAO_QUALITY_MEDIUM, GTAO_QUALITY_HIGH };
#define GTAO_QUALITY_DEFAULT GTAO_QUALITY_HIGH
/* --ssgi: one diffuse bounce of the frame's lit colour on top of XeGTAO's passes
 * (visibility-bitmask slices, temporal accumulation, depth-aware denoise); 0 = off. */
enum { SSGI_OFF, SSGI_LOW, SSGI_MEDIUM, SSGI_HIGH };
#define SSGI_STRENGTH_DEFAULT 1.0f
#define SSGI_STRENGTH_MIN 0.1f
#define SSGI_STRENGTH_MAX 3.0f
typedef struct {
    int on;           /* --ssao / --no-ssao */
    float strength;   /* --ssao-strength 0.1-2 */
    float radius;     /* --ssao-radius 0.1-8 (view units: the title's world units) */
    int mode;         /* --ssao-mode classic|gtao (SSAO_MODE_*) */
    int quality;      /* --gtao-quality low|medium|high (GTAO_QUALITY_*) */
    int ssgi;         /* --ssgi off|low|medium|high (SSGI_*): needs --ssao --ssao-mode gtao */
    float ssgi_strength;   /* --ssgi-strength 0.1-3 */
} SsaoConfig;
#define SSAO_DEFAULTS {0, SSAO_STRENGTH_DEFAULT, SSAO_RADIUS_DEFAULT, SSAO_MODE_CLASSIC, GTAO_QUALITY_DEFAULT, SSGI_OFF, SSGI_STRENGTH_DEFAULT}

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

/* Working size of the renderer's occlusion (renderer_ssao): half the output
 * (presented) size whatever the supersampling, so supersample 2 does not quadruple its cost.
 * iw x ih is the internal size (the output size times ss, the supersampling factor; below 1 counts
 * as 1). scale = 2 x ss internal pixels per texel, w x h the texel counts rounded up (at least 1),
 * and texel (i, j) stands for internal pixel (scale i + offset, scale j + offset): offset =
 * scale / 2 - 1, the top-left of the block's central 2x2 (0 at scale 2, as before the cap). */
typedef struct {
    int scale, w, h, offset;
} SsaoWorkSize;
static inline SsaoWorkSize ssao_work_size(int iw, int ih, int ss) {
    SsaoWorkSize r;
    r.scale = 2 * (ss > 1 ? ss : 1);
    r.w = iw > 0 ? (iw + r.scale - 1) / r.scale : 1;
    r.h = ih > 0 ? (ih + r.scale - 1) / r.scale : 1;
    r.offset = r.scale / 2 - 1;
    return r;
}

/* XeGTAO (Intel, MIT; GameTechDev/XeGTAO a5b1686c, XeGTAO.h GTAOUpdateConstants) in our terms, for
 * the renderer's gtao passes (shaders/gtao_*.comp) at the working size ws of an iw x ih internal
 * image. XeGTAO's view depth depth_unpack[0] / (depth_unpack[1] - NDC z) is ours, B / (A - z); its
 * tangents of the half field of view are 1 / px, 1 / py (clip x = px view x); ndc_to_view_mul/add
 * take a [0, 1] screen position (y down) and a view depth to view x, y (y up, z forward: the
 * classic pass's view_at); texel_view is ndc_to_view_mul times one working texel (ws.scale internal
 * pixels). A working texel (i, j) stands for internal pixel (scale i + offset, scale j + offset)
 * (ssao_work_size), so its screen position is ((scale i + offset + 0.5) / iw, ...): screen_mul,
 * screen_add. effect_radius is radius times XeGTAO's auto-tuned multiplier; the other heuristics
 * are its defaults. */
#define GTAO_RADIUS_MULTIPLIER 1.457f
#define GTAO_FALLOFF_RANGE 0.615f
#define GTAO_SAMPLE_DISTRIBUTION_POWER 2.0f
#define GTAO_THIN_OCCLUDER_COMPENSATION 0.0f
#define GTAO_FINAL_VALUE_POWER 2.2f
#define GTAO_DEPTH_MIP_SAMPLING_OFFSET 3.30f
#define GTAO_DENOISE_BLUR_BETA 1.2f
#define GTAO_MIP_LEVELS 5
typedef struct {
    float depth_unpack[2], tan_half_fov[2], ndc_to_view_mul[2], ndc_to_view_add[2], texel_view[2];
    float screen_mul[2], screen_add[2];
    float effect_radius, falloff_range, sample_power, thin_compensation, final_power, mip_offset;
} GtaoConsts;
static inline void gtao_constants(float depth_a, float depth_b, float px, float py, SsaoWorkSize ws, int iw, int ih, float radius,
                                  GtaoConsts *out) {
    const float tx = 1.0f / (px > 0 ? px : 1.0f), ty = 1.0f / (py > 0 ? py : 1.0f);
    const float w = (float)(iw > 0 ? iw : 1), h = (float)(ih > 0 ? ih : 1);
    GtaoConsts k = {{depth_b, depth_a}, {tx, ty}, {tx * 2.0f, ty * -2.0f}, {-tx, ty}, {tx * 2.0f * (float)ws.scale / w, ty * -2.0f * (float)ws.scale / h},
                    {(float)ws.scale / w, (float)ws.scale / h}, {((float)ws.offset + 0.5f) / w, ((float)ws.offset + 0.5f) / h},
                    radius * GTAO_RADIUS_MULTIPLIER, GTAO_FALLOFF_RANGE, GTAO_SAMPLE_DISTRIBUTION_POWER, GTAO_THIN_OCCLUDER_COMPENSATION,
                    GTAO_FINAL_VALUE_POWER, GTAO_DEPTH_MIP_SAMPLING_OFFSET};
    *out = k;
}
/* The view position (x right, y up, z = d forward) the gtao passes give working texel (i, j) at view
 * depth d: XeGTAO_ComputeViewspacePosition at the texel's screen position (gtao_common.glsl gtao_view_pos). */
static inline void gtao_view_pos(const GtaoConsts *k, int i, int j, float d, float out[3]) {
    const float u = (float)i * k->screen_mul[0] + k->screen_add[0], v = (float)j * k->screen_mul[1] + k->screen_add[1];
    out[0] = (k->ndc_to_view_mul[0] * u + k->ndc_to_view_add[0]) * d;
    out[1] = (k->ndc_to_view_mul[1] * v + k->ndc_to_view_add[1]) * d;
    out[2] = d;
}
static inline int gtao_slices(int quality) { return quality <= GTAO_QUALITY_LOW ? 1 : quality == GTAO_QUALITY_MEDIUM ? 2 : 3; }
static inline int gtao_steps(int quality) { return quality <= GTAO_QUALITY_MEDIUM ? 2 : 3; }
/* --ssgi's presets (shaders/ssgi_main.comp): slices x steps per side and the reach (world units) the
 * bounce light is gathered from. */
static inline int ssgi_slices(int q) { return q <= SSGI_LOW ? 2 : q == SSGI_MEDIUM ? 2 : 3; }
static inline int ssgi_steps(int q) { return q <= SSGI_LOW ? 4 : q == SSGI_MEDIUM ? 6 : 8; }
static inline float ssgi_radius(int q) { return q <= SSGI_LOW ? 2.5f : q == SSGI_MEDIUM ? 3.0f : 3.5f; }

/* --contact-shadows: short occlusion from above on the ground right
 * under and around characters and props (renderer_contact), at the end of the opaque world. */
#define CONTACT_STRENGTH_DEFAULT 0.6f
#define CONTACT_STRENGTH_MIN 0.1f
#define CONTACT_STRENGTH_MAX 2.0f
#define CONTACT_RADIUS 0.6f   /* world units the rays reach above the ground (about a shin's height) */
typedef struct {
    int on;           /* --contact-shadows / --no-contact-shadows */
    float strength;   /* --contact-shadows-strength 0.1-2 */
} ContactConfig;
#define CONTACT_DEFAULTS {0, CONTACT_STRENGTH_DEFAULT}
/* Parses one option as ssao_arg. */
int contact_arg(ContactConfig *c, const char *opt, const char *val);
/* The frame's contact shadows from its camera (D3D row-vector view and projection matrices: a
 * perspective projection, proj[11] = +-1, proj[15] = 0, and an orthonormal view rotation; world
 * up is +z, the title's world, given in view space) and depth projection ab (NULL: the defaults
 * below). Returns 1 and out, or 0 (out unchanged) when off or without a usable camera. */
#define SSAO_DEPTH_A_DEFAULT 1.0002f
#define SSAO_DEPTH_B_DEFAULT 0.0102f
int contact_build(const ContactConfig *c, const float view[16], const float proj[16], const float ab[2], RContact *out);
#endif
