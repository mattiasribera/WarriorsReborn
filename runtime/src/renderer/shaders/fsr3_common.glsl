// --upscaler fsr3: shared part of the FSR 3.1 upscaler passes (fsr3_*.comp).
// A GLSL / Vulkan 1.0 port of AMD FidelityFX FSR 3.1's upscaler (FidelityFX SDK, upscalers/fsr3,
// FFX_FSR3UPSCALER 3.1.6: ffx_fsr3upscaler_common.h, _sample.h and the pass headers named in each
// shader), FP32 only, fed by this renderer's inputs:
//   colour     s.color's top-left render-size rectangle (jittered, display-referred LDR: exposure 1)
//   depth      s.depth (same rectangle; 1 = far, view distance w = b / (a - z), metres)
//   motion     computed here from the camera reprojection rows (taa_reprojection) and, where a
//              skinned character is, the velocity target (taa_vel_record): FSR's convention, the
//              UV offset from this frame's unjittered position to the previous frame's
//   jitter     taa_jitter_n: FSR's Halton (2, 3) sequence, 8 x ratio^2 phases, in render pixels
// Resources live in VK_IMAGE_LAYOUT_GENERAL; "cur" / "prev" are the two halves of each ring.
// ---------------------------------------------------------------------------------------------
// Derived from AMD FidelityFX FSR 3.1 (FidelityFX SDK, https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK).
//
// Copyright (C) 2024-2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// ---------------------------------------------------------------------------------------------

layout(push_constant) uniform Push {
    vec4 rx, ry, rw;   // the reprojection map's rows (current clip (ndc w, w, 1) to previous clip x, y, w)
    vec4 vp;           // the 3D viewport in output pixels: x, y, 2 / width, 2 / height
    vec4 size;         // render width, height; output width, height
    vec4 jit;          // this frame's jitter, the previous frame's (render pixels)
    vec4 prev;         // previous render width, height; depth a, b
    vec4 misc;         // frame index (0: reset), jitter phases, velocity target live (1), unused
} pc;

// Sampled bindings (combined image samplers).
layout(set = 0, binding = 0) uniform sampler2D in_color;
#ifdef MSAA
layout(set = 0, binding = 1) uniform sampler2DMS in_depth;
#else
layout(set = 0, binding = 1) uniform sampler2D in_depth;
#endif
layout(set = 0, binding = 2) uniform sampler2D in_vel;
layout(set = 0, binding = 3) uniform sampler2D t_dil;        // xy dilated motion, z dilated depth, w farthest depth (m)
layout(set = 0, binding = 4) uniform sampler2D t_luma;       // r: this frame's luma
layout(set = 0, binding = 5) uniform sampler2D t_luma_prev;  // r: the previous frame's luma
layout(set = 0, binding = 6) uniform usampler2D t_recon;     // reconstructed previous nearest depth (float bits)
layout(set = 0, binding = 7) uniform sampler2D t_m0;         // shading change pyramid mip 0 (half render); z farthest depth mip 1
layout(set = 0, binding = 8) uniform sampler2D t_m1;
layout(set = 0, binding = 9) uniform sampler2D t_m2;
layout(set = 0, binding = 10) uniform sampler2D t_sc;        // shading change (half render)
layout(set = 0, binding = 11) uniform sampler2D t_masks;     // dilated reactive, disocclusion, shading change, accumulation
layout(set = 0, binding = 12) uniform sampler2D t_acc_prev;  // r: last frame's accumulation
layout(set = 0, binding = 13) uniform sampler2D t_lhist_prev;// last frame's luma history (4 frames)
layout(set = 0, binding = 14) uniform sampler2D t_instab;    // r: luma instability
layout(set = 0, binding = 15) uniform sampler2D t_hist_prev; // last frame's internal colour (rgb) and lock (a), output size
layout(set = 0, binding = 16) uniform sampler2D t_input_masks; // r reactive, g transparency & composition (fsr3_mask.frag; 0 without masks)

const float FP16_MIN = 6.10e-05, FP16_MAX = 65504.0, EPSILON = 6.10e-05, FP32_MAX = 3.402823466e+38, FP32_MIN = 1.175494351e-38;
const float SKY_DIST = 60000.0;
const float LOCK_THRESHOLD = 1.0, LOCK_MAX = 2.0;
const float LANCZOS_WEIGHT_SCALE = 1.0 / 16.0;
const float AVERAGE_LANCZOS_WEIGHT_PER_FRAME = 0.74 * LANCZOS_WEIGHT_SCALE;
const float ACCUMULATION_ADDED_PER_FRAME = 1.0 / 3.0, MIN_DISOCCLUSION_ACCUMULATION = -1.0 / 3.0;
const float VELOCITY_FACTOR = 1.0;

ivec2 render_size() { return ivec2(pc.size.xy); }
ivec2 output_size() { return ivec2(pc.size.zw); }
ivec2 prev_render_size() { return ivec2(pc.prev.xy); }
vec2 jitter() { return pc.jit.xy; }
vec2 prev_jitter() { return pc.jit.zw; }
float frame_index() { return pc.misc.x; }

vec3 rgb_to_ycocg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 ycocg_to_rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float rgb_to_luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float min_div_max(float a, float b, float on_zero) {
    float m = max(a, b);
    return m != 0.0 ? min(a, b) / m : on_zero;
}
bool uv_inside(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))); }
bool on_screen(ivec2 p, ivec2 sz) { return all(lessThan(uvec2(p), uvec2(sz))); }
ivec2 clamp_load(ivec2 p, ivec2 sz) { return clamp(p, ivec2(0), sz - 1); }
// uv over an image region of `region` texels inside an image of `res` texels, kept half a texel in.
vec2 clamp_uv(vec2 uv, vec2 region, vec2 res) { return clamp(uv * region, vec2(0.5), region - 0.5) / res; }
float get_4k_velocity(vec2 mv) { return length(mv * vec2(3840.0, 2160.0)); }

// View distance (metres) of a device depth; FP16_MAX for the far plane / sky.
float view_depth(float z) {
    float a = pc.prev.z, b = pc.prev.w;
    if (!(b > 0.0) || a - z <= 1e-7) return FP16_MAX;
    return min(b / (a - z), FP16_MAX);
}
float recon_depth_threshold(float metres) { return mix(0.25, 0.75, clamp(metres / 100.0, 0.0, 1.0)); }

float depth_at(ivec2 q) {
#ifdef MSAA
    return texelFetch(in_depth, q, 0).r;
#else
    return texelFetch(in_depth, q, 0).r;
#endif
}

// FSR 1's Lanczos 2 approximation of x^2 (x^2 <= 4).
float lanczos2_approx_sq(float x2) {
    x2 = min(x2, 4.0);
    float a = (2.0 / 5.0) * x2 - 1.0, b = (1.0 / 4.0) * x2 - 1.0;
    return ((25.0 / 16.0) * a * a - (25.0 / 16.0 - 1.0)) * (b * b);
}
float lanczos2(float x) {
    x = min(abs(x), 2.0);
    const float PI = 3.141592653589793;
    return x < EPSILON ? 1.0 : (sin(PI * x) / (PI * x)) * (sin(0.5 * PI * x) / (0.5 * PI * x));
}

struct Bilinear {
    ivec2 base;
    vec4 w;   // (0,0), (1,0), (0,1), (1,1)
};
Bilinear bilinear_of(vec2 uv, ivec2 sz) {
    vec2 p = uv * vec2(sz) - 0.5;
    Bilinear b;
    b.base = ivec2(floor(p));
    vec2 f = fract(p);
    b.w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    return b;
}
const ivec2 BILINEAR_OFFSETS[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
const float RECON_BILINEAR_WEIGHT_THRESHOLD = EPSILON * 10.0;

// Motion of the surface seen at render pixel q (FSR's convention: unjittered UV now -> UV last
// frame, minus the UV now), from its depth and the camera reprojection, or the velocity target
// where a skinned character's draw covers it at that depth.
vec2 motion_at(ivec2 q, float z) {
    vec2 uv = (vec2(q) + 0.5 - jitter()) / pc.size.xy;
    vec2 px = uv * pc.size.zw;   // output pixels
    vec2 ndc = (px - pc.vp.xy) * pc.vp.zw - 1.0;
    ndc.y = -ndc.y;
    float w = view_depth(z);
    bool near = w < FP16_MAX;
    vec4 v = near ? vec4(ndc * w, w, 1.0) : vec4(ndc, 1.0, 0.0);
    vec3 r = vec3(dot(pc.rx, v), dot(pc.ry, v), dot(pc.rw, v));
    if (pc.misc.z > 0.5 && near) {
        vec4 ve = texelFetch(in_vel, q, 0);
        if (ve.z > 0.0 && ve.w > 0.0 && abs(ve.w - w) < 0.03 * w + 0.01) r = vec3(ve.xy * ve.z, ve.z);
    }
    if (!(r.z > 0.0)) return vec2(0.0);
    vec2 prev = vec2(r.x / r.z, -r.y / r.z);
    vec2 ppx = (prev + 1.0) / pc.vp.zw + pc.vp.xy;
    return ppx / pc.size.zw - uv;
}
