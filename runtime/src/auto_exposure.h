/* --auto-exposure: the options and the math of the eye adaptation at
 * the filmic anchor. The renderer runs the same adaptation step in shaders/ae_adapt.frag and the
 * same roll-off in shaders/filmic.frag (the AUTO_EXPOSURE variant) and shaders/exposure.frag from
 * the parameters auto_exposure_params computes (RAutoExposure). */
#ifndef AUTO_EXPOSURE_H
#define AUTO_EXPOSURE_H
#include "renderer.h"

#define AUTO_EXPOSURE_STRENGTH_DEFAULT 1.0f
typedef struct {
    int on;          /* --auto-exposure / --no-auto-exposure */
    float strength;  /* --auto-exposure-strength 0-2: 0 = no gain (only the dither) */
} AutoExposureConfig;
#define AUTO_EXPOSURE_DEFAULTS {0, AUTO_EXPOSURE_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not an auto-exposure option, -1 for a missing or out-of-range value (c unchanged). */
int auto_exposure_arg(AutoExposureConfig *c, const char *opt, const char *val);

/* The renderer parameters for strength 0-2 (clamped): the adapt fraction and the gain limits scale
 * with it (strength 1: 0.4 of the deviation, -0.4 to +0.5 stop); the timing is fixed. */
void auto_exposure_params(float strength, RAutoExposure *p);

/* The adaptation state the GPU keeps in its 1x1 image (shaders/ae_adapt.frag: x, y, z, w). */
typedef struct {
    float adapted;  /* log2 linear luma the eye is adapted to */
    float ev;       /* the gain in stops */
    float cut;      /* 1 on a step that snapped (a camera cut, a load), else 0 */
    float meter;    /* the last meter value (log2) */
} AutoExposureState;

/* The meter's range of log2 linear luma (darker or brighter values are clamped). */
#define AUTO_EXPOSURE_LOG_MIN (-13.0f)
#define AUTO_EXPOSURE_LOG_MAX 0.0f

/* One adaptation step of dt seconds toward meter (log2; meter_valid 0: nothing metered, e.g. every
 * pixel light-masked: the level holds). snap != 0 (the first frame, a frame after a gap, a camera
 * cut the caller detected) sets the adapted level to the meter; a meter change alone never snaps,
 * however large (a camera turn in the hideout snapped the gain by
 * 0.9 stop at once). The adapted level is kept where the gain is not at a limit (no wind-up). */
void auto_exposure_step(const RAutoExposure *p, AutoExposureState *s, float meter, int meter_valid, float dt, int snap);

/* The roll-off of a brightened tonemap output t (linear) whose largest value is tmax: identity
 * below the knee and whenever tmax <= 1; above it k + (1 - k) u / (1 + a u), u = (t - k) / (1 - k),
 * with a chosen so tmax maps to 1 (smooth at the knee, monotonic). */
float auto_exposure_rolloff(float t, float tmax, float knee);

/* The largest tonemap output at a linear gain (2^ev): with the filmic tonemap f (NULL: the
 * exposure-only pass) blend x ACES(exposure x white x gain) x norm + (1 - blend) x gain, else gain. */
float auto_exposure_tmax(const RFilmic *f, float gain);
#endif
