/* --sky-hd: the options, the rule that finds the title's sky stage
 * (dome, cloud cap, skyline backdrop, far lamp glows) among a frame's operations, the palette read
 * from a level's dome texture, and the parameters of the procedural sky (renderer_sky) and of the
 * skyline's windows and haze (renderer_skyline) built from them. Game-independent: no addresses,
 * no texture hashes. */
#ifndef SKY_H
#define SKY_H
#include <stdint.h>
#include "renderer.h"

typedef struct {
    int on;   /* --sky-hd / --no-sky-hd */
} SkyConfig;
#define SKY_DEFAULTS {0}
/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1), 0 when
 * opt is not a sky option. */
int sky_arg(SkyConfig *c, const char *opt, const char *val);

/* One operation of a frame, as the classifier sees it. Draws carry their fixed-function state:
 * fvf 0 for a vertex program; near = |P14 / P10| of the projection (the near plane of a D3D
 * perspective matrix); view_origin = the view matrix has no translation (a camera-centred mesh);
 * radius = the largest |object position| of the draw; emissive = the material's largest emissive
 * channel; tex_* = the stage-0 texture (format code of xtex.h, 0 = none). Offscreen operations are
 * SKY_OP_OTHER. */
enum { SKY_OP_OTHER, SKY_OP_CLEAR_COLOR, SKY_OP_CLEAR_DEPTH, SKY_OP_DRAW };
typedef struct {
    uint8_t kind;
    uint8_t zwrite, blend, view_origin;
    uint32_t fvf, nv;
    float near, radius, emissive;
    uint32_t tex_fmt, tex_w, tex_h;
} SkyOp;

/* The sky stage of a frame (findings section 2): the back-buffer draws after its colour clear and
 * before the depth-only clear that precedes the world.
 *   dome:    the stage's first draw: fixed function (FVF 0x152), no depth write, camera-centred
 *            (view_origin, radius <= SKY_DOME_RADIUS, near <= SKY_DOME_NEAR), a DXT1 texture at
 *            least SKY_DOME_MIN_W wide and twice as high as wide (smaller domes are interiors).
 *   cap:     the next draw when it is blended and on the same camera (view_origin, small radius).
 *   skyline: a later draw of the stage that writes depth, fixed function (0x152 / 0x252), with a
 *            far near plane (SKY_SKYLINE_NEAR_MIN..MAX) and a small atlas (at most 32 texels).
 *   glows:   the run of draws right after the depth-only clear: fixed function, blended, no depth
 *            write, emissive >= SKY_GLOW_EMISSIVE.
 * Indices are into ops, -1 when absent. Returns 1 when a dome was found (a sky to replace), else 0
 * (out all -1). */
#define SKY_DOME_FVF 0x152u
#define SKY_DOME_RADIUS 0.7f
#define SKY_DOME_NEAR 0.2f
#define SKY_DOME_MIN_W 64u
#define SKY_TEX_DXT1 0x0Cu
#define SKY_SKYLINE_NEAR_MIN 10.0f
#define SKY_SKYLINE_NEAR_MAX 100.0f
#define SKY_SKYLINE_ATLAS_MAX 32u
#define SKY_GLOW_EMISSIVE 0.9f
typedef struct {
    int32_t dome, cap, skyline, glow_first, glow_count;
} SkyStage;
int sky_classify(const SkyOp *ops, uint32_t n, SkyStage *out);
/* 1 when one draw has the dome's signature (the first-draw test of sky_classify). */
int sky_is_dome(const SkyOp *o);

/* Palette of a dome texture (w x h texels, 0xAARRGGBB, top row first): the dome maps its bottom row
 * to the horizon and its top row to the zenith (findings 1b: ~32 deg at 0.375 of the height from the
 * top, ~52 deg at 0.06), with a painted city silhouette over the bottom rows. Each colour is a row's
 * trimmed mean (texels between the 40th and 80th luminance percentile: no stars, no buildings).
 *   ramp[k]:    the sky at elevation k x 90 / (SKY_RAMP - 1) degrees; elevation 0 reads the row at
 *               SKY_HORIZON_ROW of the height (the glow just above the silhouette).
 *   silhouette: the mean of the darkest third of the bottom SKY_SILHOUETTE_ROWS of the texture.
 *   stars:      0-1, how starry the upper sky is: the share of texels in the top SKY_STAR_ROWS that
 *               are points, brighter by SKY_STAR_DELTA (0-255 luminance) than each texel two away
 *               (so painted moons and cloud edges do not count), scaled so SKY_STAR_FULL of them
 *               is 1; at least SKY_STAR_DARK_MIN when the zenith (ramp[SKY_RAMP - 1]) is darker
 *               than SKY_STAR_DARK_LUM (a night sky painted without visible stars).
 * Returns 0, or -1 for a texture too small (under 8 x 8; out unchanged). */
#define SKY_RAMP 8
#define SKY_HORIZON_ROW 0.88f
#define SKY_SILHOUETTE_ROWS 0.12f
#define SKY_STAR_ROWS 0.35f
#define SKY_STAR_DELTA 40.0f
#define SKY_STAR_FULL 0.006f
#define SKY_STAR_DARK_LUM 32
#define SKY_STAR_DARK_MIN 0.35f
typedef struct {
    uint32_t ramp[SKY_RAMP];
    uint32_t silhouette;
    float stars;
} SkyPalette;
int sky_palette(const uint32_t *texels, uint32_t w, uint32_t h, SkyPalette *out);
/* Texture row (0 = top, 1 = bottom) that the dome shows at an elevation in degrees (0-90). */
float sky_dome_row(float elevation_deg);

/* The frame's procedural sky for renderer_sky.
 *   view, proj: the dome draw's D3D matrices (row vectors; world = identity), from which the
 *               world-space view direction per pixel is rebuilt (RSky.right/up/fwd).
 *   shade:      the dome's lit vertex colour (ARGB): the palette is scaled by it, as the title's
 *               fixed-function lighting scales the texture.
 *   cap:        the frame has a cloud cap draw (more cloud cover).
 *   rain:       0-1, the title's rain (its layers in the frame): clouds turn overcast, stars and
 *               moon fade out.
 *   sun:        c_vSunDir (the shadow direction, world, pointing down) or NULL: the moon sits
 *               opposite it, when it is above the horizon and the sky has stars.
 *   seconds:    the title's clock in seconds (cloud drift, twinkle).
 * Returns 1 and out, 0 without a usable camera (out unchanged). */
/* Stars (sky.frag): the sphere is cut into cells of 1 / SKY_STAR_CELLS (direction units) and a cell
 * holds a star with chance RSky.stars = palette stars x SKY_STAR_CHANCE x (1 - rain): a hazy city
 * sky, a few dozen on a screen. None below SKY_STAR_MIN_ELEVATION (radians). */
#define SKY_STAR_CELLS 160.0f
#define SKY_STAR_CHANCE 0.0022f
#define SKY_STAR_MIN_ELEVATION 0.02f
#define SKY_COVER_CLEAR 0.18f
#define SKY_COVER_CAP 0.38f
#define SKY_COVER_RAIN 0.92f
#define SKY_MOON_RADIUS 0.03f   /* radians */
#define SKY_MOON_MIN_ELEVATION 0.05f   /* sin of the lowest moon elevation */
int sky_build(const SkyPalette *pal, const float view[16], const float proj[16], uint32_t shade, int cap, float rain, const float *sun,
              float seconds, RSky *out);

/* The skyline's windows and haze for renderer_skyline: the haze colour is the horizon of the
 * palette (ramp[0]) times shade; strengths at full: SKY_HAZE_MAX of the horizon colour reached at
 * SKY_HAZE_DIST view units; fewer lit windows in the rain (rain 0-1). */
#define SKY_HAZE_MAX 0.6f
#define SKY_HAZE_DIST 500.0f
void sky_skyline_build(const SkyPalette *pal, uint32_t shade, float rain, float seconds, RSkyline *out);
#endif
