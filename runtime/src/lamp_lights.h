/* --lamp-lights: the title's lamp halos turned into point lights.
 *
 * The title has no dynamic light of its own in the world: lamps, fire and neon are halo sprites
 * (bloom.h bloom_halo_draw: FVF 0x142 particle quads centred on the lamp rims) over baked vertex
 * colour. Each frame the halo quads give light sprites (lamp_sprite_quad), the sprites of one lamp
 * are merged (lamp_cluster), and a tracker (lamp_track_update) matches them to the lights of the
 * previous frames by world position, so a light keeps its id, fades in and out instead of popping,
 * and the cap (LAMP_MAX_LIGHTS) does not flip between lights frame to frame. The renderer adds
 * those lights to the opaque world (renderer_lamp_lights). */
#ifndef LAMP_LIGHTS_H
#define LAMP_LIGHTS_H
#include <stdint.h>

#define LAMP_STRENGTH_DEFAULT 1.0f
#define LAMP_STRENGTH_MIN 0.1f
#define LAMP_STRENGTH_MAX 3.0f
typedef struct {
    int on;           /* --lamp-lights / --no-lamp-lights */
    float strength;   /* --lamp-lights-strength 0.1-3 */
} LampLightsConfig;
#define LAMP_LIGHTS_DEFAULTS {0, LAMP_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a lamp-lights option, -1 for a missing or out-of-range value (c unchanged). */
int lamp_lights_arg(LampLightsConfig *c, const char *opt, const char *val);

/* One halo quad: world centre, size (its largest world extent), colour (the vertex diffuse, 0-1)
 * and weight (how much light it stands for: mean alpha x the colour's brightest channel). */
typedef struct {
    float pos[3];
    float size;
    float rgb[3];
    float weight;
} LampSprite;
/* What makes a halo quad a light (measured in level51 and the hideout):
 * lamp halos are bright (weight 1: opaque vertex alpha, warm white), 2-4 units wide; their
 * light cones on the ground are white at half alpha (weight 0.5), ~0.7 wide. The same draws carry
 * the particle atlas: sparks and blood drops (< 0.3 wide), dust and smoke (grey or brown, peak
 * channel below 0.6) and haze plumes (grey, weight 0.2): none of them is a light. */
#define LAMP_MIN_WEIGHT 0.45f
#define LAMP_MIN_PEAK 0.6f
#define LAMP_MIN_SIZE 0.3f
/* A quad (nv vertices, 3-6; world positions and D3DCOLOR ARGB diffuse) as a sprite. Returns 0 and
 * *out, or -1 when nv is out of range or it is no light: smaller than LAMP_MIN_SIZE, weight under
 * LAMP_MIN_WEIGHT or its brightest channel under LAMP_MIN_PEAK. */
int lamp_sprite_quad(const float (*world)[3], const uint32_t *argb, int nv, LampSprite *out);

/* Merges in place the sprites whose centres lie within LAMP_MERGE x the larger size of the two
 * (the halo, cone and glow quads of one lamp): weighted centre and colour, summed weight, largest
 * size. Order-independent for well-separated lamps. Returns the sprites left (at the front). */
#define LAMP_MERGE 0.75f
uint32_t lamp_cluster(LampSprite *s, uint32_t n);

/* A light for the renderer: world position, radius of influence (world units), colour (0-1)
 * and intensity (already faded), plus its stable id. */
typedef struct {
    float pos[3];
    float radius;
    float rgb[3];
    float intensity;
    uint32_t id;
} LampLight;
/* Radius from a sprite's size: LAMP_RADIUS_SCALE x size within LAMP_RADIUS_MIN-MAX. */
#define LAMP_RADIUS_SCALE 4.0f
#define LAMP_RADIUS_MIN 1.0f
#define LAMP_RADIUS_MAX 10.0f
float lamp_radius(float size);

/* The lights followed across frames. The title draws a halo only while its lamp is on screen, but
 * the lamp lights what is on screen near it: an unmatched track whose position in_view (when given)
 * says is out of view keeps its level for up to LAMP_HOLD frames instead of fading. A sprite matches the nearest unmatched track within
 * max(LAMP_MATCH_MIN, 0.5 x its radius); a match eases the track's position, colour and weight
 * towards the sprite's and its level up by LAMP_FADE per frame; a track without a match fades by
 * LAMP_FADE per frame and is dropped at 0; a sprite without a track starts one at level 0 with a
 * new id. The output is the LAMP_MAX_LIGHTS (or cap, if smaller) tracks of highest priority
 * (weight x level x radius^2 / (distance to eye^2 + radius^2); x LAMP_KEEP for a track that was in
 * the previous output, so two similar lights do not swap every frame), each with intensity
 * min(weight, 1) x level; tracks at level 0 are left out. */
#define LAMP_MAX_LIGHTS 32
#define LAMP_TRACK_MAX 128
#define LAMP_MATCH_MIN 0.5f
#define LAMP_FADE 0.125f
#define LAMP_KEEP 1.5f
#define LAMP_HOLD 300
typedef struct {
    float pos[3], size, rgb[3], weight, level;
    uint32_t id;
    uint8_t matched, shown;
    uint16_t held;   /* frames kept out of view without a halo */
} LampTrack;
typedef struct {
    LampTrack t[LAMP_TRACK_MAX];
    uint32_t n, next_id;
} LampTracker;
void lamp_tracker_reset(LampTracker *k);
uint32_t lamp_track_update(LampTracker *k, const LampSprite *s, uint32_t n, const float eye[3], LampLight *out, uint32_t cap,
                           int (*in_view)(const float pos[3], void *ctx), void *ctx);

/* Falloff of a light at distance d with radius r (> 0): (1 - x^2)^2 / (1 + LAMP_FALLOFF_K x^2),
 * x = d / r: 1 at the light, smooth and 0 from r on (zero slope there, no ring). The renderer's
 * shader (lamp.frag) uses the same formula. */
#define LAMP_FALLOFF_K 4.0f
/* Characters gain this much more than the world around them (they gain the most: the title lights
 * them flat). */
#define LAMP_CHAR_GAIN 1.35f
float lamp_falloff(float d, float r);
#endif
