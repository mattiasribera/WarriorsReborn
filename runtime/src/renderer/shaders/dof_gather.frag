#version 450
#extension GL_GOOGLE_include_directive : require
// --cutscene-dof gather, half size, over the prefilter (colour, signed CoC c in half pixels): a
// disc of 32 taps (golden-angle spiral) out to R, the larger of the texel's own |c| and the
// nearest near-field CoC an 8-tap probe finds around it (at most the frame's largest radius). A tap
// at distance r covers the texel when its CoC disc reaches it (soft over one texel); a tap behind
// the texel (larger c) reaches at most the texel's own |c|, so a sharp character gets no halo of
// the blurred background, while a tap in front reaches its own (a blurred near shoulder spreads
// over what is behind it). Covered taps replace their share of the running average (the texel's
// own colour where nothing reaches). Out: colour, the near field's coverage (the share of the disc
// that nearer, near-field taps cover x2, at most 1) for the composite.
#define DOF_NO_DEPTH
#include "dof_common.glsl"
layout(location = 0) out vec4 o;
const int N = 32;

void main() {
    ivec2 hp = ivec2(gl_FragCoord.xy), mx = ivec2(pc.half_.xy) - 1;
    vec4 c0 = texelFetch(pre, hp, 0);
    float cc = c0.a, rc = abs(cc), Rmax = pc.half_.z;
    float rn = 0.0;   // the nearest near-field CoC around (probe)
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.7853982 + 0.3926991;
        vec2 off = vec2(cos(a), sin(a)) * (i % 2 == 0 ? Rmax : 0.5 * Rmax);
        float ct = texelFetch(pre, clamp(hp + ivec2(round(off)), ivec2(0), mx), 0).a;
        if (ct < min(cc, 0.0)) rn = max(rn, -ct);
    }
    float R = min(max(rc, rn), Rmax);
    if (R < 0.5) {
        o = vec4(c0.rgb, 0.0);
        return;
    }
    vec3 acc = c0.rgb;
    float tot = 1.0, near = 0.0;
    for (int i = 0; i < N; i++) {
        float r = R * sqrt((float(i) + 0.5) / float(N)), a = float(i) * 2.3999632;
        vec4 s = texelFetch(pre, clamp(hp + ivec2(round(vec2(cos(a), sin(a)) * r)), ivec2(0), mx), 0);
        float rt = abs(s.a);
        if (s.a > cc) rt = min(rt, rc);
        float m = clamp(rt - r + 0.5, 0.0, 1.0);
        acc += mix(acc / tot, s.rgb, m);
        tot += 1.0;
        if (s.a < 0.0 && s.a < cc - 0.5) near += m;
    }
    o = vec4(acc / tot, min(1.0, 2.0 * near / float(N)));
}
