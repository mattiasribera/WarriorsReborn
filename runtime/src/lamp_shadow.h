/* --lamp-shadows: characters near a lit street lamp or fire cast a
 * ground shadow pointing away from it.
 *
 * The title draws one character shadow, from one light above (its shadow map: a depth map of the
 * characters seen from that light, sampled by every world draw). The
 * lamps and fires --lamp-lights / --fire-light track are point lights in the world. For each world
 * pixel near such a light the renderer marches a ray from the pixel towards the light through the
 * same shadow map, used as a height field: a ray point below a stored character depth (by less than
 * a character's height) is inside that character, so the light is blocked and the pixel darkens
 * (renderer.h renderer_lamp_shadows). This module holds the options and turns the frame's light
 * projection and lights into the renderer's input. */
#ifndef LAMP_SHADOW_H
#define LAMP_SHADOW_H
#include <stdint.h>
#include "renderer.h"

#define LAMP_SHADOW_STRENGTH_DEFAULT 0.6f
#define LAMP_SHADOW_STRENGTH_MIN 0.1f
#define LAMP_SHADOW_STRENGTH_MAX 1.0f
typedef struct {
    int on;           /* --lamp-shadows / --no-lamp-shadows */
    float strength;   /* --lamp-shadows-strength 0.1-1: darkening where one nearby light is fully blocked */
} LampShadowConfig;
#define LAMP_SHADOW_DEFAULTS {0, LAMP_SHADOW_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a lamp-shadow option, -1 for a missing or out-of-range value (c unchanged). */
int lamp_shadow_arg(LampShadowConfig *c, const char *opt, const char *val);

/* The shadow's shape, in the title's world units (metres; a character is about 1.8 tall): casters
 * up to LAMP_SHADOW_HEIGHT tall, shadows that fade out from 0.6 x to 1 x LAMP_SHADOW_LENGTH away
 * from the character, rays that skip LAMP_SHADOW_START next to the pixel (the feet: the title's own
 * shadow is there), and LAMP_SHADOW_DIR_SHARE of the darkening where the title's shadow already
 * darkens the pixel (the two blend instead of stacking). */
#define LAMP_SHADOW_HEIGHT 2.2f
#define LAMP_SHADOW_LENGTH 4.0f
#define LAMP_SHADOW_START 0.25f
#define LAMP_SHADOW_DIR_SHARE 0.5f

/* A light of the frame (world position, radius, intensity: lamp_lights.h LampLight). */
typedef struct {
    float pos[3];
    float radius;
    float intensity;
} LampShadowLight;

/* The renderer's input for the frame (*out): prj is the depth stage's projection, rows s, t, r, q
 * over the homogeneous world position (the receivers' c_mTexPrj with the stage's coordinate scale),
 * which must be a perspective light: r = A q - B with A, B > 0, and invertible. The lights (n) are
 * sorted by distance to eye and the RENDERER_LAMP_SHADOW_MAX nearest with intensity above 0.01 and
 * in front of the light (q > 0) are kept; weight = min(intensity, 1). Returns the lights kept (>= 0),
 * -1 when prj is not such a projection (out->n = 0). */
int lamp_shadow_setup(const float prj[4][4], const LampShadowLight *lights, uint32_t n, const float eye[3], float strength,
                      RLampShadows *out);

/* Inverts a 4x4 matrix (rows); returns 0, or -1 when it is singular (inv unchanged). */
int lamp_shadow_invert(const float m[4][4], float inv[4][4]);
#endif
