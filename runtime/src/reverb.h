/* I3DL2-style room reverb for the host mixer (src/audio.c): the console's listener reverb
 * (IXACTEngine_SetI3DL2Listener, run on the Xbox by the DSP effects image) rebuilt on the host.
 *
 * Signal path (mono send in, stereo wet out, 48 kHz):
 *   send -> room HF shelf (lRoomHF at flHFReference) -> delay line
 *     early reflections: 8 taps per ear spread from flReflectionsDelay over flReverbDelay,
 *       level lRoom + lReflections;
 *     late reverb: from flReflectionsDelay + flReverbDelay, two input allpass diffusers
 *       (flDiffusion) into an 8-line feedback delay network (Householder matrix, line lengths
 *       scaled by flDensity), level lRoom + lReverb. Every line has a gain and a one-pole
 *       absorption filter so the decay is flDecayTime at low frequencies and
 *       flDecayTime * flDecayHFRatio at flHFReference (ratios above 1 are clamped to 1).
 * flRoomRolloffFactor is the distance rolloff of the per-voice send (applied by the caller).
 * The default parameters (reverb_i3dl2_default, the XDK's DSI3DL2_ENVIRONMENT_PRESET_DEFAULT)
 * have lRoom -10000 mB: no reverb until the title sets a room. */
#ifndef REVERB_H
#define REVERB_H
#include <stdint.h>

#define REVERB_RATE 48000
#define REVERB_LINES 8
#define REVERB_ER_TAPS 8
#define REVERB_IN_LEN 32768     /* input delay line (power of 2): 0.68 s, the largest pre-delay */
#define REVERB_LINE_MAX 4096    /* longest FDN line at density 100 */
#define REVERB_AP_MAX 1024

/* DSI3DL2LISTENER, field for field (the title's struct is 12 dwords in this order). */
typedef struct {
    int32_t room, room_hf;        /* mB, -10000..0 */
    float room_rolloff;           /* 0..10 */
    float decay_time;             /* s, 0.1..20 */
    float decay_hf_ratio;         /* 0.1..2 */
    int32_t reflections;          /* mB, -10000..1000, relative to room */
    float reflections_delay;      /* s, 0..0.3 */
    int32_t reverb;               /* mB, -10000..2000, relative to room */
    float reverb_delay;           /* s, 0..0.1 */
    float diffusion, density;     /* percent, 0..100 */
    float hf_reference;           /* Hz, 20..20000 */
} ReverbI3dl2;

typedef struct {
    ReverbI3dl2 p;
    int active;                   /* lRoom above -10000 mB */
    /* derived */
    float room_b0, room_a1;       /* room HF one-pole: y = b0 x + a1 y1 */
    float room_y1;
    int er_tap[2][REVERB_ER_TAPS];
    float er_gain[2][REVERB_ER_TAPS];
    int late_delay;
    int ap_len[2];
    float ap_g;
    int line_len[REVERB_LINES];
    float line_b0[REVERB_LINES], line_a1[REVERB_LINES], line_y1[REVERB_LINES];
    float late_gain;
    /* state */
    uint32_t in_pos;
    float in[REVERB_IN_LEN];
    int ap_pos[2];
    float ap[2][REVERB_AP_MAX];
    int line_pos[REVERB_LINES];
    float line[REVERB_LINES][REVERB_LINE_MAX];
} Reverb;

/* mB (hundredths of a dB) to a linear gain; -10000 and below give 0. */
float reverb_mb_to_gain(int32_t mb);
void reverb_i3dl2_default(ReverbI3dl2 *p);
void reverb_init(Reverb *r);                                 /* silent, default parameters */
/* Applies new parameters (clamped to the I3DL2 ranges); the delay lines keep their contents. */
void reverb_set_i3dl2(Reverb *r, const ReverbI3dl2 *p);
int reverb_active(const Reverb *r);
/* Adds the wet signal of n mono send samples to acc (interleaved stereo). */
void reverb_process(Reverb *r, const float *send, float *acc, int n);
#endif
