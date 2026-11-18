#version 450
#extension GL_GOOGLE_include_directive : require
// The min-depth hierarchy of --reflections sssr (wet.frag's sssr_hiz, binding 6): one level per pass,
// each texel the smallest (nearest) NDC depth under it, as FidelityFX SSSR's depth downsample
// (ffx_sssr_depth_downsample.h, SDK v1.1.4; its single-pass SPD compute shader with wave operations is
// replaced by one fragment pass per level for Vulkan 1.0).
// FROM_DEPTH: level 0 at the internal size from the frame's depth (MSAA: sample 0, as depth_at);
// otherwise level n from level n - 1 (binding 0), half its size rounded down (the image's mips): a
// texel covers its 2 x 2 texels of the level above, the last column and row also the level above's
// odd one left over, so the last texel of every level covers the rest of level 0.
//
// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
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
layout(location = 0) out float o;
#ifdef FROM_DEPTH
#include "ssao_common.glsl"
void main() { o = depth_at(ivec2(gl_FragCoord.xy)); }
#else
layout(set = 0, binding = 0) uniform sampler2D prev;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy), b = 2 * q, mx = textureSize(prev, 0) - 1;
    // Up to 3 x 3 at the last column / row of an odd level above.
    ivec2 e = mix(b + 1, mx, equal(b + 2, mx));
    float m = 1.0;
    for (int y = b.y; y <= e.y; y++)
        for (int x = b.x; x <= e.x; x++) m = min(m, texelFetch(prev, min(ivec2(x, y), mx), 0).r);
    o = m;
}
#endif
