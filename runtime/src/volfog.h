/* --volfog: froxel volumetric fog, the port's lights seen in the night air.
 *
 * A camera-aligned grid of froxels (VolfogGrid: tiles of the screen x exponential depth slices,
 * VOLFOG_NEAR to volfog_far) holds, per froxel, the light the air scatters towards the camera from
 * the port's lights (the level's lamps or the halo lights, fires, neon signs): only that in-scatter
 * is added, because the title's own fog (D3D fixed-function fog, baked into every pixel by the
 * replayed draws) already darkens and tints the scene by distance. The renderer
 * (renderer_volfog) injects the lights per froxel (jittered, blended with the reprojected previous
 * frame), integrates each column front to back and composites `scene x lerp(1, T, VOLFOG_EXTINCTION_MIX)
 * + S` at the end of the opaque world, before the TAA resolve; walls stop the light through
 * per-light polar maps of the level's wall map. While on, --light-shafts is skipped (both would
 * draw the lamp cones). The pure helpers here are shared by hle_d3d.c and the unit test. */
#ifndef VOLFOG_H
#define VOLFOG_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { VOLFOG_LOW, VOLFOG_MEDIUM, VOLFOG_HIGH };
#define VOLFOG_STRENGTH_DEFAULT 1.0f
#define VOLFOG_STRENGTH_MIN 0.1f
#define VOLFOG_STRENGTH_MAX 3.0f
typedef struct {
    int on;           /* --volfog / --no-volfog */
    int quality;      /* --volfog-quality low|medium|high (VOLFOG_LOW..VOLFOG_HIGH) */
    float strength;   /* --volfog-strength 0.1-3 */
} VolfogConfig;
#define VOLFOG_DEFAULTS {0, VOLFOG_MEDIUM, VOLFOG_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a volfog option, -1 for a missing or invalid value (c unchanged). */
int volfog_arg(VolfogConfig *c, const char *opt, const char *val);
/* "low", "medium" or "high" (an invalid quality: "medium"). */
const char *volfog_quality_name(int quality);

/* The froxel grid of a quality: w x h tiles of the screen, d depth slices, at most `lights` lights
 * per frame. An invalid quality gives medium's. */
typedef struct { int w, h, d, lights; } VolfogGrid;
VolfogGrid volfog_grid(int quality);
#define VOLFOG_LIGHTS_MAX 48

/* Depth range (view units, about metres). The far plane: VOLFOG_FAR_MAX, or with the title's fog
 * VOLFOG_FAR_FOG x the distance where it is (nearly) complete (linear: its end; exp / exp2: where
 * the fog factor falls to 5 %), at least VOLFOG_FAR_MIN. */
#define VOLFOG_NEAR 0.4f
#define VOLFOG_FAR_MAX 80.0f
#define VOLFOG_FAR_MIN 10.0f
#define VOLFOG_FAR_FOG 0.7f
float volfog_far(int fog_on, uint32_t mode, float start, float end, float density);
/* Slice s (0..d, fractional) to view depth and back: z = near (far / near)^(s / d). */
float volfog_slice_depth(float s, int d, float near_z, float far_z);
float volfog_depth_slice(float z, int d, float near_z, float far_z);
/* The share of the in-scatter at view depth z the title's fog keeps: sqrt of its fog factor
 * (geom_fog_factor; 1 without fog), so a distant lamp's glow fades the way the title fogs it. */
float volfog_fog_keep(int fog_on, uint32_t mode, float start, float end, float density, float z);

/* Scattering per view unit: VOLFOG_DENSITY x strength x (1 + VOLFOG_RAIN_GAIN rain) x
 * (1 + VOLFOG_DAMP_GAIN damp) x (indoor ? VOLFOG_INDOOR : 1); rain and damp are the eased
 * 0-1 levels (light_shafts_rain_step for the rain). The shader thins it with height above the
 * camera (VOLFOG_HEIGHT_FALLOFF) and varies it by +-VOLFOG_NOISE with a slow 3D noise. */
#define VOLFOG_DENSITY 0.03f
#define VOLFOG_RAIN_GAIN 1.5f
#define VOLFOG_DAMP_GAIN 0.3f
#define VOLFOG_INDOOR 0.4f
#define VOLFOG_HEIGHT_FALLOFF 5.0f
#define VOLFOG_NOISE 0.25f
float volfog_density(float strength, float rain, float damp, int indoor);
/* Extinction = scattering / VOLFOG_ALBEDO; only VOLFOG_EXTINCTION_MIX of its transmittance darkens
 * the scene (a touch of depth, never a second fog). */
#define VOLFOG_ALBEDO 0.9f
#define VOLFOG_EXTINCTION_MIX 0.05f

/* The lights. kind: VOLFOG_LAMP (a street lamp: a downward cone over a weaker round glow),
 * VOLFOG_FIRE (a round glow, its smoke denser), VOLFOG_NEON (a small round glow). pos: right-handed
 * view position; rgb x intensity is its light (the lamp lights' colour and gain). */
enum { VOLFOG_LAMP, VOLFOG_FIRE, VOLFOG_NEON };
typedef struct {
    float pos[3];
    float radius;      /* view units: the reach of its light */
    float rgb[3];
    float intensity;
    int kind;
} VolfogLight;
/* The lights of a frame, at most cap (<= VOLFOG_LIGHTS_MAX): fires first, then lamps, then neon,
 * each kind in its input order (the trackers' stable order: nearest and brightest first). Dropped:
 * no light (intensity or colour 0, radius <= 0), and lights whose reach does not touch the volume
 * (behind the camera, or past far_z). The reach in the air: a lamp's radius x VOLFOG_LAMP_REACH (the
 * level's lamps light the ground 15-20 units round them; their glow in the air is a few metres), neon
 * radius clamped to VOLFOG_NEON_RADIUS_MIN-MAX, a lamp's or fire's reach at least VOLFOG_REACH_MIN,
 * every reach at most VOLFOG_RADIUS_MAX. Returns the
 * lights written to out. */
#define VOLFOG_LAMP_REACH 0.5f
#define VOLFOG_REACH_MIN 4.0f
#define VOLFOG_NEON_RADIUS_MIN 3.0f
#define VOLFOG_NEON_RADIUS_MAX 6.0f
#define VOLFOG_RADIUS_MAX 12.0f
/* The share of a light's colour x intensity (the lamp lights' gain) the air scatters. */
#define VOLFOG_LIGHT_GAIN 4.2f
uint32_t volfog_select(const VolfogLight *in, uint32_t n, uint32_t cap, float far_z, VolfogLight *out);
/* Henyey-Greenstein asymmetry per kind: lamps scatter forward (a glow when looking towards them
 * through the rain), neon less, fire least. */
float volfog_phase_g(int kind);
/* The light's falloff at distance r of a light of reach radius: (1 - (r / radius)^2)^2 / (1 +
 * (r / core)^2), core = max(VOLFOG_CORE, VOLFOG_CORE_SHARE x radius): a glow of at least VOLFOG_CORE
 * view units round a small light, wider for a wide one, faded out at the reach; r clamped to at
 * least VOLFOG_R_MIN (the halo sprite stays the core: no bright ball), 0 from radius on. The shader
 * uses the same formula. */
#define VOLFOG_CORE 1.5f
#define VOLFOG_CORE_SHARE 0.22f
#define VOLFOG_R_MIN 0.5f
float volfog_falloff(float r, float radius);

/* Stage 2. A street lamp's light in the air (kind VOLFOG_LAMP): x
 * volfog_lamp_shape of the cosine between world down and the way from the lamp to the air: a defined
 * cone about world down (full inside VOLFOG_CONE_COS_INNER, none outside VOLFOG_CONE_COS_OUTER,
 * narrower than the --light-shafts cone: a defined beam) of VOLFOG_CONE_GAIN over a round glow of VOLFOG_GLOW, of which only
 * VOLFOG_GLOW_ABOVE is left above the head (the lamp's shade). The shader uses the same formula. */
#define VOLFOG_CONE_COS_OUTER 0.866f   /* 30 degrees */
#define VOLFOG_CONE_COS_INNER 0.966f   /* 15 degrees */
#define VOLFOG_CONE_GAIN 1.0f
#define VOLFOG_GLOW 0.15f
#define VOLFOG_GLOW_ABOVE 0.25f
float volfog_lamp_shape(float cos_down);
/* A lamp's light in the air at distance r (reach radius) and cosine cos_down from world down: the
 * glow (volfog_lamp_shape's round part) x volfog_falloff, plus the cone x (1 - (r / radius)^2)^2: the
 * cone keeps its light down to the ground instead of a bright ball round the head (r clamped to at
 * least VOLFOG_R_MIN, 0 from radius on). The shader uses the same formula. */
float volfog_lamp_light(float cos_down, float r, float radius);
/* The ground mist: a second density layer over the ground, its share of the air at the ground
 * volfog_mist_density (VOLFOG_MIST x strength x (1 + VOLFOG_MIST_WET x wet), wet: the larger of the
 * rain and damp levels), thinned by volfog_mist of the height over the ground: exp(-h /
 * VOLFOG_MIST_HEIGHT), h at least -VOLFOG_MIST_LOW (a dip below the ground plane holds it thicker,
 * up to that depth). Lit only by the lights (in-scatter of the port's lights alone): no veil. The air
 * above it thins with the height over the ground too (VOLFOG_HEIGHT_FALLOFF). */
#define VOLFOG_MIST 1.5f
#define VOLFOG_MIST_HEIGHT 0.9f
#define VOLFOG_MIST_LOW 0.6f
#define VOLFOG_MIST_WET 1.0f
float volfog_mist(float h);
float volfog_mist_density(float strength, float wet);
/* The ground under the camera (world z, up): the level's surface (sample, valid) or, without one,
 * VOLFOG_CAM_HEIGHT below the camera (eye_z); eased by VOLFOG_GROUND_EASE per frame from prev
 * (have_prev), taken at once on the first frame or a change of more than VOLFOG_GROUND_SNAP (a
 * camera cut, another floor). */
#define VOLFOG_CAM_HEIGHT 2.5f
#define VOLFOG_GROUND_EASE 0.05f
#define VOLFOG_GROUND_SNAP 4.0f
float volfog_ground_step(float prev, int have_prev, float sample, int valid, float eye_z);
/* The halo-tracked lamps (lamp_lights: the lamp sprites the title draws) and the level's lights
 * (level, nl; view space, up: world up in it): a halo with a level light less than VOLFOG_DEDUPE view
 * units from it across up and from VOLFOG_PAIR_ABOVE above to VOLFOG_PAIR_BELOW below it along up is
 * the same lamp (the level lights the street from a point under the lamp's head): that level light
 * moves to the halo's position (its cone hangs from the head; its own colour and reach kept) and the
 * halo is dropped. Every other halo is copied to out, at most cap. Returns the halos kept. */
#define VOLFOG_DEDUPE 3.0f
#define VOLFOG_PAIR_ABOVE 3.0f
#define VOLFOG_PAIR_BELOW 6.0f
uint32_t volfog_dedupe(const VolfogLight *halo, uint32_t nh, VolfogLight *level, uint32_t nl, const float up[3], VolfogLight *out, uint32_t cap);
/* A lamp's reach so that its cone reaches the ground: at least VOLFOG_CONE_GROUND x its head's
 * height over the ground, at most VOLFOG_RADIUS_MAX (a reach already longer stays). */
#define VOLFOG_CONE_GROUND 1.3f
float volfog_lamp_reach(float reach, float height);
/* Rain (its eased 0-1 level) lit in the air: the lights' in-scatter x volfog_rain_scatter (1 +
 * VOLFOG_RAIN_LIT rain: the drops scatter, the extinction stays the air's) and the lamps' phase
 * towards VOLFOG_RAIN_G (a forward glow looking at a lamp through the rain); the rain streaks drawn
 * after the composite brighten what lies behind them, the lit air too. */
#define VOLFOG_RAIN_LIT 1.5f
/* A lamp's phase x the rain's extra scatter is capped at VOLFOG_BOOST_MAX per light (never below the dry
 * phase: a dry frame is unchanged), and the light the
 * composite adds soft-capped above VOLFOG_ADD_KNEE towards VOLFOG_ADD_CAP (its brightest channel,
 * hue kept): a camera inside a bright lamp's cone in the rain no longer washes the frame out
 * (volfog_inject.comp, volfog_comp.frag). */
#define VOLFOG_BOOST_MAX 3.0f
#define VOLFOG_ADD_KNEE 0.15f
#define VOLFOG_ADD_CAP 0.3f
#define VOLFOG_NEAR_FADE_FROM 0.5f   /* the lights' in-scatter fades in from here to VOLFOG_NEAR_FADE_TO view units */
#define VOLFOG_NEAR_FADE_TO 3.0f
#define VOLFOG_RAIN_G 0.55f
float volfog_rain_scatter(float rain);
float volfog_phase_g_rain(int kind, float rain);

/* Temporal reprojection: the current view position to the previous frame's (prev_view, cur_view:
 * the title's world-to-view matrices, rows, translation in 12-14; v = w x V). out: 3 rows of
 * prev = out x (cur, 1). Returns 0, or 1 for a camera cut (the camera moved more than
 * VOLFOG_CUT_MOVE view units or turned more than VOLFOG_CUT_TURN degrees; a matrix that cannot be
 * inverted is one too): the history is dropped. */
#define VOLFOG_CUT_MOVE 4.0f
#define VOLFOG_CUT_TURN 35.0f
int volfog_reproject(const float prev_view[16], const float cur_view[16], float out[3][4]);
/* The current frame's weight in the froxel history: VOLFOG_HISTORY (1 after a reset). */
#define VOLFOG_HISTORY 0.1f
/* The frame's sample offsets in a froxel (0-1 each): Halton bases 2, 3, 5 of frame % 16 + 1. */
void volfog_jitter(uint32_t frame, float out[3]);
#ifdef __cplusplus
}
#endif
#endif
