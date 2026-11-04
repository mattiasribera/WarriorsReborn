/* CPU stand-in for the title's YUV movie quad: YUV -> RGB
 * coefficients read from the title's own pixel shader definition (Bink's register-combiner
 * program), and a nearest-neighbour textured quad into a 32-bit XRGB render target. Kept free of
 * HLE state so a real renderer can replace yuv_draw_quad without touching the decode. */
#ifndef YUV_H
#define YUV_H
#include <stdint.h>
#include "xtex.h"   /* xbox_swizzle_offset, xbox_unswizzle */

/* rgb = M * (plane - off), clamped to [0, 1]. Planes are in shader stage order: plane 0 is the
 * texture on stage 0 (luma), planes 1 and 2 are the textures on stages 1 and 2. */
typedef struct {
    float off[3];       /* subtracted from each plane (0..1 units) */
    float m[3][3];      /* rows R, G, B; columns plane 0, 1, 2 */
} YuvCoeffs;

/* Size of D3DPIXELSHADERDEF (Xbox) in dwords. */
#define PSDEF_DWORDS 60
/* Reads the conversion from an Xbox D3DPIXELSHADERDEF when it is the Bink YUV program (seven
 * combiner stages: assemble Y/plane1/plane2, subtract the offsets, two scaled dot products for R
 * and B, an expanded dot product for G, recombine). Returns 0 when the definition does not have
 * that program, so other shaders are never treated as YUV. */
int yuv_coeffs_from_psdef(const uint32_t *def, YuvCoeffs *c);
/* ITU-R BT.601 video range, plane 1 = Cr, plane 2 = Cb (Bink's documented conversion). */
void yuv_coeffs_bt601(YuvCoeffs *c);
uint32_t yuv_to_xrgb(const YuvCoeffs *c, uint8_t p0, uint8_t p1, uint8_t p2);
/* The same with filtered plane values (0..255 units). */
uint32_t yuv_to_xrgb_f(const YuvCoeffs *c, float p0, float p1, float p2);

/* One 8-bit plane in linear layout (unswizzle first) with the texture coordinates of the quad's
 * left/top and right/bottom edges in texels (Xbox linear textures use unnormalized
 * coordinates; texel i is centred on i). */
typedef struct {
    const uint8_t *p;
    uint32_t w, h, pitch;
    float u0, v0, u1, v1;
} YuvPlane;
/* Fills dst pixels [x0, x1) x [y0, y1) (clipped to dw x dh) with the converted planes, sampling
 * each plane at the pixel centre's interpolated texture coordinate (nearest texel, clamped). */
void yuv_draw_quad(uint32_t *dst, uint32_t dst_pitch, int dw, int dh, int x0, int y0, int x1, int y1,
                   const YuvPlane planes[3], const YuvCoeffs *c);
/* The same with bilinear filtering of every plane (the two nearest texels per axis, clamped at the
 * plane edges), so the half-size chroma planes are interpolated instead of repeated (--movie-hd
 * without a renderer). */
void yuv_draw_quad_bilinear(uint32_t *dst, uint32_t dst_pitch, int dw, int dh, int x0, int y0, int x1, int y1,
                            const YuvPlane planes[3], const YuvCoeffs *c);

/* The largest rectangle with display aspect `aspect` (width / height as seen) centred in
 * in = {x0, y0, x1, y1}, in guest pixels whose display aspect is par (1 for square pixels; 4/3 /
 * (640/480) = 1 at 4:3, (16/9) / (640/480) = 4/3 at --widescreen). out may equal in. */
void yuv_fit_rect(const float in[4], float aspect, float par, float out[4]);

/* A texture coordinate of the title's movie quad for the GPU. The title's edge coordinates are
 * lo and hi (texel i centred on i, so -0.5 and size - 0.5 span a whole plane); the renderer
 * scales linear coordinates by 1 / size, where texel i is centred on i + 0.5. The result moves the
 * coordinate onto that convention and pulls the edges in by half a texel (lo -> lo + 1, hi -> hi),
 * so bilinear taps at the quad's border stay inside the plane (the renderer's sampler wraps). */
float yuv_gpu_coord(float u, float lo, float hi);
#endif
