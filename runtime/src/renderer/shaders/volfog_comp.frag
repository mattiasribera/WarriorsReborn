#version 450
#extension GL_GOOGLE_include_directive : require
// --volfog (renderer_volfog): the composite at the internal size, drawn with blend ONE /
// SRC_ALPHA (rgb only: alpha, the bloom light mask, unchanged): colour x lerp(1, T,
// VOLFOG_EXTINCTION_MIX) + S, S and T the integral (volfog_integrate.comp) at the pixel's view
// depth (the far plane and beyond: the volume's far end), +-half-step dithered. Texel k of the
// integral holds slice k's far end; nearer than slice 0's end it fades from nothing at the camera.
#include "volfog_common.glsl"
#ifdef MSAA
layout(set = 0, binding = 0) uniform sampler2DMS depth_tex;
#else
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
#endif
layout(set = 0, binding = 8) uniform sampler3D integ;
layout(location = 0) out vec4 o;
const float EXT_MIX = 0.05;   // VOLFOG_EXTINCTION_MIX
// The light the air adds is soft-capped on its brightest channel, the hue
// kept: up to S_KNEE exactly as before, above it K + (S_CAP - K) x (1 - e^(-(S - K) / (S_CAP - K))), so a
// dense, bright glow tends to S_CAP instead of white and the air never veils the frame.
const float S_KNEE = 0.15, S_CAP = 0.3;   // VOLFOG_ADD_KNEE, VOLFOG_ADD_CAP

float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float zb = texelFetch(depth_tex, clamp(p, ivec2(0), ivec2(u.depth.zw) - 1), 0).r;
    float z = zb >= 1.0 ? u.range.y : u.depth.y / max(u.depth.x - zb, 1e-6);
    float s = vf_depth_slice(min(z, u.range.y));
    vec2 uv = (vec2(p) + 0.5) / u.depth.zw;
    vec4 v = texture(integ, vec3(uv, (max(s, 1.0) - 0.5) / u.grid.z));
    if (s < 1.0) v = mix(vec4(0.0, 0.0, 0.0, 1.0), v, s);
    float t = mix(1.0, v.a, EXT_MIX);
    float m = max(v.r, max(v.g, v.b));
    if (m < 1.0 / 1024.0 && t > 0.998) discard;
    vec3 add = v.rgb;
    if (m > S_KNEE) add *= (S_KNEE + (S_CAP - S_KNEE) * (1.0 - exp(-(m - S_KNEE) / (S_CAP - S_KNEE)))) / m;
    o = vec4(add + (ign(vec2(p)) - 0.5) / 255.0, t);
}
