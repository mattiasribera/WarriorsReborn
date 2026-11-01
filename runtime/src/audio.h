/* Host audio output: a software mixer (48 kHz stereo, linear-interpolation resampling) fed by
 * voices (DirectSound buffers, XACT waves), a host backend ("sdl": SDL2 loaded with dlopen;
 * "null": no device) and an optional WAV dump of the mix.
 *
 * The mixer runs on its own host thread and is clocked by the host monotonic clock (not by the
 * device), so the dump holds the same mix whether or not a device is open. Voices are owned by
 * their creator; every voice_* call and the mixer serialize on one lock. */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>

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
#endif
