// --ssgi: shared by ssgi_prefilter.comp, ssgi_main.comp and ssgi_denoise.comp.
// One diffuse bounce at the --ssao working size (ssao.h ssao_work_size: working texel (i, j) stands
// for internal pixel (SCALE i + SCALE / 2 - 1, ...)), on XeGTAO's view-depth MIPs (gtao_prefilter.comp)
// and with its view-space conventions (gtao_common.glsl: x right, y down on screen, z = view depth
// forward). Constants from gtao_constants (ssao.h); the rest from vk_renderer.cpp ssgi_run.
layout(push_constant) uniform PC {
    vec4 depth;   // -, -, -, depth MIP levels
    vec4 ndc;     // NDCToViewMul.xy, NDCToViewAdd.xy ([0, 1] screen, y down -> view x, y)
    vec4 scr;     // working texel -> [0, 1] screen: mul.xy, add.xy (gtao_constants screen_mul / add)
    vec4 size;    // working width, height, internal width, height
    vec4 rad;     // reach (view units), thickness (view units), largest reach (working texels), noise index
    vec4 gi;      // emissive gain, bounce share of lit surfaces, energy cap, colour MIP levels
    vec4 den;     // denoise step (texels), 1 = lamp light bound (prefilter) / the last pass (denoise), relative depth tolerance, surface bias
    vec4 rcv;     // the last denoise pass: albedo, darkest local brightness, -, -
} pc;
const float SSGI_FAR = 65000.0;   // the view depth of the far plane (gtao_common.glsl GTAO_FAR)
const float SSGI_PI = 3.1415926535897932;
const float SSGI_PI_HALF = 1.5707963267948966;

// The view position of working texel t (may lie outside the image) at view depth z
// (XeGTAO_ComputeViewspacePosition, as gtao_common.glsl gtao_view_pos).
vec3 ssgi_view_pos(vec2 t, float z) {
    vec2 s = t * pc.scr.xy + pc.scr.zw;
    return vec3((pc.ndc.xy * s + pc.ndc.zw) * z, z);
}
