#version 450
#extension GL_GOOGLE_include_directive : require
// Contact shadows (--contact-shadows, renderer_contact), chosen by the specialization constant MODE:
// 0: the occlusion mask at a reduced size (pc.dir.x internal pixels per texel): every upward-facing
//    pixel of the depth that is not a character sends a cone of short rays up from the surface (one
//    straight up, six tilted about 40 degrees) and marches each through the depth; a ray point with
//    a surface in front of it (nearer the camera, within the thickness) is occluded: nearer the
//    ground counts more. Out: r the occlusion, 0-1.
// 1: its composite at the internal size (bilinear) with blend DST_COLOR / ZERO (rgb only): the
//    colour times 1 - strength x occlusion (at most 0.9 darker). Character pixels and pixels without
//    occlusion are discarded, so people never darken and the rest keeps its exact colour.
// Push constants (ssao_common.glsl): proj, size as for SSAO; params: radius (view units), strength,
// thickness (view units), 0; dir: internal pixels per mask texel, mask width, height, 0; cam[0].xyz:
// world up in view space (x right, y up, z towards the viewer).
#define CHAR_MASK
#include "ssao_common.glsl"
layout(constant_id = 0) const int MODE = 0;
layout(set = 0, binding = 1) uniform sampler2D mask_tex;   // MODE 1: the occlusion mask
layout(location = 0) out vec4 o;

const int DIRS = 7, STEPS = 6;

// Internal pixel coordinates (continuous) of a view position in front of the camera.
vec2 to_pix(vec3 v) {
    float d = -v.z;
    vec2 ndc = vec2(v.x / (d * pc.proj.z), v.y / (d * pc.proj.w));
    return vec2((ndc.x * 0.5 + 0.5) * pc.size.x, (0.5 - 0.5 * ndc.y) * pc.size.y);
}

float occlusion() {
    ivec2 mx = ivec2(pc.size.xy) - 1, p = min(ivec2(gl_FragCoord.xy * pc.dir.x), mx);
    if (depth_at(p) >= 1.0 || is_char(p)) return 0.0;
    vec3 P = view_pos(p);
    float d = -P.z, fade = 1.0 - smoothstep(20.0, 35.0, d);   // far ground: its texels are too coarse
    if (fade <= 0.0) return 0.0;
    vec3 n = surface_normal(p, P);
    if (dot(n, n) == 0.0) return 0.0;
    vec3 U = pc.cam[0].xyz;
    float up = smoothstep(0.6, 0.8, dot(n, U));   // the ground and the tops of things, not walls
    if (up <= 0.0) return 0.0;
    vec3 t1 = normalize(cross(U, abs(U.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0))), t2 = cross(U, t1);
    float R = pc.params.x, th = pc.params.z, bias = 0.02 + 0.004 * d;
    vec3 O = P + n * (0.01 + 0.002 * d);   // off the surface itself
    float occ = 0.0;
    for (int i = 0; i < DIRS; i++) {
        float a = 6.2831853 * float(i) / float(DIRS - 1);
        vec3 D = i == 0 ? U : normalize(U + 0.85 * (cos(a) * t1 + sin(a) * t2));
        for (int s = 1; s <= STEPS; s++) {
            float h = R * float(s) / float(STEPS);
            vec3 S = O + D * h;
            if (S.z > -0.05) break;   // behind the camera
            vec2 q = to_pix(S);
            if (q.x < 0.0 || q.y < 0.0 || q.x >= pc.size.x || q.y >= pc.size.y) break;
            float z = depth_at(ivec2(q));
            if (z >= 1.0) continue;
            float dz = -S.z - view_depth(z);   // > 0: a surface in front of the ray point
            if (dz > bias && dz < th) {
                occ += 1.0 - 0.7 * (h / R);
                break;
            }
        }
    }
    return clamp(occ / float(DIRS), 0.0, 1.0) * up * fade;
}

void comp() {
    // Texel t was worked out at internal pixel t x scale: continuous texel coordinate of this pixel.
    vec2 t = (gl_FragCoord.xy - 0.5) / pc.dir.x + 0.5;
    float m = texture(mask_tex, t / pc.dir.yz).r;
    if (m <= 2e-3 || is_char(ivec2(gl_FragCoord.xy))) discard;
    float f = 1.0 - min(pc.params.y * m, 0.9);
    o = vec4(f, f, f, 1.0);
}

void main() {
    if (MODE == 1)
        comp();
    else
        o = vec4(occlusion(), 0.0, 0.0, 0.0);
}
