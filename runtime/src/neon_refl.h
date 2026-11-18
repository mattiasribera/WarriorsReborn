/* --neon-reflections: neon signs mirrored in wet and damp ground.
 *
 * The signs of --neon-glow are textures with an emission mask (neon_glow.h); nothing tells where
 * they stand in the world. This option makes each glowing sign draw of the 3D scene a light of the
 * wet ground's light streaks (wet_streaks.h, with --wet-light-streaks in the rain and always with
 * --damp-streets): its vertices (clip space, the frame's camera) give a box in the world, its
 * colour is the mask-weighted mean of its texture (the lit tubes only), and a tracker of its own
 * (lamp_lights.h: stable ids, fades, held while out of view) keeps a sign mirrored while it is
 * above the top of the screen, as the lamps are. In the wet pass a sign is an extended light (its
 * streak is as wide as the sign seen from the ground) and the screen-space reflection keeps
 * saturated colours (red and blue tubes are dark by luma alone) where the option is on. */
#ifndef NEON_REFL_H
#define NEON_REFL_H
#include <stdint.h>
#include "lamp_lights.h"
#include "renderer.h"

#define NEON_REFL_STRENGTH_DEFAULT 1.0f
#define NEON_REFL_STRENGTH_MIN 0.1f
#define NEON_REFL_STRENGTH_MAX 3.0f
typedef struct {
    int on;           /* --neon-reflections / --no-neon-reflections (with --neon-glow and wet or damp ground) */
    float strength;   /* --neon-reflections-strength 0.1-3 */
} NeonReflConfig;
#define NEON_REFL_DEFAULTS {0, NEON_REFL_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a neon-reflections option, -1 for a missing or out-of-range value (c unchanged). */
int neon_refl_arg(NeonReflConfig *c, const char *opt, const char *val);

/* A sign's mask on a NEON_REFL_GRID x NEON_REFL_GRID grid (texel centres, nearest), from an L8 mip
 * chain (neon_glow_load's layout: levels one after the other, each max(1, w >> l) x max(1, h >> l)):
 * read from the smallest level that is still at least NEON_REFL_GRID wide and high (else level 0).
 * Returns 0, or -1 without data. */
#define NEON_REFL_GRID 16
int neon_refl_grid(const uint8_t *chain, uint32_t w, uint32_t h, uint32_t levels, uint8_t grid[NEON_REFL_GRID * NEON_REFL_GRID]);
/* The colour of the lit parts: the mean of the texture's texels (B8G8R8A8 0xAARRGGBB, tw x th,
 * sampled at the grid's texel centres, nearest) weighted by the grid's mask, in 0-1, and *cover the
 * mask's mean (0-1). Returns 0, or -1 when the mask is dark everywhere (no lit part: no light). */
int neon_refl_colour(const uint8_t grid[NEON_REFL_GRID * NEON_REFL_GRID], const uint32_t *argb, uint32_t tw, uint32_t th, float rgb[3],
                     float *cover);

/* A sign draw as a light sprite: its n vertices' clip positions (x, y, z, w floats, `stride` floats
 * apart) taken back to the world through the frame's camera (view: D3D row vectors, world z up;
 * proj: a perspective projection, proj[11] = +-1), their box's centre and largest extent (the
 * sprite's size, at least NEON_REFL_MIN_SIZE), the colour rgb and weight 1. Vertices behind the
 * camera (w <= 0) are left out. Returns 0, or -1 without a usable camera or vertex. */
#define NEON_REFL_MIN_SIZE 0.5f
int neon_refl_sprite(const float *xyzw, uint32_t stride, uint32_t n, const float view[16], const float proj[16], const float rgb[3],
                     LampSprite *out);

/* The look: a sign's light has the tracker's radius (lamp_radius of its size) up to
 * NEON_REFL_RADIUS_MAX (its streak's reach: the ground near the sign, not the whole street), its
 * colour times NEON_REFL_GAIN x strength; at most NEON_REFL_CAP signs a frame, after the lamps and
 * fires (RENDERER_WET_LIGHTS in all). */
#ifndef NEON_REFL_GAIN
#define NEON_REFL_GAIN 0.8f
#endif
#ifndef NEON_REFL_RADIUS_MAX
#define NEON_REFL_RADIUS_MAX 5.0f
#endif
#define NEON_REFL_CAP 8

/* Appends the frame's sign lights (n of them, world space; already faded by their tracker) to the
 * wet ground w after its lamps and fires (wet_streaks_build), when c->on and the streaks are on
 * (w->streaks > 0): each in view space as wet_streaks_build's lights, its colour x intensity x
 * NEON_REFL_GAIN x strength, its radius at most NEON_REFL_RADIUS_MAX, its size (RRainWet.light[].size,
 * the extended light) the tracker's radius / LAMP_RADIUS_SCALE; and w->neon = strength (the screen-space reflection keeps saturated
 * colours). Returns the lights appended; with c->on == 0, w->streaks 0 or an unusable camera,
 * returns 0 and leaves w unchanged. */
uint32_t neon_refl_apply(const NeonReflConfig *c, const LampLight *l, uint32_t n, const float view[16], const float proj[16], RRainWet *w);
#endif
