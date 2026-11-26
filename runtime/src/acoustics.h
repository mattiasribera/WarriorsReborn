/* Geometry-aware sound: Steam Audio's direct-path simulation (occlusion
 * and transmission through the level's surfaces) and listener-centred reflections (the reverb
 * times of the place), from the level's collision mesh on the player's own disc.
 *
 * The level's collision triangles (materials.h: the .lev's class 4/7 chunks with a MATERIAL byte
 * per triangle) become one Steam Audio static mesh; each MATERIAL id maps to an acoustic material
 * (ac_material). Camera ghosts are left out, and so are the door panels the title opens (ac_doors).
 * An acoustics thread builds the scene and runs the simulations (direct paths every title frame,
 * reflections when the listener reaches a spot not traced yet: AC_REVERB_CELL_M) and publishes the results; the callers only exchange
 * positions and results under a short lock, so no ray is traced on the guest or the mixer thread.
 *
 * Axes: queries come in the title's audio axes (DirectSound: left-handed, y up; the world is (x, z,
 * y) of them). Steam Audio's are right-handed y up: (x, y, -z) of the audio
 * axes, which is (x, z, -y) of the world's (a rotation, so triangle windings stay as they are).
 *
 * Nothing derived from the disc is written anywhere. */
#ifndef ACOUSTICS_H
#define ACOUSTICS_H
#include <stdint.h>
#include "materials.h"
#include "steam_audio.h"

/* The acoustic material of a collision MATERIAL id: Steam Audio's standard presets (generic, brick,
 * concrete, gravel, glass, plaster, wood, metal) by the id's class (mat_class) and a few ids of their
 * own: drywall plaster, the door panels (material 0) wood, chain-link fences and railings almost
 * transparent. */
void ac_material(uint8_t mat, SaMaterial *out);
/* 1 when triangles of MATERIAL id mat belong in the acoustic scene (0: camera ghosts). */
int ac_tri_kept(uint8_t mat);
/* World (z up) and audio (DirectSound) positions to Steam Audio's axes. */
void ac_world_to_sa(const float w[3], float o[3]);
void ac_audio_to_sa(const float a[3], float o[3]);

/* The direct path's result as the existing muffling stage takes it (hle_audio.c, audio_occlusion's
 * gain and one-pole coefficient): per band the share that arrives, e = occl + (1 - occl) trans; the
 * gain e(400 Hz)^AC_GAIN_EXP, at least AC_GAIN_MIN (the paths around corners and through openings
 * that a direct-path model leaves out: a concrete or brick wall about -9 dB, as one wall of the wall
 * map's -6 dB); the tone a one-pole low-pass whose response at 2.5 kHz is
 * (e(2.5 kHz) / e(400 Hz))^AC_TONE_EXP x (1 - (1 - AC_HIDDEN_TONE) (1 - e(400 Hz))) (the material's own
 * tilt, and the highs lost bending around what blocks the low band: brick about 1.9 kHz, glass 1.6 kHz, wood 900 Hz,
 * concrete 780 Hz), no lower than AC_LP_MIN_HZ; nearly in the clear (above 0.98) a bypass. */
#define AC_GAIN_EXP 0.25f
#define AC_GAIN_MIN 0.2f
#define AC_TONE_EXP 0.35f
#define AC_HIDDEN_TONE 0.6f
#define AC_LP_MIN_HZ 400.0f
void ac_direct_to_muffle(float occl, const float trans[3], float *gain, float *lp);

/* Sound around corners (--audio-pathing): the direct path's result with the paths that bend through the
 * openings (sa_sim_pathing) when the direct path is blocked (occl < AC_PATH_OCCL). A path's amplitude
 * share is its level over a free path's at the direct distance d (1 / max(d, 1)) times its 400 Hz bend
 * gain, at most 1; it is ignored when it arrives within AC_PATH_SAME_DEG of the source's own direction
 * (then it is the direct path the volumetric occlusion already measured). Energies add: the gain is
 * sqrt(gd^2 + gp^2) (gd the direct path's gain without its AC_GAIN_MIN floor), at least that floor and at
 * most 1, so a source with a path is never quieter than with the direct path alone; the 2.5 kHz response
 * is the energy-weighted mix of the direct tone and the path's bend (eq 2.5 kHz / 400 Hz), and the
 * direction the energy-weighted sum of the source's own (dir_src) and the paths' (unit vectors, any one
 * axis convention). No path (p NULL or level 0): exactly ac_direct_to_muffle and dir_src. */
#define AC_PATH_OCCL 0.98f
#define AC_PATH_SAME_DEG 5.0f
void ac_mix_paths(float occl, const float trans[3], const SaPath *p, float d, const float dir_src[3], float *gain, float *lp, float dir[3]);

/* Reverb from the place (--audio-reverb-geo): the title's I3DL2 decay time and HF ratio replaced by
 * the reflections' RT60: decay = the mean of the 400 Hz and 2.5 kHz bands, the HF ratio the decay at
 * hf_ref Hz (log-interpolated between the 2.5 and 15 kHz bands) over it; clamped to the I3DL2 ranges
 * (0.1-20 s, 0.1-2). Returns 0, -1 for an unusable result (nothing comes back: an open space). */
int ac_reverb_to_i3dl2(const float rt60[3], float hf_ref, float *decay, float *hf_ratio);

/* ---- runtime ---- */
#define AC_MAX_SOURCES 32         /* sources simulated per run (the most recently queried) */
#define AC_SOURCE_RADIUS 0.5f     /* volumetric occlusion sphere around a source, units (m) */
#define AC_SOURCE_LIFT 1.0f       /* a source below the listener is lifted up to this (as the wall map) */
#define AC_STALE_S 0.5            /* a source not queried for this long leaves the simulation */
#define AC_REVERB_RAYS 1024
#define AC_REVERB_BOUNCES 32      /* the decay of a closed concrete room comes out about 1 s (more bounces: longer, dearer) */
/* Reverb on demand: one run of the reflections costs 25-65 ms of a core
 * on a large level, and a spot's result only changes with the place, so they are traced per cell of
 * AC_REVERB_CELL_M (Steam Audio axes, a cube) and kept: a cell's result is the mean of its first
 * AC_REVERB_CELL_RUNS runs (one run alone varies by about a third: 1024 rays), a still listener's cell
 * is refined every AC_REVERB_GAP_S until it has them and then never traced again, and a known cell
 * (also of another scene: a door shut again) is published from the cache with no run. The cell is only
 * looked up again after the listener has moved AC_REVERB_MOVE_M from where it was last looked up, so
 * standing on a cell edge switches nothing; a new scene is looked up at once. At most one run per
 * AC_REVERB_GAP_S (none of that wait without the acoustics thread: tests); the least recently used of
 * AC_REVERB_CELLS cells is forgotten. The reverb's own 0.5 s easing (main.c geo_listener) smooths the
 * steps between cells. */
#define AC_REVERB_CELL_M 1.5f
#define AC_REVERB_CELL_RUNS 4
#define AC_REVERB_GAP_S 0.5
#define AC_REVERB_MOVE_M 0.5f
#define AC_REVERB_CELLS 512
typedef struct {
    int32_t c[3];
    uint32_t scene;      /* the scene's key (the mesh and its open doors) */
    float rt60[3];       /* the mean of the runs with a tail (0 s: none had one) */
    uint16_t runs, tails;
    uint32_t used;       /* LRU tick */
} AcRevCell;
typedef struct {
    AcRevCell cell[AC_REVERB_CELLS];
    int n, cur;          /* cells in use; the published one (-1: none) */
    uint32_t tick, scene;   /* scene: the one last looked up */
    float at[3];         /* where the cell was last looked up */
    int have_at;
    double last_run;
} AcRevCache;
enum { AC_REV_IDLE, AC_REV_PUBLISH, AC_REV_TRACE };
void ac_rev_init(AcRevCache *c);
/* What to do for listener lis (Steam Audio axes) in scene `scene` at time t (s): AC_REV_TRACE (run the
 * reflections there, then ac_rev_result), AC_REV_PUBLISH (a known cell's result in rt60) or AC_REV_IDLE. */
int ac_rev_plan(AcRevCache *c, uint32_t scene, const float lis[3], double t, float rt60[3]);
/* A run's result at lis into its cell; the cell's result to publish into out. */
void ac_rev_result(AcRevCache *c, uint32_t scene, const float lis[3], double t, const float rt60[3], float out[3]);
/* A finished cell (AC_REVERB_CELL_RUNS runs, from the cache file) into c unless c knows it: 1 when added. */
int ac_rev_put(AcRevCache *c, const AcRevCell *cell);
/* Reverb cache files (with ac_set_cache_dir): each level's finished cells, reverb-<level>-<hash of the
 * mesh and settings>.bin beside the paths, written at a level change and at most every AC_REVERB_SAVE_S
 * while new cells finish; read at the level's load (a cell's scene key is the mesh's bytes and its open
 * doors, so a file serves every door state). */
#define AC_REVERB_SAVE_S 20.0
#define AC_REVERB_CACHE_VERSION 1
/* Pathing probes and bake: a probe every AC_PATH_SPACING m, AC_PATH_HEIGHT
 * over each floor; more than AC_PATH_MAX_PROBES widens the spacing (the bake's memory grows with the square
 * of the count: about 100 MB at 1650 probes, 32-bit process). Probes see each other through 1 m spheres
 * (one ray per pair, 10% unblocked) up to AC_PATH_VIS_RANGE apart; paths up to AC_PATH_RANGE long. The
 * bake scene leaves every door panel out (all doors open); at run time the baked paths are checked
 * against the scene with the closed doors in it, and a blocked one is searched again. */
#define AC_PATH_SPACING 4.0f
#define AC_PATH_HEIGHT 1.5f
#define AC_PATH_MAX_PROBES 2000
#define AC_PATH_VIS_RADIUS 1.0f
#define AC_PATH_VIS_THRESHOLD 0.1f
#define AC_PATH_VIS_RANGE 30.0f
#define AC_PATH_RANGE 100.0f
#define AC_PATH_CACHE_VERSION 1
/* Starts the acoustics (the library must be loaded: sa_ok). threaded 0: no thread, the caller runs
 * ac_step (tests; a pathing bake then runs inside ac_step too). reverb: also trace the listener-centred
 * reflections. pathing: sound around corners (each level's paths baked on a low-priority thread, or
 * read from the cache). Returns 0, -1. */
int ac_start(int threaded, int reverb, int pathing);
/* Where baked paths are kept, one file per level (pathing-<level>-<hash of the mesh and settings>.bin;
 * derived from the player's own disc, never shipped): NULL or "" for none (baked again at every load).
 * Before ac_start. */
void ac_set_cache_dir(const char *dir);
/* --stats and tests: the current level's probes (0: none yet) and whether they came from the cache. */
int ac_pathing_probes(int *from_cache);
void ac_stop(void);
int ac_running(void);
/* A new level's collision mesh (copied; gen: the load's number, door lists name it), or none (NULL). */
void ac_set_mesh(const MatMesh *m, uint32_t gen, const char *name);
/* Reads level `level`'s .lev from the disc and hands its mesh over (the guest thread at a load). */
void ac_level(const char *disc, const char *level, uint32_t gen);
/* The collision triangles the title has opened now (doors, increasing), for load gen: the scene is
 * rebuilt without them when the list changes (on the acoustics thread). */
void ac_doors(uint32_t gen, const uint16_t *open, uint32_t n);
/* The muffling of source `key` (a non-zero id, as the title's source handle) at src heard at lis,
 * both in audio axes: the latest result (one title frame old) into *gain, *lp, and the positions
 * queued for the next run. pos: where the sound seems to come from (audio axes; with pathing the
 * opening it bends through, at the source's distance; src otherwise). Returns 0 with a result, -1 when
 * there is none yet (no scene for this level, or a new source): the caller keeps its own model then. */
int ac_query(uint32_t key, const float lis[3], const float src[3], float *gain, float *lp, float pos[3]);
/* The listener's pose for the reflections (audio axes). */
void ac_listener(const float pos[3], const float front[3], const float top[3]);
/* The latest listener-centred RT60 per band; returns its sequence number (0 = none: no scene). */
uint32_t ac_reverb(float rt60[3]);
/* One update: scene changes, direct paths of the queued sources, the reflections when due. */
void ac_step(void);
/* --stats: the acoustics cost since the last call: "[stats] acoustics ..." line into buf. */
void ac_stats(char *buf, int n);
/* --stats: the thread prints that line every 5 s, with its CPU time as a share of one core. */
void ac_set_stats(int on);
#endif
