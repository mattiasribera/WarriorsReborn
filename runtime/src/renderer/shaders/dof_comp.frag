#version 450
#extension GL_GOOGLE_include_directive : require
// --cutscene-dof composite, internal size: the pixel's own CoC from the depth (none on a character), the gathered colour
// upsampled from the 2x2 nearest half-size texels (bilinear weights times how close each texel's
// prefilter CoC is to the pixel's: no colour from across a depth edge) and blended over the sharp
// pixel by the larger of its own blur (none under 0.5 pixel, full from 2) and the near field's
// coverage there, times the frame's weight (the fade with the cutscene bars). Alpha (the bloom
// light mask) kept.
#include "dof_common.glsl"
layout(location = 0) out vec4 o;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), mx = ivec2(pc.half_.xy) - 1;
    vec4 s = texelFetch(scene, p, 0);
    float c = pixel_coc(p, focus_range());
    vec2 hpos = (vec2(p) + 0.5) * 0.5 - 0.5;
    ivec2 h0 = ivec2(floor(hpos));
    vec2 fr = hpos - vec2(h0);
    vec3 col = vec3(0.0);
    float ws = 0.0, cov = 0.0;
    for (int i = 0; i < 4; i++) {
        ivec2 o4 = ivec2(i & 1, i >> 1);
        ivec2 q = clamp(h0 + o4, ivec2(0), mx);
        float bw = (o4.x == 1 ? fr.x : 1.0 - fr.x) * (o4.y == 1 ? fr.y : 1.0 - fr.y);
        vec4 b = texelFetch(bokeh, q, 0);
        float wd = bw / (1.0 + abs(texelFetch(pre, q, 0).a - 0.5 * c));
        col += b.rgb * wd;
        ws += wd;
        cov += b.a * bw;
    }
    col = ws > 1e-5 ? col / ws : s.rgb;
    float blend = max(smoothstep(0.5, 2.0, abs(c)), cov) * pc.proj.z;
    o = vec4(mix(s.rgb, col, blend), s.a);
}
