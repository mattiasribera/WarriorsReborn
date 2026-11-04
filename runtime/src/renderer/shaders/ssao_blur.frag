#version 450
#extension GL_GOOGLE_include_directive : require
// SSAO (--ssao) blur at half the internal size, one direction per pass (pc.dir.xy): 9 taps with
// Gaussian weights, each kept only as far as its view depth is close to the center's (tolerance
// growing with the distance and the depth), so occlusion does not bleed across depth edges. An
// all-zero neighbourhood stays exactly 0. Out: occlusion and the center's view depth.
// LIGHT_MASK (single-sample targets; with MSAA the composite does it per pixel): the last pass
// (vertical, pc.dir.y) fades the occlusion by the brightest light mask in the texel's block of
// SCALE x SCALE internal pixels (at most 4 x 4 of them read), so lights are not darkened.
#define SSAO_NO_DEPTH
#include "ssao_common.glsl"
layout(set = 0, binding = 1) uniform sampler2D ao_tex;
layout(location = 0) out vec4 o;
layout(constant_id = 1) const int SCALE = 2;   // internal pixels per texel

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), mx = ivec2(pc.dir.zw) - 1;
    vec2 c = texelFetch(ao_tex, p, 0).rg;
    float sum = c.r, wsum = 1.0;
    for (int k = 1; k <= 4; k++)
        for (int s = -1; s <= 1; s += 2) {
            vec2 t = texelFetch(ao_tex, clamp(p + ivec2(pc.dir.xy) * (k * s), ivec2(0), mx), 0).rg;
            float tol = (0.03 * c.g + 0.01) * float(k);
            float w = exp(-float(k * k) / 8.0) * max(1.0 - abs(t.g - c.g) / tol, 0.0);
            sum += t.r * w;
            wsum += w;
        }
    float ao = sum / wsum;
#ifdef LIGHT_MASK
    if (pc.dir.y > 0.5 && ao > 0.0) {
        const int step = SCALE >= 8 ? SCALE / 4 : 1;
        ivec2 base = p * SCALE, imx = ivec2(pc.size.xy) - 1;
        float m = 0.0;
        for (int j = 0; j < SCALE; j += step)
            for (int i = 0; i < SCALE; i += step) m = max(m, texelFetch(light_tex, min(base + ivec2(i, j), imx), 0).a);
        ao *= light_keep(m);
    }
#endif
    o = vec4(ao, c.g, 0.0, 0.0);
}
