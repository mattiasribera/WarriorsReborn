// Stochastic screen-space reflections for the wet ground (--reflections sssr, wet.frag with SSSR > 0):
// GLSL ported from AMD FidelityFX SSSR, SDK v1.1.4 (sdk/include/FidelityFX/gpu/sssr/ffx_sssr_intersect.h:
// SampleGGXVNDF, SampleReflectionVector, FFX_SSSR_InitialAdvanceRay, FFX_SSSR_AdvanceRay,
// FFX_SSSR_HierarchicalRaymarch, FFX_SSSR_ValidateHit), adapted to this renderer:
// - non-inverted depth (NDC 0 near .. 1 far, clip z = a w - b) and no wave operations: Vulkan 1.0 has
//   no subgroups, so the low-occupancy early exit is left out and the march stops at a fixed budget
//   (sl.sssr.y); a hit is the march stepping below its most detailed level, running out of budget,
//   leaving the screen or the ray's reach is a miss;
// - the hierarchy (binding 6, sssr_hiz.frag) is the min depth of the internal-size depth, level 0 that
//   depth itself, levels 1-6 each half the size (rounded up);
// - the ray is a segment: t = 1 is its end (the wet pass's reach, kept in front of the camera);
// - the hit is validated by its distance behind the surface there (the classic trace's thickness),
//   the self-intersection and background tests and FidelityFX's screen-border vignette; there is no
//   normal buffer, so the back-face test is left out;
// - random numbers: interleaved gradient noise per texel, offset by a golden-ratio sequence per
//   frame (no blue-noise texture).
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

const float SSSR_PI = 3.14159265358979;
const float SSSR_FLOAT_MAX = 3.402823466e+38;
const int SSSR_MAX_MIP = 6;           // the hierarchy's last level (FFX_SSSR_DEPTH_HIERARCHY_MAX_MIP)
const int SSSR_STEPS_LIMIT = 96;      // the loop bound (src/sssr.h SSSR_MAX_STEPS_LIMIT)

// http://jcgt.org/published/0007/04/01/paper.pdf by Eric Heitz
// Input Ve: view direction
// Input alpha_x, alpha_y: roughness parameters
// Input U1, U2: uniform random numbers
// Output Ne: normal sampled with PDF D_Ve(Ne) = G1(Ve) * max(0, dot(Ve, Ne)) * D(Ne) / Ve.z
vec3 sssr_sample_ggx_vndf(vec3 Ve, float alpha_x, float alpha_y, float U1, float U2) {
    // Section 3.2: transforming the view direction to the hemisphere configuration
    vec3 Vh = normalize(vec3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));
    // Section 4.1: orthonormal basis (with special case if cross product is zero)
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);
    // Section 4.2: parameterization of the projected area
    float r = sqrt(U1);
    float phi = 2.0 * SSSR_PI * U2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
    // Section 4.3: reprojection onto hemisphere
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    // Section 3.4: transforming the normal back to the ellipsoid configuration
    return normalize(vec3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0, Nh.z)));
}

// The reflected direction (view space) of the view direction V about a microfacet normal sampled
// from the GGX distribution of roughness alpha around N, with the random numbers u.
vec3 sssr_sample_reflection(vec3 V, vec3 N, float alpha, vec2 u) {
    vec3 U;
    if (abs(N.z) > 0.0) {
        float k = sqrt(N.y * N.y + N.z * N.z);
        U = vec3(0.0, -N.z / k, N.y / k);
    } else {
        float k = sqrt(N.x * N.x + N.y * N.y);
        U = vec3(N.y / k, -N.x / k, 0.0);
    }
    // TBN 3x3 matrix
    vec3 T0 = U, T1 = cross(N, U), T2 = N;
    vec3 vt = vec3(dot(T0, -V), dot(T1, -V), dot(T2, -V));
    vec3 nt = sssr_sample_ggx_vndf(vt, alpha, alpha, u.x, u.y);
    vec3 rt = reflect(-vt, nt);
    // transpose(TBN) * reflected direction
    return rt.x * T0 + rt.y * T1 + rt.z * T2;
}

// The hierarchy's level m at screen uv: its min NDC depth (texel clamped to the level).
float sssr_load(vec2 pos, int m) {
    ivec2 sz = textureSize(sssr_hiz, m);
    return texelFetch(sssr_hiz, clamp(ivec2(pos), ivec2(0), sz - 1), m).r;
}

void sssr_initial_advance(vec3 origin, vec3 direction, vec3 inv_direction, vec2 mip_res, vec2 mip_res_inv, vec2 floor_offset, vec2 uv_offset,
                          out vec3 position, out float current_t) {
    vec2 mip_pos = mip_res * origin.xy;
    // Intersect ray with the half box that is pointing away from the ray origin.
    vec2 xy_plane = floor(mip_pos) + floor_offset;
    xy_plane = xy_plane * mip_res_inv + uv_offset;
    // o + d * t = p' => t = (p' - o) / d
    vec2 t = xy_plane * inv_direction.xy - origin.xy * inv_direction.xy;
    current_t = min(t.x, t.y);
    position = origin + current_t * direction;
}

bool sssr_advance(vec3 origin, vec3 direction, vec3 inv_direction, vec2 mip_pos, vec2 mip_res_inv, vec2 floor_offset, vec2 uv_offset, float surface_z,
                  inout vec3 position, inout float current_t) {
    // Create boundary planes
    vec2 xy_plane = floor(mip_pos) + floor_offset;
    xy_plane = xy_plane * mip_res_inv + uv_offset;
    vec3 boundary_planes = vec3(xy_plane, surface_z);
    // Intersect ray with the half box that is pointing away from the ray origin.
    vec3 t = boundary_planes * inv_direction - origin * inv_direction;
    // Prevent using z plane when shooting out of the depth buffer.
    t.z = direction.z > 0.0 ? t.z : SSSR_FLOAT_MAX;
    // Choose nearest intersection with a boundary.
    float t_min = min(min(t.x, t.y), t.z);
    // Smaller z means closer to the camera.
    bool above_surface = surface_z > position.z;
    // Decide whether we are able to advance the ray until we hit the xy boundaries or if we had to
    // clamp it at the surface (bitwise: is t_min the t.z fed into the min above).
    bool skipped_tile = floatBitsToUint(t_min) != floatBitsToUint(t.z) && above_surface;
    // Make sure to only advance the ray if we're still above the surface.
    current_t = above_surface ? t_min : current_t;
    position = origin + current_t * direction;
    return skipped_tile;
}

// The march from origin (screen uv, NDC depth) along direction (uv and depth per t; t 1 the ray's
// end) through the hierarchy of the screen size `screen` (level 0 texels), starting at level
// `most_detailed`, at most `steps` intersections. Returns the position; *hit whether it is a hit.
vec3 sssr_march(vec3 origin, vec3 direction, vec2 screen, int most_detailed, int steps, out bool hit) {
    vec3 inv_direction = vec3(direction.x != 0.0 ? 1.0 / direction.x : SSSR_FLOAT_MAX, direction.y != 0.0 ? 1.0 / direction.y : SSSR_FLOAT_MAX,
                              direction.z != 0.0 ? 1.0 / direction.z : SSSR_FLOAT_MAX);
    int mip = most_detailed;
    vec2 mip_res = screen * exp2(-float(mip)), mip_res_inv = 1.0 / mip_res;
    // Offset to the bounding boxes uv space to intersect the ray with the center of the next pixel.
    vec2 uv_offset = 0.005 * exp2(float(most_detailed)) / screen;
    uv_offset = mix(uv_offset, -uv_offset, lessThan(direction.xy, vec2(0.0)));
    // Offset applied depending on current mip resolution to move the boundary to the left/right
    // upper/lower border depending on ray direction.
    vec2 floor_offset = mix(vec2(1.0), vec2(0.0), lessThan(direction.xy, vec2(0.0)));
    // Initially advance ray to avoid immediate self intersections.
    float t;
    vec3 position;
    sssr_initial_advance(origin, direction, inv_direction, mip_res, mip_res_inv, floor_offset, uv_offset, position, t);
    hit = false;
    for (int i = 0; i < SSSR_STEPS_LIMIT; i++) {
        if (i >= steps || t > 1.0 || any(lessThan(position.xy, vec2(0.0))) || any(greaterThan(position.xy, vec2(1.0)))) return position;
        vec2 mip_pos = mip_res * position.xy;
        float surface_z = sssr_load(mip_pos, mip);
        bool skipped = sssr_advance(origin, direction, inv_direction, mip_pos, mip_res_inv, floor_offset, uv_offset, surface_z, position, t);
        // Don't increase mip further than this because we did not generate it
        if (!(skipped && mip >= SSSR_MAX_MIP)) {
            mip += skipped ? 1 : -1;
            mip_res *= skipped ? 0.5 : 2.0;
            mip_res_inv *= skipped ? 2.0 : 0.5;
        }
        if (mip < most_detailed) {
            hit = t <= 1.0;
            return position;
        }
    }
    return position;
}

// How far to trust a hit (0-1): the screen-border vignette times how close the ray's point (view
// depth) lies to the surface there (surface_z, its NDC depth; within `thickness` view units); 0 off
// the screen, for a hit within two texels of the start (self intersection) and on the background.
float sssr_validate(vec3 hit, vec2 uv, vec2 screen, float surface_z, float thickness) {
    // Reject hits outside the view frustum
    if (hit.x < 0.0 || hit.y < 0.0 || hit.x > 1.0 || hit.y > 1.0) return 0.0;
    // Reject the hit if we didnt advance the ray significantly to avoid immediate self reflection
    vec2 manhattan = abs(hit.xy - uv);
    if (manhattan.x < 2.0 / screen.x && manhattan.y < 2.0 / screen.y) return 0.0;
    // Don't lookup radiance from the background.
    if (surface_z >= 1.0) return 0.0;
    float dist = abs(view_depth(hit.z) - view_depth(surface_z));
    // Fade out hits near the screen borders
    vec2 fov = 0.05 * vec2(screen.y / screen.x, 1.0);
    vec2 border = smoothstep(vec2(0.0), fov, hit.xy) * (1.0 - smoothstep(vec2(1.0) - fov, vec2(1.0), hit.xy));
    float vignette = border.x * border.y;
    // We accept all hits that are within a reasonable minimum distance below the surface.
    float confidence = 1.0 - smoothstep(0.0, thickness, dist);
    return vignette * confidence * confidence;
}
