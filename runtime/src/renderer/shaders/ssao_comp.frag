#version 450
#extension GL_GOOGLE_include_directive : require
// SSAO (--ssao) composite at the internal size, multiplied onto the target (blend DST_COLOR / ZERO,
// rgb only: the bloom light mask in alpha is kept): the four nearest half-size texels, bilinear
// weights times how close each texel's view depth is to this pixel's (taps from another surface
// are left out; without any close tap the closest one), then 1 - strength x occlusion. The far
// plane (depth 1) and unoccluded pixels are discarded, so they keep their exact color.
#include "ssao_common.glsl"
layout(set = 0, binding = 1) uniform sampler2D ao_tex;
layout(location = 0) out vec4 o;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float z = depth_at(p);
    if (z >= 1.0) discard;
    float d = view_depth(z);
    vec2 h = vec2(p) * 0.5;   // half-size texel i holds the pixel 2i (its top-left)
    ivec2 b = ivec2(floor(h)), mx = ivec2(pc.dir.zw) - 1;
    vec2 fr = h - vec2(b);
    float tol = 0.03 * d + 0.01, sum = 0.0, wsum = 0.0, best = 1e30, best_ao = 0.0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec2 t = texelFetch(ao_tex, clamp(b + ivec2(i, j), ivec2(0), mx), 0).rg;
            float dd = abs(t.g - d);
            float w = (i == 1 ? fr.x : 1.0 - fr.x) * (j == 1 ? fr.y : 1.0 - fr.y) + 1e-3;
            w *= max(1.0 - dd / tol, 0.0);
            sum += t.r * w;
            wsum += w;
            if (dd < best) {
                best = dd;
                best_ao = t.r;
            }
        }
    float ao = wsum > 1e-4 ? sum / wsum : best_ao;
    if (ao <= 0.0) discard;
    float f = clamp(1.0 - pc.params.y * ao, 0.0, 1.0);
    o = vec4(f, f, f, 1.0);
}
