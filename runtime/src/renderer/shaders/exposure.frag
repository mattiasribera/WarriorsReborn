#version 450
#extension GL_GOOGLE_include_directive : require
// --auto-exposure without --filmic: the minimal exposure-only pass at the filmic anchor, over the
// copy of the resolved scene. Linear value x gain (the adaptation state); a brightened frame rolls
// off from the knee toward 1 instead of clipping (tmax = gain); then the dither. Alpha (the bloom
// light mask) kept.
layout(set = 0, binding = 0) uniform sampler2D scene;
#include "ae_common.glsl"
layout(push_constant) uniform PC {
    vec4 a;   // knee, dither, frame seed, 0
} pc;
layout(location = 0) out vec4 o;

void main() {
    vec4 s = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    float g = ae_gain();
    vec3 t = ae_rolloff(pow(clamp(s.rgb, 0.0, 1.0), vec3(2.2)) * g, g, pc.a.x);
    vec3 c = pow(clamp(t, 0.0, 1.0), vec3(1.0 / 2.2));
    if (pc.a.y > 0.0) c += ae_dither(gl_FragCoord.xy, pc.a.z);
    o = vec4(clamp(c, 0.0, 1.0), s.a);
}
