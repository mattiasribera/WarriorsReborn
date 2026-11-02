// Shared by the --ssao passes (ssao_ao.frag, ssao_blur.frag, ssao_comp.frag): push constants and
// the depth target (unless SSAO_NO_DEPTH), read as a single-sample or (MSAA defined)
// multisampled texture, sample 0.
layout(push_constant) uniform PC {
    vec4 proj;     // depth_a, depth_b, 1 / px, 1 / py (renderer.h RSsao)
    vec4 size;     // internal width, height, 1 / width, 1 / height
    vec4 params;   // radius (view units), strength, bias, largest sampling radius (pixels)
    vec4 dir;      // blur step (half-size texels), half width, half height
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
