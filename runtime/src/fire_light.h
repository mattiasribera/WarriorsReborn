/* --fire-light: the burning fire barrels light the walls, the floor and
 * the characters near them with a warm, gently flickering light.
 *
 * The title has no light of its own for its fires: a barrel's fire is a particle emitter whose
 * flame sprites are drawn each frame (alpha-blended, depth write off) from the flame textures. Each
 * frame those flame quads become fire sprites (fire_sprite_quad, world space), the sprites of one
 * fire are merged (fire_cluster), and a lamp tracker (lamp_lights.h lamp_track_update, its own
 * instance) follows them across frames, so a fire keeps its id, fades in and out and only the
 * FIRE_MAX_LIGHTS nearest the camera are kept. fire_light_shade turns the tracked fires into
 * lights: a ~2000 K colour, a few metres of radius, and a smooth flicker of intensity, radius and
 * position from the time in seconds (frame-rate independent: the same at 30 and 60 Hz). Those
 * lights join the lamp lights of the frame, so the world-lit (--world-lighting) and lit character
 * (--char-lighting) draws pick them up through their per-draw light masks. */
#ifndef FIRE_LIGHT_H
#define FIRE_LIGHT_H
#include <stdint.h>
#include "lamp_lights.h"

#define FIRE_STRENGTH_DEFAULT 1.0f
#define FIRE_STRENGTH_MIN 0.1f
#define FIRE_STRENGTH_MAX 3.0f
typedef struct {
    int on;           /* --fire-light / --no-fire-light */
    float strength;   /* --fire-light-strength 0.1-3 */
} FireLightConfig;
#define FIRE_LIGHT_DEFAULTS {0, FIRE_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a fire-light option, -1 for a missing or out-of-range value (c unchanged). */
int fire_light_arg(FireLightConfig *c, const char *opt, const char *val);

/* The flame textures (content hashes, texpack.h texpack_hash): the barrels' fire, a 6 x 6 grid of
 * flame frames (256 x 256), and the taller fires' strip of flame frames (128 x 64). Their draws carry
 * the lamp halos' render state (bloom.h bloom_halo_draw), so with --fire-light their quads become
 * fire sprites instead of lamp sprites (a fire would otherwise be a steady white lamp). */
#define FIRE_TEX_FLAMES 0x35a4458c1ece9bcfull
#define FIRE_TEX_FLAME_STRIP 0xe447396d218f54a9ull
int fire_light_texture(uint64_t hash);

/* A flame quad (nv vertices, 3-6; world positions and D3DCOLOR ARGB diffuse) as a sprite: centre,
 * size (largest world extent) and weight 1 (each flame counts the same, whatever its alpha: the
 * flame animation fades its quads, the fire does not dim; the tracker's intensity is then its fade
 * level). The colour is not the flame's (the texture carries it) but the fire's (FIRE_R/G/B).
 * Returns 0 and *out, or -1 when nv is out of range, the quad is degenerate (smaller than
 * FIRE_MIN_SIZE) or faded out (mean alpha under FIRE_MIN_WEIGHT). */
#define FIRE_MIN_SIZE 0.05f
#define FIRE_MIN_WEIGHT 0.05f
int fire_sprite_quad(const float (*world)[3], const uint32_t *argb, int nv, LampSprite *out);

/* Merges in place the sprites whose centres lie within FIRE_MERGE world units of each other (the
 * many flame quads of one barrel, which rise and drift): weighted centre, summed weight, the largest
 * size. Two barrels a few metres apart stay two fires. Returns the sprites left (at the front).
 * Closest pair first: O(n^3) at worst, so the runtime keeps at most FIRE_FRAME_SPRITES flame quads
 * a frame (a level shows one quad per small fire, a handful in view). */
#define FIRE_MERGE 1.2f
#define FIRE_FRAME_SPRITES 64
uint32_t fire_cluster(LampSprite *s, uint32_t n);

/* The light of a fire: about 2000 K (sRGB-ish, 0-1), FIRE_RADIUS world units (the title's units are
 * metres), FIRE_INTENSITY at full level before the strength. The world-lit shader adds light as
 * v0 + L x (1 - v0) after a soft knee, so this cannot blow out the baked light. */
#define FIRE_R 1.0f
#define FIRE_G 0.47f
#define FIRE_B 0.14f
#define FIRE_RADIUS 4.5f
#define FIRE_INTENSITY 0.9f
#define FIRE_MAX_LIGHTS 8

/* Flicker of fire id at time t (seconds): a sum of smooth value-noise octaves (1.3, 3.1 and 7.3 Hz),
 * within [1 - FIRE_FLICKER_AMP, 1]; continuous in t (no jumps between frames at any frame rate),
 * deterministic, and different per id. */
#define FIRE_FLICKER_AMP 0.35f
float fire_flicker(uint32_t id, double t);
/* Position jitter of fire id at time t: a smooth offset, each axis within +-FIRE_JITTER (the light
 * sways with the flames; z up a little less). */
#define FIRE_JITTER 0.12f
void fire_jitter(uint32_t id, double t, float off[3]);

/* The tracked fires (lamp_track_update output, n of them) as lights at time t, in place: colour
 * FIRE_R/G/B, radius FIRE_RADIUS x (0.92 + 0.08 x flicker), position + jitter and intensity
 * FIRE_INTENSITY x the track's intensity (its fade) x flicker x strength. */
void fire_light_shade(LampLight *l, uint32_t n, double t, float strength);

#endif
