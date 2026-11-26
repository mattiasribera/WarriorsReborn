/* High-level emulation (HLE) of the audio API boundary: the title-facing XACT engine and
 * DirectSound entry points are replaced with host functions (DirectSound buffers and XACT cues
 * played by the host mixer in audio.c), so the statically linked XACTENG/DSOUND code never
 * touches MCPX APU/AC97 MMIO. */
#ifndef HLE_AUDIO_H
#define HLE_AUDIO_H
#include <stdint.h>

typedef uint32_t (*HleLookup)(const char *name);   /* symbol -> address, 0 if unknown */

/* Patches every known entry point with a jmp to a host stub. An entry is patched only when its
 * first bytes match the code this table was derived from (otherwise it is reported and skipped).
 * lookup may be NULL; when given, a symbol that resolves to a different address is skipped too.
 * Returns the number of patched entries. */
int hle_audio_install(HleLookup lookup);
void hle_audio_summary(void);          /* one line: total HLE calls and per-function counts */
/* One "[i3dl2] summary" line: whether the title set an I3DL2 listener, its velocity, orientation and
 * mix-bin call counts (the "[i3dl2]" log lines record the values themselves). */
void hle_audio_i3dl2_summary(void);
extern int hle_audio_log_limit;        /* log lines per function (default 32, 0 = silent) */
/* --audio-levels tags (voice_set_tag): an XACT cue's voices carry the cue index, DirectSound
 * buffers (Bink movie audio) this value. */
#define HLE_AUDIO_TAG_DSOUND 0x100000
/* --sfx-3d-volume: extra level in dB on every 3D (positioned) XACT cue, default 0. 2D cues,
 * DirectSound buffers (Bink, streams) are unchanged. parse accepts a decimal number in
 * HLE_AUDIO_SFX3D_MIN_DB..HLE_AUDIO_SFX3D_MAX_DB and returns 0, else -1 (*db untouched). */
#define HLE_AUDIO_SFX3D_MIN_DB (-40.0f)
#define HLE_AUDIO_SFX3D_MAX_DB 12.0f
int hle_audio_parse_db(const char *s, float *db);
/* Player volumes (--dialogue-volume, --music-volume, --sfx-volume DB, same range and parser as
 * --sfx-3d-volume, default 0): an extra level on every XACT cue of the class, 2D and 3D, on top of
 * the game's own levels. Movies (Bink, DirectSound buffers) are unchanged, also the dialogue in
 * them. hle_audio_cue_class: music = category 1; dialogue = the spoken-line cues of this title's
 * sound bank (see hle_audio.c); everything else is a sound effect. --sfx-3d-volume applies to
 * 3D sound effects only, not to dialogue. */
enum { HLE_AUDIO_CLASS_SFX, HLE_AUDIO_CLASS_MUSIC, HLE_AUDIO_CLASS_DIALOGUE, HLE_AUDIO_NCLASSES };
void hle_audio_set_class_volume(int cls, float db);
struct XsbBank;
int hle_audio_cue_class(const struct XsbBank *sb, uint32_t index, int category);
void hle_audio_set_sfx_3d_volume(float db);
float hle_audio_sfx_3d_gain(void);   /* linear */
/* --no-xact-controls: 0 plays every cue at its sound entry volume, ignoring the parameter
 * controls ("V", "F") the title sets in Prepare/PlayEx and SetParameterControl (default 1). */
void hle_audio_set_xact_controls(int on);
int hle_audio_xact_controls(void);
/* --doppler / --no-doppler (default on): Doppler pitch on positioned (3D) cues, from the
 * velocities the title sets or, when it sets none, from the position deltas over hle_audio_clock.
 * --rear-cue / --no-rear-cue (default on): a low-pass on 3D cues behind the listener. 2D cues,
 * DirectSound buffers and streams are unchanged. */
/* --no-rolloff-curves: attenuate 3D cues by the inverse distance law instead of the rolloff curve
 * of their sound bank 3D record (the Xbox uses the curve; default on). For comparison. */
void hle_audio_set_rolloff_curves(int on);
void hle_audio_set_doppler(int on);
void hle_audio_set_rear_cue(int on);
#define HLE_AUDIO_SPEED_OF_SOUND 343.0f   /* units per second (DirectSound 3D: 1 unit = 1 m) */
#define HLE_AUDIO_DOPPLER_MAX 2.0f        /* the ratio is clamped to [1/2, 2] */
/* 3D audio, mono positioned (3D) cues on a positioned source only; 2D cues, stereo
 * waves, DirectSound buffers and streams are unchanged:
 * --audio-hrtf (default off): binaural headphone render (audio.h audio_binaural) instead of the
 *   stereo pan.
 * --audio-air / --no-audio-air (default on): distance air absorption (audio_air_lowpass).
 * --audio-occlusion / --no-audio-occlusion (default on): sources behind walls quieter and duller
 *   (audio_occlusion), from the occluder: the walls between the listener and a source, both in
 *   the title's audio axes (main.c: hle_d3d_audio_walls, the level's wall map). The wall count
 *   eases with a 0.12 s time constant on hle_audio_clock; a source's first query takes it at once.
 *   Without an occluder nothing changes. */
void hle_audio_set_hrtf(int on);
void hle_audio_set_air(int on);
void hle_audio_set_occlusion(int on);
void hle_audio_set_occluder(int (*walls)(const float lis[3], const float src[3]));
/* Steam Audio: with --audio-occlusion, the level's geometry through Steam Audio
 * (src/acoustics.c ac_query: occlusion and transmission per material, as a gain and a one-pole) takes
 * the place of the wall count for a source whenever it has a result (key: the title's source handle);
 * eased with the same time constant. In the [3d] log the "walls" field then shows minus that gain. */
/* pos: set by the occluder to where the sound seems to come from (pathing: the opening it bends through;
 * it starts as src); the pan and the binaural render take it, eased as an offset from the source. */
void hle_audio_set_geo_occluder(int (*q)(uint32_t key, const float lis[3], const float src[3], float *gain, float *lp, float pos[3]));
/* Called every XACTEngineDoWork with the listener's pose (audio axes): --audio-reverb-geo. */
extern void (*hle_audio_listener_hook)(const float pos[3], const float front[3], const float top[3]);
#define HLE_AUDIO_OCCLUSION_TAU 0.12
/* The title's clock in seconds for the position deltas (main.c: the frame-locked counter the title
 * reads); NULL = the mixed audio clock. */
extern double (*hle_audio_clock)(void);
/* An XACT cue whose waves ran out stays playing for this long after its last sample was mixed
 * (audio.h voice_end_frame): its source reads as busy (IXACTSoundSource_GetStatus) and the stop
 * notification waits until then. The console's XACT does not release a finished cue at once: the
 * Armies of the Night "Go" beep (vags/misc/bleep27, 174 ms), which the HUD replays as soon as the
 * previous one is no longer playing, repeats every 266.7 ms on the console (8 frames at 30 Hz, so
 * released 59-93 ms after its end) and every 200 ms without the latency.
 * Stop calls and looping cues are unchanged. */
#define HLE_AUDIO_XACT_STOP_LATENCY_MS 65
/* Host file descriptor behind a guest file handle (for streamed wave banks), -1 if none. */
extern int (*hle_audio_handle_fd)(uint32_t handle);

/* Mod music channel (src/hle_modmenu.c, docs/MODDING.md "Menus and music"): cues of the title's sound
 * bank played for a mod, each at a level (linear, 0..1, on top of the cue's own volume and the
 * music volume), plus a set of the game's own cues kept at a common level while a mod's music
 * plays (the hideout radio's songs). Call on the title's thread (the mod's Lua, XACTEngineDoWork).
 * valid: the main sound bank has cue. start: a new voice slot (> 0, 0 = failed) playing cue once or
 *   looping; on_radio: on the sound source of the last muted game cue played (positioned where the
 *   game's radio is, at the game's radio level), else heard as a 2D sound. alive: still playing.
 * mute_set replaces the set (and forgets the radio's source); game_level sets their level (1 = unchanged). hle_audio_music_tick runs
 * every XACTEngineDoWork (the fades). */
int hle_audio_music_valid(uint32_t cue);
int hle_audio_music_start(uint32_t cue, int loop, int on_radio, float level);
void hle_audio_music_level(int id, float level);
int hle_audio_music_alive(int id);
void hle_audio_music_stop(int id);
void hle_audio_music_mute_set(const uint32_t *cue, int n);
void hle_audio_music_game_level(float level);
extern void (*hle_audio_music_tick)(void);

/* The title's listener (audio axes): 1 once the title set its position, else 0 (a mod's 3D sound,
 * src/modsound.h). Any thread. */
int hle_audio_listener_get(float pos[3], float front[3], float top[3]);

/* Table access for tests. */
int hle_audio_entry_count(void);
uint32_t hle_audio_entry_addr(int i);
int hle_audio_entry_argbytes(int i);
const char *hle_audio_entry_name(int i);
const uint8_t *hle_audio_entry_sig(int i);   /* HLE_SIG_LEN bytes */
#define HLE_SIG_LEN 5
#endif
