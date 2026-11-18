/* --level-lights: the level's own light list, read from the title's
 * memory every frame, as the source of the lamp lights.
 *
 * The level scripts create the lights of a level with SetLight (about 3,100 real point lights over
 * the game, 83 % of them without a halo sprite), and the engine keeps them in its LightManager: a
 * singleton whose pointer is at LEVEL_LIGHTS_MGR_VA. Its pool (+0x04 an array of light pointers,
 * +0x0c the live count: the first live entries are the lights in use, 0x9a8a0 / 0x9bf40) holds
 * every light of the level; two lists (vectors of light pointers at +0x20 and +0x30) hold the
 * lights of the player's surroundings that its per-object query (0x9ae10) walks. The title itself
 * lights only characters and props with them (at most 3 point lights a draw); the world gets baked
 * vertex colour. level_lights_read walks the pool and both lists with checked reads and keeps the
 * enabled point lights
 * (with a radius) plus the directional and ambient lights; level_lights_select ranks the point
 * lights against the camera, keeps up to a cap with stable ids (the light object's address) and
 * fades them in and out, as lamp_lights.h lamp_track_update does for the halo lights. The halo
 * tracker stays the fallback: a frame whose read finds no usable light uses the halos.
 *
 * The reader is shared: world shadows and reflections can use the same LevelLightSet. */
#ifndef LEVEL_LIGHTS_H
#define LEVEL_LIGHTS_H
#include <stdint.h>
#include "lamp_lights.h"

#define LEVEL_LIGHTS_CAP_DEFAULT 32
#define LEVEL_LIGHTS_CAP_MIN 1
#define LEVEL_LIGHTS_CAP_MAX 32
typedef struct {
    int on;         /* --level-lights / --no-level-lights */
    uint32_t cap;   /* --level-lights-max 1-32: point lights per frame */
} LevelLightsConfig;
#define LEVEL_LIGHTS_DEFAULTS {0, LEVEL_LIGHTS_CAP_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a level-lights option, -1 for a missing or out-of-range value (c unchanged). */
int level_lights_arg(LevelLightsConfig *c, const char *opt, const char *val);

/* The title's layout (Ghidra). */
#define LEVEL_LIGHTS_MGR_VA 0x004cf078u   /* Graphics\LightManager * */
#define LEVEL_MGR_POOL 0x04u              /* light pointer array; +0x08 its capacity, +0x0c the live count */
#define LEVEL_MGR_LIST0 0x20u             /* two vectors {?, begin, end, capacity} of light pointers */
#define LEVEL_MGR_LIST1 0x30u
#define LEVEL_MGR_CHAR_LIGHT 0x7cu        /* the manager's own point light, moved onto characters (skipped) */
#define LEVEL_LIGHT_BYTES 0x98u           /* a light object, as far as it is read */
#define LEVEL_LIGHT_DIRTY 0x00u           /* byte: 1 after an update, cleared later (not a validity flag) */
#define LEVEL_LIGHT_KIND 0x04u            /* 1 point, 2 spot, 3 directional, 4 ambient */
#define LEVEL_LIGHT_COLOR 0x08u           /* rgba floats */
#define LEVEL_LIGHT_POS 0x38u             /* world position */
#define LEVEL_LIGHT_DIR 0x44u             /* unit direction (directional, spot) */
#define LEVEL_LIGHT_RADIUS 0x50u          /* range: the title's linear fall-off reaches 0 there */
#define LEVEL_LIGHT_GLOW 0x90u            /* uint16 halo index, 0xffff = none */
#define LEVEL_LIGHT_ENABLE 0x94u          /* uint16: 0 off */
#define LEVEL_KIND_POINT 1
#define LEVEL_KIND_SPOT 2
#define LEVEL_KIND_DIR 3
#define LEVEL_KIND_AMBIENT 4
/* What a valid list and light look like: at most LEVEL_LIST_MAX entries per list or pool (the
 * levels create at most ~250 records), positions within LEVEL_POS_MAX of the origin,
 * radius in (0, LEVEL_RADIUS_MAX], colour channels in [0, LEVEL_COLOR_MAX], all finite. */
#define LEVEL_LIST_MAX 1024u
#define LEVEL_POS_MAX 20000.0f
#define LEVEL_RADIUS_MAX 60.0f
#define LEVEL_COLOR_MAX 16.0f

/* Reads len bytes of guest memory at va into dst: 0, or -1 when [va, va + len) is not readable. */
typedef int (*LevelRead)(uint32_t va, void *dst, uint32_t len, void *ctx);

typedef struct {
    float pos[3];
    float radius;    /* world units */
    float rgb[3];    /* the script's colour (it can exceed 1) */
    uint32_t id;     /* the light object's guest address: stable while the light lives */
    uint8_t halo;    /* the light has its own halo sprite */
    uint8_t near;    /* in the manager's lists (the player's surroundings), not only in the pool */
} LevelPoint;
typedef struct {
    float dir[3];    /* unit, world space: the direction the light travels */
    float rgb[3];
} LevelDir;
#define LEVEL_POINTS_MAX 256
#define LEVEL_DIRS_MAX 4
typedef struct {
    LevelPoint p[LEVEL_POINTS_MAX];
    uint32_t n;                  /* point lights kept (enabled, radius > 0, valid) */
    LevelDir dir[LEVEL_DIRS_MAX];
    uint32_t ndir;               /* enabled directional lights: the moon, then the fill (script order) */
    float ambient[3];            /* sum of the enabled ambient lights */
    uint32_t listed;             /* lights in the pool and the lists (each counted once) */
    uint32_t in_lists;           /* of those, in the lists */
    uint32_t rejected;           /* entries skipped: unreadable or invalid */
    uint32_t off;                /* entries skipped: disabled, glow-only (radius 0) or a spot */
    uint32_t dropped;            /* point lights over LEVEL_POINTS_MAX */
} LevelLightSet;
/* Walks the manager (pointer at mgr_ptr_va) into *out. Returns 0, or -1 when the manager, its pool
 * and both lists are unreadable (out then holds no light). A light listed twice is kept once. */
int level_lights_read(LevelRead rd, void *ctx, uint32_t mgr_ptr_va, LevelLightSet *out);

/* The title's fall-off is linear, (R - d) / R to 0 at the range R; the renderer's lamp fall-off
 * (lamp_lights.h lamp_falloff) is smooth. A lamp light of radius LEVEL_RADIUS_FIT x R follows the
 * linear one within 0.075 (the closest fit of the two curves) and ends without a ring. */
#define LEVEL_RADIUS_FIT 1.29f
/* Intensity of a light at full level: the baked vertex colour of the world already holds these
 * lamps' pools (and the lamp pass multiplies it), so the world gets a share of the light the title
 * gives a character (tuned in level99 and level51 against the original mood). */
#define LEVEL_GAIN 0.45f
/* A light is shown when its sphere of influence (LEVEL_RADIUS_FIT x radius) reaches into the view
 * (visible: the caller's frustum test). Ranking: luminance x r^2 / (distance to eye^2 + r^2),
 * x LAMP_KEEP for a light shown last frame, x LEVEL_NEAR_BONUS for a light of the player's
 * surroundings (the manager's lists: the lights the title itself uses there); a light fades in and out by LAMP_FADE a frame (it
 * enters, leaves the cap or the view, is switched off). */
#define LEVEL_NEAR_BONUS 2.0f
#define LEVEL_TRACK_MAX 256
typedef struct {
    uint32_t id;
    float level;
    uint8_t shown, seen;
} LevelTrack;
typedef struct {
    LevelTrack t[LEVEL_TRACK_MAX];
    uint32_t n;
} LevelTracker;
void level_tracker_reset(LevelTracker *k);
/* The renderer's light for p at level (radius x LEVEL_RADIUS_FIT; colour over 1 goes into the
 * intensity, at most LEVEL_INTENSITY_MAX; x LEVEL_GAIN). */
#define LEVEL_INTENSITY_MAX 2.0f
LampLight level_lamp(const LevelPoint *p, float level);
/* One frame: the lights of s (NULL: none, everything fades out) against the camera at eye. Writes
 * the up to cap (<= LAMP_MAX_LIGHTS) lights of highest priority to out, faded; returns how many. */
uint32_t level_lights_select(LevelTracker *k, const LevelLightSet *s, const float eye[3], int (*visible)(const float pos[3], float radius, void *ctx),
                             void *ctx, LampLight *out, uint32_t cap);
#endif
