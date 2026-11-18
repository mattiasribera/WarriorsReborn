/* Temporal anti-aliasing (--taa, --taa-sharpness F): the pure part.
 *
 * With --taa the 3D scene's draws are offset by a sub-pixel jitter that changes every frame (the
 * Halton (2, 3) sequence, 8 positions) and, at the end of the 3D scene, a resolve pass blends the
 * frame into a history image reprojected from the previous frame (renderer_taa,
 * shaders/taa_resolve.frag). The motion of a pixel comes from the camera: its depth gives its view
 * distance (NDC depth = A - B / w, the frame's depth fit), its clip position goes back to the world
 * through the current world-to-clip matrix and forward to the previous frame's clip position through
 * the previous one. Only the x, y and w rows of the matrices are used (a world program's c69-c72 z
 * row is not the depth), so the map is a 3 x 4 matrix over (ndc.x w, ndc.y w, w, 1). This file holds
 * the option parsing, the jitter, that matrix and the camera-cut test, so they can be tested without
 * a GPU. */
#ifndef TAA_H
#define TAA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sharpening of the resolved frame (RCAS, 0 = none, 1 = strongest); the history itself is never
 * sharpened (no feedback). 0.5 brings the upscaled Quality image's fine detail back to about native
 * 1080p's. */
#define TAA_SHARPNESS_DEFAULT 0.5f
#define TAA_SHARPNESS_MAX 1.0f
/* --taa-sharpness left out: the default for temporal upscaling at 2x per axis and below (lower render
 * sizes lose more fine detail), taa_auto_sharpness. */
#define TAA_SHARPNESS_LOW_RES 0.85f
/* Jitter positions before the sequence repeats. */
#define TAA_JITTER_PHASES 8
/* A frame whose camera moves a screen point by more than this (NDC units, 2 = the screen width) at
 * TAA_CUT_DIST view units or at infinity is a cut: the history is dropped. */
#define TAA_CUT_NDC 0.5f
#define TAA_CUT_DIST 4.0f

/* Temporal upscaling (--taa-upscale MODE|RATIO): the 3D
 * scene renders below the output size, FSR 2's ratios per axis (ultra 1.3x, quality 1.5x, balanced
 * 1.7x, performance 2x) or a ratio given as a number (TAA_UP_CUSTOM), and the resolve reconstructs
 * the output size from the jittered frames. Implies --taa. */
typedef enum { TAA_UP_OFF, TAA_UP_QUALITY, TAA_UP_BALANCED, TAA_UP_PERFORMANCE, TAA_UP_ULTRA, TAA_UP_CUSTOM } TaaUpscale;
/* The ratios --taa-upscale takes as a number (per axis). */
#define TAA_UP_SCALE_MIN 1.1f
#define TAA_UP_SCALE_MAX 3.0f

typedef struct {
    int on;
    float sharpness;
    TaaUpscale upscale;
    float upscale_ratio;   /* TAA_UP_CUSTOM's ratio per axis (0 with a named mode) */
    int sharpness_set;     /* --taa-sharpness given (else taa_auto_sharpness of the render ratio) */
} TaaOptions;

/* The command line's --taa / --taa-sharpness (main.c), read by hle_d3d.c at renderer start. */
extern TaaOptions taa_options;

/* A number in 0..TAA_SHARPNESS_MAX with nothing after it. Returns 0 and sets *out, or -1. */
int taa_parse_sharpness(const char *s, float *out);

/* The sharpening for a render ratio k (output / render per axis) when --taa-sharpness is left out:
 * TAA_SHARPNESS_DEFAULT up to ultra's 1.3 (and for plain TAA, k <= 1), rising linearly to
 * TAA_SHARPNESS_LOW_RES at 2 and kept there beyond. */
float taa_auto_sharpness(float k);

/* Halton(index, base) for index >= 1: the radical inverse in (0, 1). */
float taa_halton(uint32_t index, uint32_t base);
/* The jitter of frame n in target pixels, x right and y down, each in (-0.5, 0.5): Halton (2, 3)
 * at index (n % TAA_JITTER_PHASES) + 1, minus 0.5. Its mean over the TAA_JITTER_PHASES phases is
 * close to 0. */
void taa_jitter(uint32_t n, float out[2]);
/* The same with a sequence of `phases` positions (index (n % phases) + 1); taa_jitter is phases =
 * TAA_JITTER_PHASES. phases 0 counts as TAA_JITTER_PHASES. */
void taa_jitter_n(uint32_t n, uint32_t phases, float out[2]);

/* "off", "ultra", "quality", "balanced", "performance" (exact, lower case; *ratio = 0), or a ratio
 * per axis from TAA_UP_SCALE_MIN to TAA_UP_SCALE_MAX with nothing after it (TAA_UP_CUSTOM, *ratio =
 * the number). Returns 0 and sets both, or -1 (and leaves them). */
int taa_parse_upscale(const char *s, TaaUpscale *out, float *ratio);
/* The mode's name as taa_parse_upscale takes it ("custom" for TAA_UP_CUSTOM, "off" for an unknown
 * value). */
const char *taa_upscale_name(TaaUpscale m);
/* The mode's scale per axis (1.0 for off; 0 for TAA_UP_CUSTOM, whose ratio is the option's). */
float taa_upscale_scale(TaaUpscale m);
/* Render size for an out_w x out_h output: each axis divided by the scale, rounded to the nearest
 * integer (halves up), at least 1 (fsr_render_size's rule). Off: the output size. */
void taa_upscale_size(TaaUpscale m, int out_w, int out_h, int *rw, int *rh);
/* The same for a ratio k per axis (k <= 1: the output size). */
void taa_upscale_size_k(float k, int out_w, int out_h, int *rw, int *rh);
/* Jitter positions for a render width rw at a display width dw: FSR 2's 8 x (dw / rw)^2, rounded up,
 * at least TAA_JITTER_PHASES (also for rw >= dw or rw <= 0). */
uint32_t taa_jitter_phases(int rw, int dw);
/* Texture LOD bias for a render width rw at a display width dw: log2(rw / dw), 0 when rw >= dw or
 * rw <= 0 (textures keep the detail of the display size). */
float taa_lod_bias(int rw, int dw);

/* The reprojection map from the current to the previous frame: cur and prev are world-to-clip
 * matrices (rows x, y, z, w over the homogeneous world position, clip = m p; the z row is ignored).
 * out = prev's x, y, w rows times the inverse of cur's x, y, w rows completed with [0 0 0 1], so
 * for a pixel at NDC (x, y) and view distance w, out (x w, y w, w, 1) is the previous clip (x', y',
 * w'), previous NDC (x' / w', y' / w'); a direction (x, y, 1, 0) gives the same for a point at
 * infinity (the sky). Returns 0, or -1 (out unchanged) when cur's x, y, w rows are not independent. */
int taa_reprojection(const float cur[4][4], const float prev[4][4], float out[3][4]);

/* The largest motion (NDC units) of a 3 x 3 grid of screen points (NDC -0.8, 0, 0.8 on each axis)
 * through the map m, at view distance dist and at infinity; a point that lands behind the previous
 * camera (w' <= 0) counts as 1e9. */
float taa_camera_motion(const float m[3][4], float dist);
/* 1 when the camera motion (taa_camera_motion at TAA_CUT_DIST) exceeds TAA_CUT_NDC. */
int taa_camera_cut(const float m[3][4]);

/* Previous NDC of a pixel at NDC (x, y) and NDC depth d with the depth projection a, b (NDC depth =
 * a - b / w): the shader's arithmetic (shaders/taa_resolve.frag) on the host, for the tests. Depth
 * 1 or beyond the projection's range: the point at infinity. Returns 0, or -1 behind the camera. */
int taa_reproject_point(const float m[3][4], float a, float b, float x, float y, float d, float out[2]);

#ifdef __cplusplus
}
#endif

#endif
