#version 450
#extension GL_GOOGLE_include_directive : require
// SSAO (--ssao), first pass at half the internal size: per texel, the view position of the
// top-left pixel of its 2x2 block rebuilt from the depth, its normal from the neighbouring depths
// two pixels away (on each axis the side with the smaller depth step, so a silhouette does not
// bend the normal), and RENDERER_SSAO_SAMPLES (12) taps on a disc of the radius projected at that
// depth (golden-angle spiral, rotated per texel by interleaved gradient noise; the blur passes
// average the rotation out). A tap occludes by the cosine between the normal and the direction
// to it, less a bias (flat or slightly curved surfaces do not occlude themselves), fading to 0 at
// the radius (a nearer object farther than the radius away, like a character in front of a far
// wall, casts no halo). Out: occlusion (0-1) and view depth (for the depth-aware blur).
#include "ssao_common.glsl"
layout(location = 0) out vec4 o;
const int N = 12;

vec3 view_at(ivec2 p, out bool far) {
    ivec2 q = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float z = depth_at(q);
    far = z >= 1.0;
    float d = view_depth(z);
    vec2 ndc = vec2((float(q.x) + 0.5) * pc.size.z * 2.0 - 1.0, 1.0 - (float(q.y) + 0.5) * pc.size.w * 2.0);
    return vec3(ndc.x * d * pc.proj.z, ndc.y * d * pc.proj.w, d);
}

void main() {
    ivec2 hp = ivec2(gl_FragCoord.xy);
    ivec2 p = hp * 2;
    ivec2 mx = ivec2(pc.size.xy) - 1;
    bool far, f;
    vec3 P = view_at(p, far);
    if (far) {   // nothing drawn: no occlusion, a depth no surface has
        o = vec4(0.0, 65000.0, 0.0, 0.0);
        return;
    }
    vec3 r = view_at(p + ivec2(2, 0), f), l = view_at(p - ivec2(2, 0), f);
    vec3 u = view_at(p - ivec2(0, 2), f), dn = view_at(p + ivec2(0, 2), f);
    bool use_r = p.x + 2 <= mx.x && (p.x < 2 || abs(r.z - P.z) < abs(P.z - l.z));
    bool use_d = p.y + 2 <= mx.y && (p.y < 2 || abs(dn.z - P.z) < abs(P.z - u.z));
    vec3 dx = use_r ? r - P : P - l, dy = use_d ? dn - P : P - u;
    vec3 n = cross(dx, dy);
    n = dot(n, n) > 1e-12 ? normalize(n) : vec3(0.0, 0.0, -1.0);
    if (dot(n, P) > 0.0) n = -n;   // towards the camera (at the origin, looking down +z)
    float R = pc.params.x;
    float rpx = R / (P.z * pc.proj.w) * pc.size.y * 0.5;   // the radius in pixels at this depth
    if (rpx < 1.0) {
        o = vec4(0.0, P.z, 0.0, 0.0);
        return;
    }
    rpx = min(rpx, pc.params.w);
    float rot = 6.2831853 * fract(52.9829189 * fract(dot(vec2(hp), vec2(0.06711056, 0.00583715))));
    float occ = 0.0;
    for (int i = 0; i < N; i++) {
        float t = (float(i) + 0.5) / float(N), a = float(i) * 2.3999632 + rot;
        vec2 off = vec2(cos(a), sin(a)) * (rpx * sqrt(t));
        vec3 S = view_at(p + ivec2(round(off)), f);
        if (f) continue;
        vec3 v = S - P;
        float d2 = dot(v, v);
        occ += max(dot(v, n) * inversesqrt(d2 + 1e-6) - pc.params.z, 0.0) * max(1.0 - d2 / (R * R), 0.0);
    }
    o = vec4(clamp(2.0 * occ / float(N), 0.0, 1.0), P.z, 0.0, 0.0);
}
