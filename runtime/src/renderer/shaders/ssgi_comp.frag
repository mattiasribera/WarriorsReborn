#version 450
#extension GL_GOOGLE_include_directive : require
// --ssgi composite over the depth of the opaque world at the internal size, in
// the ssao composite pass after the lamp light and before the occlusion (which then darkens the
// bounce too): the four nearest working-size texels of the factor (ssgi_denoise.comp's last pass:
// light x albedo / the local lit brightness; SCALE internal pixels per texel), bilinear weights
// times how close each texel's view depth is to this pixel's (taps from another surface are left
// out; without any close tap, the texel of the closest depth among the 4 x 4 around it), times
// pc.params.y (--ssgi-strength), capped at pc.params.z, drawn with blend DST_COLOR / ONE
// (FS_LIGHTEN): dst x (1 + factor), rgb only, so the surface's own colour and texture tint the
// bounce it receives. With LIGHT_MASK (MSAA) the light also fades by the pixel's light mask (light_keep).
// The far plane and unlit pixels are discarded: they keep their exact colour.
#include "ssao_common.glsl"
layout(constant_id = 1) const int SCALE = 2;
layout(set = 0, binding = 1) uniform sampler2D gi_tex;   // ssgi_denoise.comp: light, view depth
layout(location = 0) out vec4 o;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float z = depth_at(p);
    if (z >= 1.0) discard;
    float d = view_depth(z);
    vec2 h = (vec2(p) - float(SCALE / 2 - 1)) / float(SCALE);   // texel i holds pixel SCALE i + SCALE / 2 - 1
    ivec2 b = ivec2(floor(h)), mx = ivec2(pc.dir.zw) - 1;
    vec2 fr = h - vec2(b);
    float tol = 0.03 * d + 0.01, wsum = 0.0, best = 1e30;
    vec3 sum = vec3(0.0), best_l = vec3(0.0);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec4 t = texelFetch(gi_tex, clamp(b + ivec2(i, j), ivec2(0), mx), 0);
            float dd = abs(t.a - d);
            float w = (i == 1 ? fr.x : 1.0 - fr.x) * (j == 1 ? fr.y : 1.0 - fr.y) + 1e-3;
            w *= max(1.0 - dd / tol, 0.0);
            sum += t.rgb * w;
            wsum += w;
            if (dd < best) {
                best = dd;
                best_l = t.rgb;
            }
        }
    if (wsum <= 1e-4)   // no tap on this surface: the closest depth in the ring around the 2 x 2
        for (int j = -1; j <= 2; j++)
            for (int i = -1; i <= 2; i++) {
                if (i >= 0 && i <= 1 && j >= 0 && j <= 1) continue;
                vec4 t = texelFetch(gi_tex, clamp(b + ivec2(i, j), ivec2(0), mx), 0);
                float dd = abs(t.a - d);
                if (dd < best) {
                    best = dd;
                    best_l = t.rgb;
                }
            }
    vec3 l = (wsum > 1e-4 ? sum / wsum : best_l) * pc.params.y;
#ifdef LIGHT_MASK
    l *= light_keep(texelFetch(light_tex, p, 0).a);
#endif
    l = clamp(l, vec3(0.0), vec3(pc.params.z));   // the energy cap after the strength too
    if (!(max(l.r, max(l.g, l.b)) >= 1.0 / 1024.0) || any(isnan(l))) discard;   // nothing to add (or not a number)
    o = vec4(l, 1.0);
}
