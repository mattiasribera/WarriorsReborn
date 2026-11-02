#version 450
#extension GL_GOOGLE_include_directive : require
// SSAO (--ssao) blur at half the internal size, one direction per pass (pc.dir.xy): 9 taps with
// Gaussian weights, each kept only as far as its view depth is close to the center's (tolerance
// growing with the distance and the depth), so occlusion does not bleed across depth edges. An
// all-zero neighbourhood stays exactly 0. Out: occlusion and the center's view depth.
#define SSAO_NO_DEPTH
#include "ssao_common.glsl"
layout(set = 0, binding = 1) uniform sampler2D ao_tex;
layout(location = 0) out vec4 o;

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
    o = vec4(sum / wsum, c.g, 0.0, 0.0);
}
