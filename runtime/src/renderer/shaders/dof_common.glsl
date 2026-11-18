// Shared by the --cutscene-dof passes (dof_focus.frag, dof_down.frag, dof_gather.frag,
// dof_comp.frag; vk_renderer.cpp DofPush, src/dof.h): push constants, the bindings and the
// circle of confusion. The depth target is read single-sample or (MSAA defined) multisampled,
// sample 0. Keep coc_px in step with dof.c dof_coc_range and is_char with dof_char_visible.
layout(push_constant) uniform PC {
    vec4 proj;    // depth_a, depth_b, weight, dt (seconds)
    vec4 size;    // internal width, height, 1 / width, 1 / height
    vec4 coc;     // RDof coc x height (pixels), near_max, focus_ref, focus_min_scale
    vec4 focus;   // focus_min, pct, tau, snap
    vec4 half_;   // half width, half height, the gather's largest radius (half pixels), char_margin
    vec4 spk;     // the speaker's weight (0: none), his view distance, char_blur, char_cap
} pc;
#ifndef DOF_NO_DEPTH
#ifdef MSAA
layout(set = 0, binding = 0) uniform sampler2DMS depth_tex;
#else
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
#endif
float depth_at(ivec2 p) {
    return texelFetch(depth_tex, clamp(p, ivec2(0), ivec2(pc.size.xy) - 1), 0).r;
}
// The character mask (renderer_rain_chars_on, single-sample, internal size): the depth of the
// frame's RENDERER_CHARACTER draws alone, 1 where there are none.
layout(set = 0, binding = 5) uniform sampler2D char_tex;
#endif
layout(set = 0, binding = 1) uniform sampler2D scene;    // the copy of the resolved scene (filmic src)
layout(set = 0, binding = 2) uniform sampler2D pre;      // half size: colour, signed CoC (half pixels)
layout(set = 0, binding = 3) uniform sampler2D bokeh;    // half size: gathered colour, near coverage
layout(set = 0, binding = 4) uniform sampler2D fstate;   // 1x1: log2 focus, log2 meter, metered, weight

// View distance of an NDC depth (NDC depth = a - b / distance); nothing drawn (1): far away.
float view_dist(float z) { return z >= 1.0 ? 1e6 : pc.proj.y / max(pc.proj.x - z, 1e-6); }
// Signed CoC in internal pixels (negative: nearer than the focus f), dof.c dof_coc.
float coc_one(float d, float fs) {
    float k = pc.coc.x * clamp(pc.coc.z / fs, pc.coc.w, 1.0);
    return clamp(k * (1.0 - fs / max(d, 1e-4)), -pc.coc.y * k, k);
}
// With the sharp range r (near, far: the characters' span, or the centre focus at both ends): 0
// inside it, the near field against r.x, the far field against r.y (dof.c dof_coc_range).
float coc_px(float d, vec2 r) {
    float ns = max(r.x, pc.focus.x), fs = max(r.y, ns);
    return d < ns ? coc_one(d, ns) : d > fs ? coc_one(d, fs) : 0.0;
}
// The smoothed sharp range of this frame's state (log2 near, log2 far).
vec2 focus_range() { return exp2(texelFetch(fstate, ivec2(0), 0).xy); }
#ifndef DOF_NO_DEPTH
// Internal pixel p shows a character: its visible depth is the mask's (dof.c dof_char_visible).
bool is_char(ivec2 p) {
    ivec2 q = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float c = texelFetch(char_tex, q, 0).r;
    if (c >= 1.0) return false;
    float dc = view_dist(c), ds = view_dist(depth_at(q));
    return abs(dc - ds) <= 0.02 + 0.01 * ds;
}
// The pixel's CoC: none on a character; with a speaker (pc.spk.x > 0, the range is around him), a
// character gets his own CoC times char_blur, at most char_cap of the far limit, times the speaker's
// weight (dof.c dof_coc_speaker): the speaker sharp, the others soft, never as soft as the background.
float pixel_coc(ivec2 p, vec2 r) {
    float d = view_dist(depth_at(p));
    if (!is_char(p)) return coc_px(d, r);
    if (!(pc.spk.x > 0.0)) return 0.0;
    float ns = max(r.x, pc.focus.x), fs = max(r.y, ns), g = d < ns ? ns : fs;
    float cap = pc.spk.w * pc.coc.x * clamp(pc.coc.z / g, pc.coc.w, 1.0);
    return clamp(coc_px(d, r) * pc.spk.z, -cap, cap) * pc.spk.x;
}
#endif
