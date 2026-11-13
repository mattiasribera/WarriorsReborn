/* AMD FidelityFX Super Resolution 1 (--fsr MODE, --fsr-sharpness F): the pure part.
 *
 * With a mode other than off the 3D scene renders at the output size divided by the mode's scale
 * (rounded to the nearest pixel per axis, at least 1), EASU upscales that frame to the output size
 * and RCAS sharpens the result (shaders/fsr_easu.frag, shaders/fsr_rcas.frag, ports of AMD's
 * ffx_fsr1.h 32-bit path). This file holds the option parsing, the scale and size rules, the
 * conflict rules with the other output options and the shader constants (FsrEasuCon, FsrRcasCon),
 * so they can be tested without a GPU. */
#ifndef FSR_H
#define FSR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AMD's standard FSR 1 presets: ultra quality 1.3x, quality 1.5x, balanced 1.7x, performance 2x
 * per axis. */
typedef enum { FSR_OFF, FSR_ULTRA, FSR_QUALITY, FSR_BALANCED, FSR_PERFORMANCE } FsrMode;

/* RCAS sharpness in stops (0 = strongest; each stop halves it). 0.2 is AMD's sample default. */
#define FSR_SHARPNESS_DEFAULT 0.2f
#define FSR_SHARPNESS_MAX 2.0f

typedef struct {
    FsrMode mode;
    float sharpness;
} FsrOptions;

/* The command line's --fsr / --fsr-sharpness (main.c), read by hle_d3d.c at renderer start. */
extern FsrOptions fsr_options;

/* "off", "ultra", "quality", "balanced", "performance" (exact, lower case). Returns 0 and sets
 * *out, or -1 (and leaves *out) for anything else. */
int fsr_parse_mode(const char *s, FsrMode *out);
/* A number in 0..FSR_SHARPNESS_MAX with nothing after it. Returns 0 and sets *out, or -1. */
int fsr_parse_sharpness(const char *s, float *out);
/* The mode's name as fsr_parse_mode takes it ("off" for an unknown value). */
const char *fsr_mode_name(FsrMode m);
/* The mode's scale per axis (1.0 for off). */
float fsr_mode_scale(FsrMode m);
/* Internal render size for an out_w x out_h output: each axis divided by the mode's scale,
 * rounded to the nearest integer (halves up), at least 1. Off: the output size. */
void fsr_render_size(FsrMode m, int out_w, int out_h, int *rw, int *rh);

/* Conflict rules with a mode other than off: FSR replaces supersampling (the scene renders below
 * the output size, not above it) and RCAS replaces CAS. Clears *supersample (> 1) and *sharpen
 * (> 0) and returns which of them were dropped (FSR_DROPPED_* bits) so the caller can warn; off
 * changes nothing and returns 0. */
enum { FSR_DROPPED_SUPERSAMPLE = 1, FSR_DROPPED_SHARPEN = 2 };
int fsr_resolve(FsrMode m, int *supersample, int *sharpen);

/* FsrEasuCon: the 4 x uvec4 EASU constants (float bits) for an in_w x in_h image filling its
 * resource, upscaled to out_w x out_h. */
void fsr_easu_con(uint32_t con[16], int in_w, int in_h, int out_w, int out_h);
/* FsrRcasCon: con[0] = the float bits of 2^-sharpness (sharpness clamped to 0..FSR_SHARPNESS_MAX),
 * con[1] the packed half pair (unused by the 32-bit shader), con[2..3] = 0. */
void fsr_rcas_con(uint32_t con[4], float sharpness);

#ifdef __cplusplus
}
#endif

#endif
