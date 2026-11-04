/* Band-limited resampling of the mixer's voices (src/audio.c): a polyphase windowed-sinc kernel
 * (default) or the earlier linear interpolation (--resampler linear).
 *
 * Sinc: 64 taps, Kaiser window (beta 8.96, about 90 dB stopband) around a cutoff at the source
 * Nyquist, so the passband is flat to about 0.45 fs_in and the images of a 0.45 fs_in tone (at
 * 0.55 fs_in and above) are about 90 dB down. The coefficients are tabulated at 1024 phases per
 * sample and interpolated linearly between adjacent phases, so any ratio (and a ratio that
 * changes between calls: XACT pitch, Doppler, DirectSound SetFrequency) works without a table
 * per ratio. When the source is faster than the output (step > 1) the kernel is stretched by
 * the step (cutoff at the output Nyquist, up to RESAMPLE_MAX_SCALE) so it does not alias.
 * At ratio 1 the sinc kernel is an exact delay (its zeros sit on the integer taps), so 48 kHz
 * sources such as the Bink movie audio pass through unchanged.
 *
 * Timing: output k of a block is the band-limited source value at position pos + k * step
 * (source frames, relative to the src pointer). The caller keeps resample_reach() frames of
 * history before floor(pos) and after the last position; see audio.c for the streaming
 * FIFO, which delays sinc output by RESAMPLE_SINC_DELAY source frames so it asks the source
 * for exactly as many frames as linear interpolation does. */
#ifndef RESAMPLE_H
#define RESAMPLE_H
#include <stdint.h>

typedef enum { RESAMPLE_LINEAR = 0, RESAMPLE_SINC = 1 } ResampleKind;

#define RESAMPLE_SINC_HALF 32     /* sinc taps on each side of the position at step <= 1 */
#define RESAMPLE_MAX_SCALE 4      /* widest kernel stretch for step > 1 (steps beyond alias) */
#define RESAMPLE_MAX_REACH (RESAMPLE_SINC_HALF * RESAMPLE_MAX_SCALE)
#define RESAMPLE_SINC_DELAY (RESAMPLE_SINC_HALF - 1)

/* "linear" / "sinc" -> kind (returns 0), anything else returns -1. */
int resample_parse(const char *name, ResampleKind *kind);
const char *resample_name(ResampleKind kind);

/* Half width of the kernel in source frames: output at position p reads source frames
 * floor(p) - reach + 1 .. floor(p) + reach. Linear: 1. */
int resample_reach(ResampleKind kind, double step);
/* The largest reach over all steps (history a stream must keep). */
int resample_max_reach(ResampleKind kind);

/* Resamples n output frames from interleaved s16 src (channels 1 or 2) into interleaved float
 * out (same channel count). See the timing note above for which frames must be readable. */
void resample_block(ResampleKind kind, const int16_t *src, int channels, double pos, double step, float *out, int n);
#endif
