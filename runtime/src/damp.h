/* --damp-streets: damp night streets without rain.
 *
 * Most night levels never rain, so the wet ground of --rain-hd (screen-space reflections, puddles,
 * light streaks, the --materials collision map) never shows there. This option runs the same wet
 * pass in dry night frames at a much lower strength: only asphalt and rooftop tar get a faint
 * sheen (concrete less, every other surface none; the material comes from the level's collision
 * map), with tight reflections of the bright parts (lamps, neon, fires), soft light streaks, no
 * ripples and a few small damp patches near kerbs. The ground gets a touch darker under the
 * sheen, so only the lit spots brighten (the frame as a whole by about 1%). Only outdoors at night (the frame draws the
 * title's sky dome and its zenith is dark), never under something solid (the collision map's second
 * surface), never while it rains (the rain's own path wins). */
#ifndef DAMP_H
#define DAMP_H
#include <stdint.h>
#include "renderer.h"

#define DAMP_STRENGTH_DEFAULT 1.0f
#define DAMP_STRENGTH_MIN 0.1f
#define DAMP_STRENGTH_MAX 2.0f
typedef struct {
    int on;           /* --damp-streets / --no-damp-streets */
    float strength;   /* --damp-streets-strength 0.1-2 */
} DampConfig;
#define DAMP_DEFAULTS {0, DAMP_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a damp option, -1 for a missing or out-of-range value (c unchanged). */
int damp_arg(DampConfig *c, const char *opt, const char *val);

/* The look at strength 1 (the rain's wet ground at full rain: reflection 2.5 x 1.8, darkening
 * 0.3 x 1.8): reflection gain, darkening (at most DAMP_DARKEN_MAX), the traced reach in view
 * units, the damp patches' strength (RRainWet.puddles, without ripples) and the light streaks'
 * share of their rain gain. */
/* (the first look, reflection 1.6, streaks 0.8 reaching 4.5 light radii,
 * read as a rain-soaked mirror on a dry yard: long, strong lines from every lamp across the screen.
 * Now faint glints by the lamps: short streaks fading out within DAMP_STREAK_REACH radii.) */
#define DAMP_REFLECT 0.7f
#define DAMP_DARKEN 0.04f
#define DAMP_DARKEN_MAX 0.3f
#define DAMP_REACH 30.0f
#define DAMP_PUDDLES 0.45f
#define DAMP_STREAKS 0.35f
#define DAMP_STREAK_REACH 1.75f

/* Night: the sky dome's zenith colour (ARGB, alpha ignored) has a luma (0.299 R + 0.587 G +
 * 0.114 B, 0-255) below DAMP_NIGHT_LUMA. */
#define DAMP_NIGHT_LUMA 64.0f
int damp_night(uint32_t zenith_argb);

/* The damp ground (renderer_rain_wet) for a dry frame: the camera from the frame's view and
 * projection matrices as for rain_wet_build (world up +z) with its axes and eye in the world (as
 * rain_puddle_build), the depth projection ab (NULL: the rain's defaults), the look above times
 * c->strength, RRainWet.damp = c->strength (the damp mode of the pass) and time 0. Returns 1 and
 * out, or 0 (out unchanged) when off or without a usable camera. */
int damp_wet_build(const DampConfig *c, const float view[16], const float proj[16], const float ab[2], RRainWet *out);
/* After wet_streaks_build on that ground: the streaks at DAMP_STREAKS of their gain, reaching
 * DAMP_STREAK_REACH light radii (less than the rain's 2.5: a soft glint on the ground near the lamp,
 * not a line across the street), and no miss fallback (a dry street reflects nothing where the trace
 * finds nothing). */
void damp_streaks_apply(RRainWet *w);

/* Fade in and out (the player walks indoors, the rain starts or stops, a level without night): a
 * level 0-1 moved towards `want` by the title's clock, DAMP_FADE_MS from one end to the other (a
 * frame without the clock counts DAMP_FADE_FRAME_MS, a clock step at most DAMP_FADE_MS; the first
 * clock reading moves nothing). A zeroed DampFade is level 0. */
#define DAMP_FADE_MS 600.0f
#define DAMP_FADE_FRAME_MS 16.7f
typedef struct {
    float level;
    uint32_t last_ms;
    int has_ms;
} DampFade;
float damp_fade_step(DampFade *f, int want, int has_ms, uint32_t ms);
#endif
