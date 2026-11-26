/* Dynamic resolution (--dynres TARGET_MS|auto, --dynres-min RATIO): the
 * pure controller. With temporal upscaling the 3D scene renders into the top-left rw x rh of the
 * output-size targets and the TAA history is kept at the output size, so the render size can change
 * from one frame to the next without recreating anything: this module picks it.
 *
 * Each completed frame's GPU time (Vulkan timestamps, a frame or two late) is filtered (fast up,
 * slow down). Above the target the render scale (render / output per axis) drops at once to the
 * scale the pixel-proportional model predicts (cost ~ scale^2), aiming a little below the target;
 * below DYNRES_UP_FRACTION of the target for DYNRES_UP_FRAMES frames it rises by at most
 * DYNRES_UP_STEP per change. After a change DYNRES_SETTLE frames are ignored (the timestamps still
 * describe the old size). The scale stays within [scale_min, scale_max]: scale_max is the
 * --taa-upscale mode's (the quality ceiling), scale_min 1 / --dynres-min. */
#ifndef DYNRES_H
#define DYNRES_H

#ifdef __cplusplus
extern "C" {
#endif

/* --dynres auto: 85 % of a 60 fps frame (the game renders 60 frames a second; the rest is headroom
 * for the CPU side, clock changes and spikes). */
#define DYNRES_AUTO_MS 14.0f
/* --dynres auto under --taa-upscale native: 93 % of a 60 fps frame, so the
 * full-size scene is kept as long as the GPU makes the frame. Hysteresis: a drop needs two frames over
 * it (15.5 ms), a rise needs DYNRES_UP_FRAMES under DYNRES_UP_FRACTION of it (12.4 ms) and takes at
 * most DYNRES_UP_STEP: one step back to native costs (1 / 0.96)^2 = 8.5 % more, which lands at
 * 13.5 ms, still under the drop line, so a dropped size does not flap back and forth (the back-off
 * below covers what clocks add). */
#define DYNRES_AUTO_NATIVE_MS 15.5f
/* The floor under native without --dynres-min: ultra's ratio (the quality the native mode replaces). */
#define DYNRES_NATIVE_MIN_RATIO 1.3f
#define DYNRES_TARGET_MIN 4.0f
#define DYNRES_TARGET_MAX 100.0f
/* --dynres-min: the lowest render size as a ratio per axis (2 = FSR 2's performance, 1/4 of the
 * pixels). */
#define DYNRES_MIN_RATIO_DEFAULT 2.0f
#define DYNRES_SETTLE 4
#define DYNRES_UP_FRACTION 0.80f
#define DYNRES_UP_FRAMES 30
#define DYNRES_UP_STEP 0.04f
/* A drop aims at this fraction of the target. */
#define DYNRES_AIM 0.92f
/* A drop keeps at least this fraction of the scale unless the load is over DYNRES_BIG_OVER x the
 * target: small overloads step down instead of jumping (a GPU that is far from busy lowers its
 * clock, which makes the next frames look slower than they are). */
#define DYNRES_DROP_STEP 0.88f
#define DYNRES_BIG_OVER 1.25f
/* A drop within DYNRES_BACKOFF_FRAMES of a rise doubles the frames the next rise waits for (up to
 * DYNRES_BACKOFF_MAX x DYNRES_UP_FRAMES); DYNRES_CALM_FRAMES without such a drop resets it. */
#define DYNRES_BACKOFF_FRAMES 120
#define DYNRES_BACKOFF_MAX 16
#define DYNRES_CALM_FRAMES 1200
/* Changes smaller than this (scale units) are not made, except to reach a bound. */
#define DYNRES_MIN_CHANGE 0.01f

typedef struct {
    int on;
    float target_ms;   /* 0 = auto (DYNRES_AUTO_MS) */
    float min_ratio;   /* per axis, > 1 */
    int min_set;       /* --dynres-min given */
} DynresOptions;
extern DynresOptions dynres_options;

/* "auto" (*ms = 0) or a number of milliseconds DYNRES_TARGET_MIN..DYNRES_TARGET_MAX with nothing
 * after it. Returns 0 and sets *ms, or -1 (and leaves it). */
int dynres_parse_target(const char *s, float *ms);
/* A ratio per axis 1.1..3 with nothing after it. Returns 0 and sets *ratio, or -1. */
int dynres_parse_min(const char *s, float *ratio);
/* The target in ms an option value stands for (0 -> DYNRES_AUTO_MS). */
float dynres_target(float option_ms);
/* The same under --taa-upscale native (native != 0: 0 -> DYNRES_AUTO_NATIVE_MS). */
float dynres_target_for(float option_ms, int native);
/* The lowest render scale per axis: 1 / min_ratio, or under native without --dynres-min (min_set 0)
 * 1 / DYNRES_NATIVE_MIN_RATIO. */
float dynres_floor(float min_ratio, int min_set, int native);

typedef struct {
    float target_ms, scale_min, scale_max;
    float scale;    /* current render scale per axis */
    float ema_ms;   /* filtered GPU time, < 0 before the first sample */
    int settle;     /* samples still ignored after a change */
    int above, below;
    int up_frames;    /* frames below the target a rise waits for (DYNRES_UP_FRAMES, more after flapping) */
    int since_up;     /* samples since the last rise */
    int calm;         /* samples since the last drop that undid a rise */
    unsigned changes;
} Dynres;

/* Starts at scale_max (clamped to (0, 1]; scale_min clamped to (0, scale_max]). */
void dynres_init(Dynres *d, float target_ms, float scale_min, float scale_max);
/* One completed frame's GPU time (ms; <= 0: none, ignored). Returns 1 when d->scale changed. */
int dynres_update(Dynres *d, float gpu_ms);
/* Render size for the scale at an out_w x out_h output, each axis rounded to the nearest integer
 * and clamped to 1..max_w / 1..max_h (the allocated render rectangle). */
void dynres_size(float scale, int out_w, int out_h, int max_w, int max_h, int *rw, int *rh);

#ifdef __cplusplus
}
#endif

#endif
