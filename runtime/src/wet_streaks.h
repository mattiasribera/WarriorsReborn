/* --wet-light-streaks: lamps and fires mirrored in wet ground.
 *
 * The --rain-hd-wet pass reflects what the screen shows (a screen-space trace over an 8-bit
 * frame): a street lamp above the top of the screen, the usual third-person view, is never found,
 * and a lamp it does find is clipped at the frame's white. With this option the wet pass also gets
 * the frame's light list, the one the lighting uses (frame_lamp: the level's lights or the halo
 * lamps, then the fires; already faded in and out by their trackers), and every wet pixel adds an
 * analytic highlight for each light mirrored about its own height (renderer.h RRainWet.streaks):
 * the long, unbroken streaks of the PS3-era wet street. Where the trace finds nothing the ground
 * reflects the fog colour instead (the miss fallback). */
#ifndef WET_STREAKS_H
#define WET_STREAKS_H
#include <stdint.h>
#include "lamp_lights.h"
#include "renderer.h"

#define WET_STREAKS_STRENGTH_DEFAULT 1.0f
#define WET_STREAKS_STRENGTH_MIN 0.1f
#define WET_STREAKS_STRENGTH_MAX 4.0f
#define WET_STREAKS_CAP_DEFAULT 16
#define WET_STREAKS_CAP_MIN 1
#define WET_STREAKS_CAP_MAX RENDERER_WET_LIGHTS
typedef struct {
    int on;           /* --wet-light-streaks / --no-wet-light-streaks (with --rain-hd and its wet ground) */
    float strength;   /* --wet-light-streaks-strength 0.1-4 */
    int cap;          /* --wet-light-streaks-cap 1-32: the most lights a frame streaks */
} WetStreaksConfig;
#define WET_STREAKS_DEFAULTS {0, WET_STREAKS_STRENGTH_DEFAULT, WET_STREAKS_CAP_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a wet-light-streaks option, -1 for a missing or out-of-range value (c unchanged). */
int wet_streaks_arg(WetStreaksConfig *c, const char *opt, const char *val);

/* The look (renderer.h RRainWet): gain at strength 1, lobe width along the view on the ground,
 * falloff in light radii, the miss fallback's share and its colour without a fog colour (a dim
 * night blue-grey). Lights farther than WET_STREAKS_FAR view units in front of the camera, or more
 * than their radius behind it, are left out. */
#define WET_STREAKS_GAIN 1.6f
#define WET_STREAKS_ROUGH 0.11f
#define WET_STREAKS_REACH 2.5f
#define WET_STREAKS_FALLBACK 0.5f
#define WET_STREAKS_FOG_DEFAULT 0x0C1018u
#define WET_STREAKS_FAR 90.0f

/* Fills the streak fields of w (wet ground already built by rain_wet_build) for the frame's lights
 * (n of them, world space z up; lights from first_fire on are fires) with the frame's camera
 * (view and projection as rain_wet_build's) when c->on: each light in view space with colour x
 * intensity x scale (lights at intensity 0 or black left out), the fires first (the nearest
 * lights), then the others in their order (their trackers' priority), at most c->cap; the camera's
 * axes and eye in the world (as rain_puddle_build); the fog colour (ARGB, alpha ignored) when
 * has_fog, else WET_STREAKS_FOG_DEFAULT. The streaks stay on (w->streaks > 0) with no light: the
 * miss fallback still applies. Returns the lights kept; with c->on == 0 or an unusable camera,
 * returns 0 and leaves w unchanged (w->streaks 0: exactly as without the option). */
/* Light li into slot k of w (view space through view and proj, colour x intensity x scale, size
 * 0: a point light) when it is lit and in reach (no more than its radius behind the camera, at most
 * WET_STREAKS_FAR ahead); returns 1 when kept, else 0 (slot k unchanged). The camera must be
 * perspective (proj[11] = +-1). wet_streaks_build keeps its lights so. */
int wet_streaks_keep(const LampLight *li, float scale, const float view[16], const float proj[16], RRainWet *w, uint32_t k);
uint32_t wet_streaks_build(const WetStreaksConfig *c, const LampLight *l, uint32_t n, uint32_t first_fire, float scale, const float view[16],
                           const float proj[16], int has_fog, uint32_t fog_argb, RRainWet *w);
#endif
