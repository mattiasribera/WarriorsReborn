#version 450
// --fsr: RCAS (robust contrast-adaptive sharpening) of the EASU output, at the output size. A
// GLSL port of the non-packed 32-bit path (FSR_RCAS_F: FsrRcasF, without FSR_RCAS_DENOISE or
// FSR_RCAS_PASSTHROUGH_ALPHA) of AMD's ffx_fsr1.h, with the ffx_a.h helpers it uses inlined.
// con.x: the float bits of 2^-sharpness (fsr_rcas_con in src/fsr.c, FsrRcasCon), a push
// constant. Texels outside the image repeat the edge.
// ---------------------------------------------------------------------------------------------
// Derived from AMD FidelityFX Super Resolution 1 (FSR 1), ffx_fsr1.h and ffx_a.h v1.20210629,
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
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { uvec4 con; } pc;
layout(location = 0) out vec4 o;

// This is set at the limit of providing unnatural results for sharpening.
#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

// ffx_a.h helpers.
float APrxMedRcpF1(float a) {
    float b = uintBitsToFloat(uint(0x7ef19fff) - floatBitsToUint(a));
    return b * (-b * a + 2.0);
}
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }

ivec2 lim;
vec4 FsrRcasLoadF(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), lim), 0); }

void FsrRcasF(out float pixR, out float pixG, out float pixB, uvec2 ip, uvec4 con) {
    // Algorithm uses minimal 3x3 pixel neighborhood.
    //    b
    //  d e f
    //    h
    ivec2 sp = ivec2(ip);
    vec3 b = FsrRcasLoadF(sp + ivec2(0, -1)).rgb;
    vec3 d = FsrRcasLoadF(sp + ivec2(-1, 0)).rgb;
    vec3 e = FsrRcasLoadF(sp).rgb;
    vec3 f = FsrRcasLoadF(sp + ivec2(1, 0)).rgb;
    vec3 h = FsrRcasLoadF(sp + ivec2(0, 1)).rgb;
    // Rename (32-bit).
    float bR = b.r;
    float bG = b.g;
    float bB = b.b;
    float dR = d.r;
    float dG = d.g;
    float dB = d.b;
    float eR = e.r;
    float eG = e.g;
    float eB = e.b;
    float fR = f.r;
    float fG = f.g;
    float fB = f.b;
    float hR = h.r;
    float hG = h.g;
    float hB = h.b;
    // No input transform (FsrRcasInputF): the frame is already in its display encoding.
    // Min and max of ring.
    float mn4R = min(AMin3F1(bR, dR, fR), hR);
    float mn4G = min(AMin3F1(bG, dG, fG), hG);
    float mn4B = min(AMin3F1(bB, dB, fB), hB);
    float mx4R = max(AMax3F1(bR, dR, fR), hR);
    float mx4G = max(AMax3F1(bG, dG, fG), hG);
    float mx4B = max(AMax3F1(bB, dB, fB), hB);
    // Immediate constants for peak range.
    vec2 peakC = vec2(1.0, -1.0 * 4.0);
    // Limiters, these need to be high precision RCPs. Deviation from ffx_fsr1.h: the divisor is
    // kept away from 0, so a black ring (mx4 = 0) gives hitMin = 0 and a white one (mn4 = 1)
    // hitMax = 0 instead of 0 * inf or 0 / 0 = NaN, whose result in max() differs between
    // implementations; elsewhere the values are the same.
    float hitMinR = min(mn4R, eR) * (1.0 / max(4.0 * mx4R, 1.0 / 65536.0));
    float hitMinG = min(mn4G, eG) * (1.0 / max(4.0 * mx4G, 1.0 / 65536.0));
    float hitMinB = min(mn4B, eB) * (1.0 / max(4.0 * mx4B, 1.0 / 65536.0));
    float hitMaxR = (peakC.x - max(mx4R, eR)) * (1.0 / min(4.0 * mn4R + peakC.y, -1.0 / 65536.0));
    float hitMaxG = (peakC.x - max(mx4G, eG)) * (1.0 / min(4.0 * mn4G + peakC.y, -1.0 / 65536.0));
    float hitMaxB = (peakC.x - max(mx4B, eB)) * (1.0 / min(4.0 * mn4B + peakC.y, -1.0 / 65536.0));
    float lobeR = max(-hitMinR, hitMaxR);
    float lobeG = max(-hitMinG, hitMaxG);
    float lobeB = max(-hitMinB, hitMaxB);
    float lobe = max(float(-FSR_RCAS_LIMIT), min(AMax3F1(lobeR, lobeG, lobeB), 0.0)) * uintBitsToFloat(con.x);
    // Resolve, which needs the medium precision rcp approximation to avoid visible tonality
    // changes.
    float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    pixR = (lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL;
    pixG = (lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL;
    pixB = (lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL;
}

void main() {
    lim = textureSize(src, 0) - 1;
    vec3 c;
    FsrRcasF(c.r, c.g, c.b, uvec2(gl_FragCoord.xy), pc.con);
    o = vec4(c, 1.0);
}
