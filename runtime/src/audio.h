/* Host audio output: a software mixer (48 kHz stereo, windowed-sinc or linear resampling per
 * voice, src/resample.h) fed by
 * voices (DirectSound buffers, XACT waves), a host backend ("sdl": SDL2 loaded with dlopen;
 * "null": no device) and an optional WAV dump of the mix.
 *
 * The mixer runs on its own host thread. Without a device (or with --audio-lead-ms 0) it is
 * clocked by the host monotonic clock. With an SDL device it keeps the device queue at the
 * output lead (default 50 ms): it prefills the queue, mixes ahead whenever the device pulled
 * frames and refills at once after an underrun, so the device clock paces the mix; a device
 * that stops consuming falls back to the host clock (mixed, not queued) so DirectSound cursors
 * and cue ends keep moving. Voices are owned by their creator; every voice_* call and the mixer
 * serialize on one lock. */
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
/* --audio-levels: after every second of mixed audio, prints "[levels] t=S all A rev R lim G TAG:L ...":
 * the RMS level in dBFS (both channels, before the peak limiter) of the whole mix, of the reverb
 * return, the lowest peak-limiter gain in dB (0.0 = it did not engage) and of every voice tag
 * that sounded in that second (voice_set_tag; -1 untagged). */
void audio_set_levels(int on);
/* --reverb / --no-reverb (default on): the I3DL2 room reverb (src/reverb.h) on the voices'
 * sends. audio_set_i3dl2 applies the title's listener parameters (the default, lRoom -10000,
 * is silent); they are kept while the reverb is off. */
void audio_set_reverb(int on);
int audio_reverb_enabled(void);
void audio_set_i3dl2(const ReverbI3dl2 *p);
void audio_get_i3dl2(ReverbI3dl2 *p);

/* --audio-lead-ms N (set before audio_open): output lead the mixer keeps queued on the SDL
 * device, 0..AUDIO_LEAD_MAX_MS. 0 = the host-clocked queue of earlier builds (1024-frame device
 * periods, chunks due by the host clock, a chunk dropped above 150 ms queued). */
#define AUDIO_LEAD_DEFAULT_MS 50
#define AUDIO_LEAD_MAX_MS 500
void audio_set_lead_ms(int ms);
int audio_lead_ms(void);
/* Frames mixed but still queued for the device (0 without a device or without a lead): the
 * DirectSound play cursor is held back by them (audio_cursor_behind). */
uint32_t audio_output_queued_frames(void);

/* Output lead planner (pure; the mixer thread calls it every loop with the device queue level).
 * The queue is kept at lead frames: below it, enough chunks to reach it are mixed and queued
 * (prefill, top-up, the refill after an underrun). A device that consumed nothing for the stall
 * time (250 ms or four periods) is treated as stalled: chunks are mixed on the host clock
 * from the last consumed position and not queued. */
typedef struct {
    uint32_t lead;                     /* target queued frames */
    uint64_t stall_ns;
    uint64_t sent;                     /* frames queued so far */
    uint64_t alive_pos, alive_ns;      /* device position (sent - queued) when it last moved */
    uint64_t host;                     /* frames mixed without queueing since then (stalled device) */
    int started;
} AudioLead;
typedef struct {
    int chunks;      /* AUDIO_CHUNK-frame chunks to mix now */
    int send;        /* 1: queue them on the device; 0: stalled device, mix only */
    int underrun;    /* the queue ran dry since the last call */
} AudioLeadPlan;
void audio_lead_init(AudioLead *l, uint32_t lead_frames, uint32_t period);
AudioLeadPlan audio_lead_plan(AudioLead *l, uint32_t queued, uint64_t now_ns);
/* Device period to request for a lead of ms: the largest power of two up to half the lead,
 * 256..1024 frames. */
uint32_t audio_lead_period(int ms);
/* Lead in frames for ms with the device's period: at least two periods (0 ms = 0). */
uint32_t audio_lead_frames(int ms, uint32_t period);
/* A DirectSound play cursor (bytes; frame_bytes per frame) moved back by lag frames, at most by
 * the consumed frames read since Play or a cursor change: looping buffers wrap below 0 to the
 * end (bytes rounded down to whole frames), one-shot buffers stop at 0. */
uint32_t audio_cursor_behind(uint32_t cursor, uint32_t bytes, uint32_t frame_bytes, uint32_t lag, uint64_t consumed, int looping);

/* Xbox headroom (--no-xbox-headroom turns it off; default on). The console's DirectSound gives
 * every new voice a headroom of 600 mB (-6 dB) unless its creation flags include any of
 * 0x182000 or 0x200010 (XBE 0x3ac844); the voice plays at its volume minus its headroom
 * (0x3a8457), SetHeadroom changes it. XACT wave voices (flags 0x400000) keep 600 mB, BINK sets 0.
 * audio_ds_default_headroom: the default for creation flags, in hundredths of a dB. */
#define AUDIO_XBOX_HEADROOM 600
void audio_set_xbox_headroom(int on);
int audio_xbox_headroom(void);
int32_t audio_ds_default_headroom(uint32_t flags);

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
/* Tag of the voice for --audio-levels (the HLE uses the XACT cue index; -1 = untagged). */
void voice_set_tag(AudioVoice *v, int32_t tag);

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
/* Distance gain with an Xbox DirectSound rolloff curve (SetRolloffCurve; full-HRTF calculator
 * 0x3af4c8): n linear gains at min + k * (max - min) / n for k = 1..n, 1.0 at the min distance,
 * linear in between; 1 inside min, the last point at and beyond max (this title's curves end at
 * 0: silent). n = 0 (or no curve) falls back to audio_ds3d_distance_gain with rolloff. */
float audio_ds3d_curve_gain(float dist, float min_dist, float max_dist, const float *curve, int n, float rolloff);
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
