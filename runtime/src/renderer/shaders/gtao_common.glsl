// --ssao-mode gtao: shared by gtao_prefilter.comp, gtao_main.comp and
// gtao_denoise.comp. A GLSL port of XeGTAO's 32-bit path (no half floats, no bent normals, no
// multi-bounce) at the --ssao working size (ssao.h ssao_work_size: working texel (i, j) stands for
// internal pixel (SCALE i + SCALE / 2 - 1, ...)). Constants from gtao_constants (src/ssao.c).
// ---------------------------------------------------------------------------------------------
// Derived from XeGTAO (XeGTAO.h v1.30, XeGTAO.hlsli, vaGTAO.hlsl), https://github.com/GameTechDev/XeGTAO
// at commit a5b1686c7ea37788eeb3576b5be47f7c03db532c. XeGTAO is based on GTAO/GTSO, Jimenez et al.,
// "Practical Real-Time Strategies for Accurate Indirect Occlusion".
//
// MIT License
//
// Copyright (C) 2016-2021, Intel Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ---------------------------------------------------------------------------------------------
layout(push_constant) uniform PC {
    vec4 depth;   // DepthUnpackConsts (B, A: view depth = B / (A - NDC z)), dead zone, depth MIP levels
    vec4 ndc;     // NDCToViewMul.xy, NDCToViewAdd.xy ([0, 1] screen, y down -> view x, y)
    vec4 scr;     // working texel -> [0, 1] screen: mul.xy, add.xy (gtao_constants screen_mul / add)
    vec4 size;    // working width, height, internal width, height
    vec4 rad;     // effect radius (view units), falloff range, sample distribution power, thin occluder compensation
    vec4 misc;    // final value power, depth MIP sampling offset, largest radius (working texels), noise index
    vec4 den;     // denoise blur beta, 1 = the last denoise pass, surface bias (view units), -
} pc;
layout(constant_id = 1) const int SCALE = 2;   // internal pixels per working texel
const float GTAO_FAR = 65000.0;                // the view depth of the far plane (nothing drawn); fits fp16
const float GTAO_PI = 3.1415926535897932;
const float GTAO_PI_HALF = 1.5707963267948966;

// XeGTAO_ComputeViewspacePosition at working texel t (may lie outside the image).
vec3 gtao_view_pos(vec2 t, float z) {
    vec2 s = t * pc.scr.xy + pc.scr.zw;
    return vec3((pc.ndc.xy * s + pc.ndc.zw) * z, z);
}

// XeGTAO_CalculateEdges: 1 where a neighbour lies on the same surface, towards 0 across a depth edge.
vec4 gtao_edges(float c, float l, float r, float t, float b) {
    vec4 e = vec4(l, r, t, b) - c;
    float slope_lr = (e.y - e.x) * 0.5, slope_tb = (e.w - e.z) * 0.5;
    vec4 adj = e + vec4(slope_lr, -slope_lr, slope_tb, -slope_tb);
    e = min(abs(e), abs(adj));
    return clamp(1.25 - e / (c * 0.011), 0.0, 1.0);
}
// XeGTAO_PackEdges / UnpackEdges, 2 bits per edge; stored as the integer 0-255 (exact in fp16).
float gtao_pack_edges(vec4 e) {
    e = round(clamp(e, 0.0, 1.0) * 2.9);
    return dot(e, vec4(64.0, 16.0, 4.0, 1.0));
}
vec4 gtao_unpack_edges(float v) {
    uint p = uint(v + 0.5);
    return clamp(vec4((p >> 6) & 3u, (p >> 4) & 3u, (p >> 2) & 3u, p & 3u) / 3.0, 0.0, 1.0);
}
