/* Steam Audio: Valve's spatial audio library (Apache 2.0), built from
 * source for our 32-bit targets by third_party/steam-audio/build.sh and loaded at run time like
 * SDL2, so the runtime builds and runs without it: a missing library leaves every sound exactly as
 * before (the callers keep their own models as the fallback).
 *
 * Coordinates: Steam Audio's axes are right-handed, x right, y up, -z ahead. The title's audio
 * axes are DirectSound's (left-handed, y up); callers pass listener-relative directions already in
 * Steam Audio's axes (sa_listener_dir).
 *
 * Threads: sa_open once at start-up; the binaural objects are created, applied and freed under the
 * audio lock (the mixer thread applies them). */
#ifndef STEAM_AUDIO_H
#define STEAM_AUDIO_H
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define SA_LIB_NAME "phonon.dll"
#else
#define SA_LIB_NAME "libphonon.so"
#endif
#define SA_FRAME 240        /* frames per effect call: AUDIO_CHUNK, fixed at sa_open */
#define SA_RATE 48000

/* Loads the library and creates the context and the default HRTF (the CIPIC set built into the
 * library). path: the library file, or NULL for the program's folder and then (development trees)
 * its steam-audio/<platform>/ subfolder. Prints one "[steam-audio]" line either way. Returns 0, or
 * -1 (missing or unusable library: nothing changes). Safe to call again (returns the first result). */
int sa_open(const char *path);
int sa_ok(void);              /* 1 after a successful sa_open */
const char *sa_version(void); /* "4.8.1" (the library's), "" when not loaded */
/* Releases everything (tests; the runtime keeps it until exit). sa_open may then load it again. */
void sa_close(void);

/* The listener-relative unit direction of a source in Steam Audio's axes, from the title's
 * listener (pos, front, top: DirectSound, left-handed, right = top x front) and source position;
 * (0, 0, -1) (ahead) for a coincident source. Pure (no library needed). */
void sa_listener_dir(const float pos[3], const float front[3], const float top[3], const float src[3], float dir[3]);

/* HRTF binaural render of one mono voice. */
typedef struct SaBinaural SaBinaural;
SaBinaural *sa_binaural_new(void);     /* NULL without the library */
void sa_binaural_free(SaBinaural *b);
void sa_binaural_reset(SaBinaural *b); /* drops the filter history (a restarted voice) */
/* One SA_FRAME block: in (mono) through the HRTF for direction dir (sa_listener_dir) into
 * out_l/out_r. blend 0..1: 0 unspatialized (the same signal in both ears), 1 full HRTF (the pan's
 * narrowing inside the min distance). Bilinear HRTF interpolation. */
void sa_binaural_apply(SaBinaural *b, const float dir[3], float blend, const float *in, float *out_l, float *out_r);

/* ---- Geometry (src/acoustics.c drives these from its own thread) ----
 * An acoustic material: 3-band absorption (400 Hz, 2.5 kHz, 15 kHz), scattering, 3-band
 * transmission (Steam Audio's IPLMaterial, field for field). */
typedef struct { float absorption[3], scattering, transmission[3]; } SaMaterial;
/* A scene of one static mesh: vertices in Steam Audio's axes, triangles counter-clockwise seen from
 * their front, a material index per triangle. NULL without the library or on failure. */
typedef struct SaScene SaScene;
SaScene *sa_scene_new(const float (*vert)[3], int nvert, const int (*tri)[3], const int *mat_index, int ntri, const SaMaterial *mats, int nmat);
void sa_scene_free(SaScene *s);

/* A simulator with max_sources direct-path sources and one listener-centred reverb source; pathing: the
 * sources may also find paths through a probe batch (sa_sim_pathing). */
typedef struct SaSim SaSim;
SaSim *sa_sim_new(int max_sources, int reverb_rays, int pathing);
void sa_sim_free(SaSim *m);
void sa_sim_set_scene(SaSim *m, SaScene *s);   /* NULL: no geometry (no occlusion, no reflections) */
/* Direct path of sources src[0..n) (n <= max_sources) heard at lis, all in Steam Audio's axes:
 * occl[i] 0 (hidden) .. 1 (in view; volumetric, a sphere of radius `radius` around the source) and
 * trans[i][3] the share of each band that passes the surfaces in between (1 when none). */
void sa_sim_direct(SaSim *m, const float lis[3], const float (*src)[3], int n, float radius, float *occl, float (*trans)[3]);
/* Listener-centred reverb: reflections traced from lis (rays x bounces), the 3-band decay times
 * (RT60, seconds) of the energy that returns. Returns 0, -1 without a scene. */
int sa_sim_reverb(SaSim *m, const float lis[3], const float ahead[3], const float up[3], int bounces, float rt60[3]);
/* ---- Pathing: sound that travels around corners ----
 * Probes are spheres a fixed height over every floor of the scene on a uniform grid; a bake finds which
 * probes see each other and the shortest probe-to-probe path between every pair (all of it in the probe
 * batch). At run time a source behind something takes the baked path from its nearest probe to the probes
 * around the listener, optionally checked against the current scene (closed doors) with a real-time search
 * for another path when it is blocked. */
typedef struct SaProbes SaProbes;
/* Probes over scene s inside the box lo..hi (Steam Audio axes), spacing apart, height over each floor.
 * NULL without the library or when no floor lies in the box. */
SaProbes *sa_probes_new(SaScene *s, const float lo[3], const float hi[3], float spacing, float height);
int sa_probes_count(const SaProbes *p);
size_t sa_probes_data_size(const SaProbes *p);   /* bytes of the baked paths (0: not baked) */
typedef struct {
    float radius;      /* probe sphere for the visibility rays (m) */
    float threshold;   /* share of unblocked rays for two probes to see each other */
    float vis_range;   /* probes further apart never see each other (m) */
    float path_range;  /* longer paths are dropped (m) */
    int samples;       /* points per probe: samples^2 rays per probe pair */
    int threads;
} SaPathBake;
/* Bakes the paths between the probes in scene s. Blocking (any thread; the scene is only read). Not
 * cancellable: 4.8.1's iplPathBakerCancelBake leaves worker jobs running on freed state. Returns 0, -1. */
int sa_probes_bake(SaProbes *p, SaScene *s, const SaPathBake *b);
/* The probe batch serialized (a cache file): malloc'd bytes into *out, returns their count (0: failed). */
size_t sa_probes_save(SaProbes *p, uint8_t **out);
SaProbes *sa_probes_load(const uint8_t *d, size_t n);   /* NULL: unusable bytes or no paths in them */
void sa_probes_free(SaProbes *p);
/* The baked probes pathing uses (NULL: none); the caller keeps them until replaced or the simulator freed. */
void sa_sim_set_probes(SaSim *m, SaProbes *p);
typedef struct {
    float vis_radius, vis_threshold, vis_range;   /* as the bake's, for the run-time checks */
    int validate;   /* test the baked paths against the current scene and search again when blocked */
} SaPathQuery;
typedef struct {
    float level;    /* the paths' summed amplitude, inverse distance over their lengths (1 m minimum); 0: no path */
    float eq[3];    /* per band (400 Hz, 2.5 kHz, 15 kHz) what the bends leave (the deviation model) */
    float dir[3];   /* unit direction the paths arrive from at the listener (Steam Audio axes, not rotated) */
} SaPath;
/* Paths of src[0..n) (n <= max_sources) to lis (Steam Audio axes). A source the listener sees gets its
 * direct path (level 1 / distance, its own direction). */
void sa_sim_pathing(SaSim *m, const float lis[3], const float (*src)[3], int n, const SaPathQuery *q, SaPath *out);
#endif
