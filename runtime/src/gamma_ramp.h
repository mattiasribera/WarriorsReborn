/* --gamma-ramp: the title's D3DDevice_SetGammaRamp ramps, applied to the finished frame.
 *
 * The Xbox applies the ramp on scanout (the video encoder's per-channel lookup), so it touches
 * every pixel the console shows: the 3D scene, the HUD, the menus and the movies. The title sets
 * one every frame (XBE 0xbbbb3 in levels, 0x249d55 in the movie player; the Brightness screen,
 * DlgLight in level100main.lua, ends in it too), and most are not the identity: 0xba800 builds
 * them from a per-level curve (0x459288, 40-byte entries; level81's is 0..256 -> 0..285, a slope
 * of about 1.11) plus an offset that the Brightness setting raises, so dark tones are crushed and
 * highlights lifted (menus: mid-grey 128 -> 118). Without them the picture lacks the console's
 * contrast and the Brightness slider does nothing.
 *
 * The renderer runs the same lookup (shaders/gamma.frag) as the last pass on the finished frame,
 * after every post effect of the port (bloom, filmic look, sharpening) and before the scaling to
 * the window and the port's own frame counter: where the console's scanout sits. An identity ramp
 * skips the pass, so the frame is bit-identical to one without it. */
#ifndef GAMMA_RAMP_H
#define GAMMA_RAMP_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Xbox D3DGAMMARAMP (XDK d3d8types.h; the HLE'd D3DDevice_SetGammaRamp at 0x361110 copies 0xc0
 * dwords): one byte per entry, unlike the PC's WORD ramps. */
#define GAMMA_RAMP_BYTES 768
typedef struct {
    uint8_t red[256], green[256], blue[256];
} GammaRamp;

/* D3DDevice_SetGammaRamp's Flags (stack argument 1, the ramp is argument 2): D3DSGR_CALIBRATE
 * (bit 0) and D3DSGR_IMMEDIATE (bit 1: load now instead of at the next vertical blank; the title
 * always passes 2). Neither changes the ramp: the port has no calibration and applies the latest
 * ramp to the next frame it shows. */
#define GAMMA_RAMP_CALIBRATE 1u
#define GAMMA_RAMP_IMMEDIATE 2u

/* --gamma-ramp (1, the default) / --no-gamma-ramp (0: the title's ramps are ignored, frames as
 * rendered). Set by main.c, read by the HLE'd D3DDevice_SetGammaRamp (src/hle_d3d.c). */
extern int gamma_ramp_enabled;

/* Parses one option: --gamma-ramp (on = 1) or --no-gamma-ramp (on = 0). Returns 1 when consumed,
 * 0 for any other option (on unchanged). */
int gamma_ramp_arg(int *on, const char *opt);

/* Copies a guest D3DGAMMARAMP of size bytes into out. Returns 0, or -1 (out unchanged) when src is
 * NULL or size is less than GAMMA_RAMP_BYTES. */
int gamma_ramp_parse(const void *src, size_t size, GammaRamp *out);

/* 1 when every channel maps each entry i to i (the frame is shown as rendered). */
int gamma_ramp_is_identity(const GammaRamp *g);

/* The lookup table of shaders/gamma.frag: entry i = red[i] | green[i] << 8 | blue[i] << 16. */
void gamma_ramp_lut(const GammaRamp *g, uint32_t lut[256]);

/* The CPU reference of shaders/gamma.frag on one B8G8R8A8 pixel (0xAARRGGBB): each colour
 * channel through the table, alpha kept. */
uint32_t gamma_ramp_apply(const uint32_t lut[256], uint32_t bgra);

/* Renderer side (src/renderer/vk_renderer.cpp). The table to apply to the finished frames from the
 * next one recorded (copied), or NULL: frames as rendered (no pass). The first table creates the
 * pass's resources; a device that cannot shows the frames as rendered (renderer_gamma_ramp_on()
 * stays 0). */
void renderer_gamma_ramp(const uint32_t *lut);
/* 1 while a table is applied to the frames. */
int renderer_gamma_ramp_on(void);
/* Frames the ramp pass ran on since renderer_init. */
uint32_t renderer_gamma_ramp_count(void);

#ifdef __cplusplus
}
#endif
#endif
