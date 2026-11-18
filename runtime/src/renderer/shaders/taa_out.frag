#version 450
// --taa output (renderer_taa): the new history drawn back into the frame target, sharpened by
// `sharpness` (0-1, proj.z) with RCAS: AMD FidelityFX FSR 1's robust
// contrast-adaptive sharpening (ffx_fsr1.h FsrRcasF's lobe and limiter, as fsr_rcas.frag; MIT, notice
// below), its lobe scaled by `sharpness` (1 = FSR 1's strongest, 0 stops). The lobe shrinks where
// the four neighbours' contrast is already high, so soft detail gains more than hard edges (thin
// halos stay small) and no channel is pushed past 0 or 1. 0: the history as is. UPSCALE
// (--taa-upscale): also
// writes the upscaled depth (taa_up.frag's third output) so the passes after it test and read the
// scene's depth at the output size.
// ---------------------------------------------------------------------------------------------
// The RCAS part is derived from AMD FidelityFX Super Resolution 1 (FSR 1), ffx_fsr1.h v1.20210629,
// https://github.com/GPUOpen-Effects/FidelityFX-FSR
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// ---------------------------------------------------------------------------------------------
layout(set = 0, binding = 0) uniform sampler2D hist_tex;
#ifdef UPSCALE
layout(set = 0, binding = 5) uniform sampler2D up_depth;
#endif
layout(push_constant) uniform Push {
    vec4 rx, ry, rw;
    vec4 proj;   // z: sharpness
    vec4 size;   // target width, height, 1 / width, 1 / height
    vec4 vp;
    vec4 obj;
    vec4 up;
} pc;
layout(location = 0) out vec4 o;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), last = ivec2(pc.size.xy) - 1;
    vec4 c = texelFetch(hist_tex, p, 0);
    float k = pc.proj.z;
    if (k > 0.0) {
        vec3 b = texelFetch(hist_tex, clamp(p + ivec2(0, -1), ivec2(0), last), 0).rgb;
        vec3 d = texelFetch(hist_tex, clamp(p + ivec2(-1, 0), ivec2(0), last), 0).rgb;
        vec3 f = texelFetch(hist_tex, clamp(p + ivec2(1, 0), ivec2(0), last), 0).rgb;
        vec3 h = texelFetch(hist_tex, clamp(p + ivec2(0, 1), ivec2(0), last), 0).rgb;
        vec3 e = clamp(c.rgb, 0.0, 1.0);
        vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
        // The lobe that would take a channel to 0 or 1 at most (fsr_rcas.frag's divisors kept away from 0).
        vec3 hit_min = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
        vec3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
        vec3 l3 = max(-hit_min, hit_max);
        float lobe = max(-(0.25 - 1.0 / 16.0), min(max(l3.r, max(l3.g, l3.b)), 0.0)) * k;
        c.rgb = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    }
    o = c;
#ifdef UPSCALE
    gl_FragDepth = texelFetch(up_depth, p, 0).r;
#endif
}
