// Shared by the --ssao passes (ssao_ao.frag, ssao_blur.frag, ssao_comp.frag): push constants and
// the depth target (unless SSAO_NO_DEPTH), read as a single-sample or (MSAA defined)
// multisampled texture, sample 0.
layout(push_constant) uniform PC {
    vec4 proj;     // depth_a, depth_b, 1 / px, 1 / py (renderer.h RSsao)
    vec4 size;     // internal width, height, 1 / width, 1 / height
    vec4 params;   // radius (view units), strength, bias, largest sampling radius (pixels)
    vec4 dir;      // blur step (half-size texels), half width, half height
    vec4 cam[4];   // rain splashes only (ssao_comp.frag mode 1, see there)
} pc;
#ifndef SSAO_NO_DEPTH
#ifdef MSAA
layout(set = 0, binding = 0) uniform sampler2DMS depth_tex;
#else
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
#endif
float depth_at(ivec2 p) {
    return texelFetch(depth_tex, clamp(p, ivec2(0), ivec2(pc.size.xy) - 1), 0).r;
}
#endif
// View depth of an NDC depth (clip z = a w - b).
float view_depth(float z) { return pc.proj.y / max(pc.proj.x - z, 1e-6); }
#ifndef SSAO_NO_DEPTH
// ---- Surfaces from the depth (rain splashes and wet ground: ssao_comp.frag mode 1, wet.frag) ----
// Right-handed view position (x right, y up, the camera looks down -z) of internal pixel p.
vec3 view_pos(ivec2 p) {
    ivec2 q = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float d = view_depth(depth_at(q));
    vec2 ndc = vec2((float(q.x) + 0.5) * pc.size.z * 2.0 - 1.0, 1.0 - (float(q.y) + 0.5) * pc.size.w * 2.0);
    return vec3(ndc.x * d * pc.proj.z, ndc.y * d * pc.proj.w, -d);
}
// The unit normal at pixel p (view position P) from the depths two pixels away (as ssao_ao.frag),
// on each axis the side with the smaller depth step (not across an edge), turned towards the
// camera; vec3(0) where it cannot be told.
vec3 surface_normal(ivec2 p, vec3 P) {
    ivec2 mx = ivec2(pc.size.xy) - 1;
    vec3 r = view_pos(p + ivec2(2, 0)), l = view_pos(p - ivec2(2, 0));
    vec3 u = view_pos(p - ivec2(0, 2)), dn = view_pos(p + ivec2(0, 2));
    bool use_r = p.x + 2 <= mx.x && (p.x < 2 || abs(r.z - P.z) < abs(P.z - l.z));
    bool use_d = p.y + 2 <= mx.y && (p.y < 2 || abs(dn.z - P.z) < abs(P.z - u.z));
    vec3 n = cross(use_r ? r - P : P - l, use_d ? dn - P : P - u);
    if (dot(n, n) < 1e-20) return vec3(0.0);
    n = normalize(n);
    return dot(n, P) > 0.0 ? -n : n;
}
// How far the surface k pixels to the right, left, below and above p lies from the plane through
// P with normal n (view units, absolute): none on flat ground, more the more curved or stepped.
vec4 surface_offsets(ivec2 p, vec3 P, vec3 n, int k) {
    return abs(vec4(dot(view_pos(p + ivec2(k, 0)) - P, n), dot(view_pos(p - ivec2(k, 0)) - P, n),
                    dot(view_pos(p + ivec2(0, k)) - P, n), dot(view_pos(p - ivec2(0, k)) - P, n)));
}
// The tolerance of those offsets at k pixels of world size px each: 1 flat, 0 curved or stepped.
float surface_flat(float off, float px, int k) {
    float tol = 0.006 + 0.15 * px * float(k);
    return 1.0 - smoothstep(tol, 2.0 * tol, off);
}
#endif
