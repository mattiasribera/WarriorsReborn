/* Host audio output: a software mixer (48 kHz stereo, windowed-sinc or linear resampling per
 * voice, src/resample.h) fed by
 * voices (DirectSound buffers, XACT waves), a host backend ("sdl": SDL2 loaded with dlopen;
 * "null": no device) and an optional WAV dump of the mix.
 *
 * The mixer runs on its own host thread and is clocked by the host monotonic clock (not by the
 * device), so the dump holds the same mix whether or not a device is open. Voices are owned by
 * their creator; every voice_* call and the mixer serialize on one lock. */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>
#include "reverb.h"

#define AUDIO_RATE 48000
#define AUDIO_CHUNK 240   /* output frames per mixer step (5 ms) */

/* Source of a voice: appends up to max frames (interleaved s16, the voice's channel count) to
 * dst and returns how many it wrote; 0 = end of the sound. Called by the mixer with the audio
 * lock held. */
typedef int (*AudioFill)(void *ctx, int16_t *dst, int max);

typedef struct AudioVoice AudioVoice;

/* Opens the backend (pref "sdl" falls back to "null" when SDL2 or a device is unavailable;
 * "null" opens no device) and starts the mixer thread. dump_path (may be NULL) receives the
 * mix as a 16-bit stereo 48 kHz WAV. Returns the backend name actually opened. */
const char *audio_open(const char *pref, const char *dump_path);
void audio_close(void);              /* stops the mixer, finalizes the dump (safe to call twice) */
int audio_is_open(void);
void audio_lock(void);
void audio_unlock(void);
uint64_t audio_frames_mixed(void);    /* output frames produced since audio_open */
/* Resampler of the voices created from now on (ResampleKind, src/resample.h; default sinc). */
void audio_set_resampler(int kind);
const char *audio_resampler_name(void);
/* --stats: the mixer thread prints its CPU cost every 5 s of mixed audio. */
void audio_set_stats(int on);
/* --reverb / --no-reverb (default on): the I3DL2 room reverb (src/reverb.h) on the voices'
 * sends. audio_set_i3dl2 applies the title's listener parameters (the default, lRoom -10000,
 * is silent); they are kept while the reverb is off. */
void audio_set_reverb(int on);
int audio_reverb_enabled(void);
void audio_set_i3dl2(const ReverbI3dl2 *p);
void audio_get_i3dl2(ReverbI3dl2 *p);

/* Voices (create/destroy/set_* take the lock themselves). A new voice is stopped. */
AudioVoice *voice_new(int channels, uint32_t rate, AudioFill fill, void *ctx);
void voice_free(AudioVoice *v);
void voice_play(AudioVoice *v, int on);       /* on=0 stops; the source position is kept */
int voice_playing(const AudioVoice *v);       /* 0 once the source reported its end */
void voice_set_rate(AudioVoice *v, uint32_t rate);
/* Gain per output channel (linear, 1.0 = unity) for each source channel: g[src][0..1] = L, R. */
void voice_set_gains(AudioVoice *v, const float g[2][2]);
void voice_set_volume(AudioVoice *v, float gain);   /* overall linear gain, multiplies g */
void voice_reset(AudioVoice *v);              /* drops buffered source frames (after a seek) */
/* Reverb send (linear; 0 = dry, the default): the voice's source channels, averaged, before
 * the output gains and volume, so a send keeps its own distance law. */
void voice_set_send(AudioVoice *v, float gain);
/* Doppler: a ratio multiplying the voice's rate (1 = none, the default). A playing voice eases to
 * a new ratio over about 20 ms; a stopped one takes it at once. voice_pitch returns the target. */
void voice_set_pitch(AudioVoice *v, float ratio);
float voice_pitch(const AudioVoice *v);
/* One-pole low-pass on the voice (coefficient a in (0, 1); 1 or out of range = bypass): the
 * rear cue of a 3D voice, see audio_rear_lowpass. */
void voice_set_lowpass(AudioVoice *v, float a);

/* Mixes n output frames (interleaved s16 stereo) from the current voices without the lock
 * (the caller holds it). Exposed for tests; the mixer thread uses it. */
void audio_mix(int16_t *out, int n);

/* Xbox ADPCM (IMA variant, 36-byte blocks per channel, 64 samples per block; stereo blocks
 * interleave 4-byte words). Decodes one block of 36*channels bytes into 64 frames. */
#define XADPCM_BLOCK_SAMPLES 64
void xadpcm_decode_block(const uint8_t *in, int channels, int16_t *out);

/* Writes 16-bit PCM WAV headers; the data size fields are patched by wav_finish. */
int wav_write_header(void *file, int channels, uint32_t rate);
void wav_finish(void *file, uint32_t data_bytes);

/* Parses "--audio" style volume: hundredths of a decibel (DirectSound) to a linear gain. */
float audio_db100_to_gain(int32_t db100);

/* DirectSound 3D helpers (the model XACT 1 3D sounds follow on the Xbox). Distance gain:
 * min / (min + rolloff * (d - min)) with d clamped to [min, max] (inverse distance; no further
 * attenuation beyond max). A min distance <= 0 or rolloff <= 0 gives 1. */
float audio_ds3d_distance_gain(float dist, float min_dist, float max_dist, float rolloff);
/* Stereo gains lr[0..1] (1/1 = centered, constant power) for a source at src heard by a
 * listener at pos with orientation front/top, in DirectSound's left-handed axes (right =
 * top x front). The pan narrows inside min_dist so a source passing through the listener does
 * not jump between the ears. */
void audio_ds3d_pan(const float pos[3], const float front[3], const float top[3], const float src[3], float min_dist,
                    float lr[2]);
/* Doppler ratio (DirectSound 3D model, Doppler factor 1): (c - vl.u) / (c - vs.u), u the unit
 * vector from the source to the listener, c the speed of sound in units per second. Each
 * projected speed is clamped to +-c/2, the ratio to [1/max_ratio, max_ratio]; coincident
 * positions give 1. */
float audio_doppler_ratio(const float lis_pos[3], const float lis_vel[3], const float src_pos[3], const float src_vel[3],
                          float c, float max_ratio);
/* Velocity of an emitter or the listener from its position updates over a clock t (seconds):
 * the position delta per update, smoothed with a 0.1 s time constant. Updates less than 1 ms
 * apart are folded into the next one; a jump faster than max_speed (a teleport, a reused
 * source) or a gap over 0.5 s restarts from velocity 0. */
typedef struct { float pos[3], vel[3]; double t; int have; } AudioVelocity;
void audio_velocity_update(AudioVelocity *e, const float pos[3], double t, float max_speed);
/* Front/back cue: the low-pass coefficient (voice_set_lowpass) for a source at src heard by a
 * listener at pos facing front. 1 (bypass) in front and to the sides; behind it falls linearly
 * with the rear component to the one-pole coefficient of a 5 kHz cutoff straight behind. Inside
 * min_dist the cue fades out like the pan. */
float audio_rear_lowpass(const float pos[3], const float front[3], const float src[3], float min_dist);
#endif
