/* Xbox D3DPIXELSHADERDEF -> RPixelShader (src/renderer.h) for the renderer's register-combiner
 * shader. Layout of the definition (0xF0 bytes, Xbox d3d8types.h, Cxbx-Reloaded XbPixelShader.h):
 * PSAlphaInputs[8] 0x00, PSFinalCombinerInputsABCD 0x20, PSFinalCombinerInputsEFG 0x24,
 * PSConstant0[8] 0x28, PSConstant1[8] 0x48, PSAlphaOutputs[8] 0x68, PSRGBInputs[8] 0x88,
 * PSCompareMode 0xA8, PSFinalCombinerConstant0 0xAC, PSFinalCombinerConstant1 0xB0,
 * PSRGBOutputs[8] 0xB4, PSCombinerCount 0xD4, PSTextureModes 0xD8, PSDotMapping 0xDC,
 * PSInputTexture 0xE0, PSC0Mapping 0xE4, PSC1Mapping 0xE8, PSFinalCombinerConstants 0xEC. */
#ifndef PSH_H
#define PSH_H
#include "renderer.h"
#include <stdint.h>

#define PSH_DEF_DWORDS 60   /* sizeof(D3DPIXELSHADERDEF) / 4 */
#define PSH_CONSTANTS 16    /* D3D pixel shader constants kept from SetPixelShaderConstant */

/* D3D constant (r, g, b, a floats) -> ARGB8888, each channel clamped to [0, 1]. */
uint32_t psh_argb(const float c[4]);
/* Builds the combiner record of a definition. Stage constants: the D3D constant named by the
 * stage's PSC0Mapping/PSC1Mapping nibble (0xF = none, then PSConstant0/1 of the definition), and
 * with SAME_C0/SAME_C1 (PSCombinerCount bits 12/16 clear) every stage uses stage 0's. Final
 * combiner constants: PSFinalCombinerConstants bits 0-3/4-7 likewise. d3d_const = the current
 * SetPixelShaderConstant values (ARGB8888, PSH_CONSTANTS entries); fog = ARGB fog color with the
 * fog factor in the alpha byte. */
void psh_from_def(const uint32_t def[PSH_DEF_DWORDS], const uint32_t d3d_const[PSH_CONSTANTS], uint32_t fog, RPixelShader *ps);
/* Texture mode of a stage (PSTextureModes, 5 bits per stage). */
uint32_t psh_tex_mode(const RPixelShader *ps, int stage);
#endif
