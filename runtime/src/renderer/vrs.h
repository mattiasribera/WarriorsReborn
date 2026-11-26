/* Variable rate shading (--vrs), implemented in vk_renderer.cpp.
 *
 * Kept out of renderer.h on purpose: that header is the renderer's wire API (the render64 recorder
 * and its generator read it). This one is the option and its test hooks only.
 *
 * With VK_KHR_fragment_shading_rate (attachment rates) the frame's main pass gets a shading-rate
 * image (one 8-bit rate per shadingRateTexelSize tile, 16x16 pixels on NVIDIA) built by a compute
 * pass at the end of every frame from that frame's colour (shaders/vrs_rate.comp): flat, fogged
 * or blurred tiles of the next frame's world draws are shaded at 2x1, 1x2, 2x2 (performance: up
 * to 4x4). Characters (RENDERER_CHARACTER, character-lit draws), alpha-tested draws (fences,
 * foliage), emissive draws (neon), draws without depth test (HUD, text, menus) and everything
 * after the TAA op keep 1x1: their per-draw combiner keeps the pipeline's 1x1. With temporal
 * upscaling (--taa-upscale, --dynres) the coarsest rate is 2x2. Off: nothing is created and every
 * frame is exactly as without it. */
#ifndef RENDERER_VRS_H
#define RENDERER_VRS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RENDERER_VRS_OFF, RENDERER_VRS_AUTO, RENDERER_VRS_QUALITY, RENDERER_VRS_PERFORMANCE };

/* Before renderer_init (kept across renderer_shutdown): RENDERER_VRS_*. AUTO is QUALITY where the
 * device supports attachment rates and is not an Intel GPU, else off. */
void renderer_vrs_configure(int mode);
/* "off", "auto", "quality", "performance" -> *mode; 0 on success, -1 otherwise. */
int renderer_vrs_parse(const char *s, int *mode);
const char *renderer_vrs_name(int mode);
/* In use: RENDERER_VRS_QUALITY or RENDERER_VRS_PERFORMANCE, else RENDERER_VRS_OFF. */
int renderer_vrs_mode(void);
/* One line on what is in use or why not (for the log). */
const char *renderer_vrs_info(void);
/* Tests: the rates the last completed frame computed for the next one (tile i at out[i], row
 * major), *tw x *th tiles of *texel pixels. Returns the tile count copied (at most cap), 0 when
 * off. Valid after renderer_end (synchronous). */
int renderer_vrs_rates(uint8_t *out, int cap, int *tw, int *th, int *texel);
/* Tests: draws of the last frame that took the image's rate / kept 1x1 under VRS. */
void renderer_vrs_draws(uint32_t *coarse, uint32_t *full);

#ifdef __cplusplus
}
#endif

#endif
