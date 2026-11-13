/* --light-shafts: the lamps' light seen in the air.
 *
 * Each lamp of the --lamp-lights list (lamp_lights.h: the title's halo sprites, merged and tracked
 * across frames) hangs a cone of scattered light below it, axis world down; fires (--fire-light)
 * a round glow rising above them (their smoke). The renderer (renderer_light_shafts) marches each
 * view ray up to the visible surface through those volumes at a reduced size and adds the result to
 * the opaque world: walls, characters and the ground in front of a cone hide the part behind them.
 * The air is thicker while the title's rain falls (light_shafts_rain_step). */
#ifndef LIGHT_SHAFTS_H
#define LIGHT_SHAFTS_H

#define LIGHT_SHAFTS_STRENGTH_DEFAULT 1.0f
#define LIGHT_SHAFTS_STRENGTH_MIN 0.1f
#define LIGHT_SHAFTS_STRENGTH_MAX 3.0f
typedef struct {
    int on;           /* --light-shafts / --no-light-shafts */
    int cheap;        /* --light-shafts-cheap: a quarter-size pass with fewer steps (integrated GPUs) */
    float strength;   /* --light-shafts-strength 0.1-3 */
} LightShaftsConfig;
#define LIGHT_SHAFTS_DEFAULTS {0, 0, LIGHT_SHAFTS_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a light-shafts option, -1 for a missing or out-of-range value (c unchanged). */
int light_shafts_arg(LightShaftsConfig *c, const char *opt, const char *val);

/* The rain level (0-1) after one frame: towards 1 by LIGHT_SHAFTS_RAIN_IN per frame while the title
 * draws its rain layers (raining), towards 0 by LIGHT_SHAFTS_RAIN_OUT otherwise. */
#define LIGHT_SHAFTS_RAIN_IN (1.0f / 60.0f)
#define LIGHT_SHAFTS_RAIN_OUT (1.0f / 150.0f)
float light_shafts_rain_step(float level, int raining);

/* Scattering per view unit of a light of intensity 1: LIGHT_SHAFTS_DENSITY x strength, times
 * (1 + LIGHT_SHAFTS_RAIN_GAIN x rain level). */
#define LIGHT_SHAFTS_DENSITY 0.09f
#define LIGHT_SHAFTS_RAIN_GAIN 1.5f
float light_shafts_density(float strength, float rain);

/* A cone's length from its light's radius (lamp_lights.h lamp_radius): LIGHT_SHAFTS_LEN_SCALE x
 * radius within LIGHT_SHAFTS_LEN_MIN-MAX (a street lamp, radius ~10, reaches the ground ~3 below). */
#define LIGHT_SHAFTS_LEN_SCALE 0.6f
#define LIGHT_SHAFTS_LEN_MIN 1.5f
#define LIGHT_SHAFTS_LEN_MAX 6.0f
float light_shafts_length(float radius);

/* The cone's density (0-1) at point p for a lamp at apex, unit axis and length len: 0 above the
 * apex, past len and outside LIGHT_SHAFTS_COS_OUTER of the axis; full inside LIGHT_SHAFTS_COS_INNER
 * (a smoothstep between: a soft edge), times (1 - h / len)^2 down the axis (h: the distance along
 * it) and 1 / (1 + LIGHT_SHAFTS_NEAR_K x (h / len)^2). The renderer's shader (shafts.frag) uses the
 * same formula, plus a small glow round the apex. */
#define LIGHT_SHAFTS_COS_OUTER 0.766f   /* 40 degrees */
#define LIGHT_SHAFTS_COS_INNER 0.940f   /* 20 degrees */
#define LIGHT_SHAFTS_NEAR_K 3.0f
float light_shafts_cone(const float p[3], const float apex[3], const float axis[3], float len);
#endif
