#version 450
#extension GL_GOOGLE_include_directive : require
// --cutscene-dof prefilter, half size: each texel's 2x2 internal pixels, their signed CoC (the
// depth, the sharp range of this frame's state; none on a character); the CoC kept is the nearest one (the smallest: the CoC
// grows with depth), and the colour averages the four weighted by how close their CoC is to it, so
// a texel on a character's edge stays the character's (no background in its colour). Out: colour,
// CoC in half pixels.
#include "dof_common.glsl"
layout(location = 0) out vec4 o;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) * 2;
    vec2 f = focus_range();
    vec3 c[4];
    float k[4];
    float kn = 1e9;
    for (int i = 0; i < 4; i++) {
        ivec2 q = clamp(p + ivec2(i & 1, i >> 1), ivec2(0), ivec2(pc.size.xy) - 1);
        c[i] = texelFetch(scene, q, 0).rgb;
        k[i] = pixel_coc(q, f);
        kn = min(kn, k[i]);
    }
    vec3 s = vec3(0.0);
    float ws = 0.0;
    for (int i = 0; i < 4; i++) {
        float wi = 1.0 / (1.0 + abs(k[i] - kn));
        s += c[i] * wi;
        ws += wi;
    }
    o = vec4(s / ws, 0.5 * kn);
}
