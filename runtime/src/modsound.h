/* A mod's own sounds (docs/MODDING.md "Your own assets", Sounds): the [sound NAME] outputs of its
 * assets.ini (16-bit WAV files <NAME>.wav in its asset cache, src/modassets.h), loaded into memory
 * at mod load and played from the mod's Lua through the host mixer (src/audio.h voices), 2D or at a
 * world position (3D: distance and pan from the game's listener, updated every XACTEngineDoWork).
 *
 * Lua (bound to the mod whose file runs, like ModSave: keep the copy the file sees):
 *   ModSound(name [, x, y, z] [, volume [, loop]]) -> handle or nil   volume 0..1 (default 1); loop 1
 *   ModSoundMove(handle, x, y, z)   ModSoundStop(handle)
 * Budgets: MODSOUND_BYTES_MAX of samples per mod, MODSOUND_PLAYING_MAX playing per mod; a name of
 * another mod, a bad handle or a full budget gives nil and one log line. A new Lua state (a level
 * load) stops every mod sound.
 *
 * The audio goes through ModSoundAudio (main.c wires the mixer; tests pass fakes), so this file has
 * no audio dependency. World positions are the game's (z up); the listener is in its audio axes
 * (y up): world (x, y, z) = audio (x, z, y). */
#ifndef MODSOUND_H
#define MODSOUND_H
#include <stddef.h>
#include <stdint.h>

#define MODSOUND_BYTES_MAX (16u << 20)
#define MODSOUND_PLAYING_MAX 32
#define MODSOUND_MIN_DIST 2.0f    /* m: full level within it */
#define MODSOUND_MAX_DIST 40.0f   /* m: silent beyond it */

typedef int (*ModSoundFill)(void *ctx, int16_t *dst, int max);
typedef struct {
    void *(*voice_new)(int channels, uint32_t rate, ModSoundFill fill, void *ctx);
    void (*voice_free)(void *v);
    void (*voice_play)(void *v, int on);
    int (*voice_playing)(const void *v);
    void (*voice_gains)(void *v, const float g[2][2]);
    /* the listener in audio axes; 0 when unknown (a 3D sound then plays centred) */
    int (*listener)(float pos[3], float front[3], float top[3]);
    void (*lock)(void), (*unlock)(void);   /* the mixer lock (the fill reads a voice's state) */
} ModSoundAudio;
void modsound_setup(const ModSoundAudio *a);

/* Loads the <NAME>.wav files of a mod's cache folder for mod id: the number loaded (budget and
 * format problems are logged and skipped). */
int modsound_add_dir(const char *id, const char *dir);
int modsound_count(const char *id);   /* sounds loaded for the mod (NULL: all) */
/* Plays sound name of mod id: pos NULL = 2D, else a world position. Returns a handle > 0, or 0 with
 * the reason in err (no such sound, budget, no audio). */
int modsound_play(const char *id, const char *name, const float *pos, float volume, int loop, char *err, size_t cap);
int modsound_move(const char *id, int handle, const float *pos);   /* 0, -1 bad handle */
int modsound_stop(const char *id, int handle);                     /* 0, -1 bad handle */
int modsound_playing(const char *id);                              /* sounds playing for the mod */
/* Every XACTEngineDoWork: 3D gains from the listener; finished voices freed. */
void modsound_tick(void);
void modsound_new_state(void);   /* stops every sound */
void modsound_reset(void);       /* forgets everything (tests) */

/* The gains of a positioned sound (pure; audio axes): distance law between MODSOUND_MIN_DIST and
 * MODSOUND_MAX_DIST (inverse, then a fade to 0), a constant-power pan by the angle to the
 * listener's right; g[src channel][L, R]. */
void modsound_gains(const float lis[3], const float front[3], const float top[3], const float src[3], float volume, int channels, float g[2][2]);
#endif
