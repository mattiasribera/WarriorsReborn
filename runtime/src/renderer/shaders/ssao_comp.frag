#version 450
#extension GL_GOOGLE_include_directive : require
// Composites over the depth of the opaque world at the internal size, chosen by the specialization
// constant MODE:
// 0: SSAO (--ssao), multiplied onto the target (blend DST_COLOR / ZERO, rgb only: the bloom light
//    mask in alpha is kept): the four nearest half-size texels, bilinear weights times how close
//    each texel's view depth is to this pixel's (taps from another surface are left out; without
//    any close tap the closest one), then 1 - strength x occlusion. The far plane (depth 1) and
//    unoccluded pixels are discarded, so they keep their exact color.
// 1: rain splash mask (--rain-hd, renderer_rain_splash) at the output size, pc.dir.w internal
//    pixels per texel (no blend): the drops' strength there, see splash() below.
// 2: its composite at the internal size (the mask bilinear), drawn twice: brightening the target
//    (blend DST_COLOR / ONE: dst x (1 + colour x strength)), then adding a share of it as flat
//    colour (ONE / ONE, scaled by pc.dir.w); alpha 0, rgb only. Pixels without drops are discarded.
#include "ssao_common.glsl"
layout(constant_id = 0) const int MODE = 0;
layout(set = 0, binding = 1) uniform sampler2D ao_tex;   // MODE 0: the occlusion, MODE 2: the splash mask
layout(location = 0) out vec4 o;

void ssao() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float z = depth_at(p);
    if (z >= 1.0) discard;
    float d = view_depth(z);
    vec2 h = vec2(p) * 0.5;   // half-size texel i holds the pixel 2i (its top-left)
    ivec2 b = ivec2(floor(h)), mx = ivec2(pc.dir.zw) - 1;
    vec2 fr = h - vec2(b);
    float tol = 0.03 * d + 0.01, sum = 0.0, wsum = 0.0, best = 1e30, best_ao = 0.0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec2 t = texelFetch(ao_tex, clamp(b + ivec2(i, j), ivec2(0), mx), 0).rg;
            float dd = abs(t.g - d);
            float w = (i == 1 ? fr.x : 1.0 - fr.x) * (j == 1 ? fr.y : 1.0 - fr.y) + 1e-3;
            w *= max(1.0 - dd / tol, 0.0);
            sum += t.r * w;
            wsum += w;
            if (dd < best) {
                best = dd;
                best_ao = t.r;
            }
        }
    float ao = wsum > 1e-4 ? sum / wsum : best_ao;
    if (ao <= 0.0) discard;
    float f = clamp(1.0 - pc.params.y * ao, 0.0, 1.0);
    o = vec4(f, f, f, 1.0);
}

// ---- Rain splashes (MODE 1: the mask) ----
// pc.proj, pc.size as for SSAO; pc.params: time (s), cell (world units), period (s), chance;
// pc.dir: splash and ripple strength, fade distance, internal pixels per mask texel; pc.cam[0..3].xyz: the eye and
// the camera's right, up and back axes in a world frame whose up is +z; pc.cam[0..2].w: the colour.
// Per pixel: its view and world position from the depth. The world position, on the ground plane,
// falls in a grid of cells; each cell has one impact per period at a random phase (a cell
// constant) and a random place and size (per impact), with probability chance, so the drops are
// fixed in the world and only time moves them. An impact throws 2-3 droplets up and out for 0.16 s
// (each droplet a small sphere tested against this pixel's view ray: seen in front of the ground
// behind it) and, on flat ground, a ring widens and fades over 0.5 s with a fainter inner ring.
// Features thinner than a pixel are widened to one and dimmed by the same amount (no aliasing),
// everything fades from 40% of the fade distance to it. Only where a drop shows is the surface
// checked (most pixels end before these depth reads): the normal from the depths two pixels away
// (as ssao_ao.frag) and, four to twelve pixels away (about 5 cm), whether the surface is flat.
// Only upward-facing (normal within ~40 degrees of up; rings: ~18 degrees), flat surfaces keep
// their drops: the floor, roofs, tops of crates, not walls and not the curved tops of characters.
const float SPLASH_TIME = 0.16, RIPPLE_TIME = 0.5, DROP_RADIUS = 0.006;

// Integer hash (PCG-style 3D mix, as shaders/rain.frag): three values in [0, 1).
vec3 hash3(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return vec3(v >> 8u) * (1.0 / 16777216.0);
}

// Right-handed view position (x right, y up, the camera looks down -z) of pixel p.
vec3 view_pos(ivec2 p) {
    ivec2 q = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float d = view_depth(depth_at(q));
    vec2 ndc = vec2((float(q.x) + 0.5) * pc.size.z * 2.0 - 1.0, 1.0 - (float(q.y) + 0.5) * pc.size.w * 2.0);
    return vec3(ndc.x * d * pc.proj.z, ndc.y * d * pc.proj.w, -d);
}
vec3 world_dir(vec3 v) { return v.x * pc.cam[1].xyz + v.y * pc.cam[2].xyz + v.z * pc.cam[3].xyz; }

float splash() {
    ivec2 mx = ivec2(pc.size.xy) - 1, p = min(ivec2(gl_FragCoord.xy * pc.dir.w), mx);
    float z = depth_at(p);
    if (z >= 1.0) return 0.0;
    vec3 P = view_pos(p);
    float d = -P.z, fade = 1.0 - smoothstep(0.4 * pc.dir.z, pc.dir.z, d);
    if (fade <= 0.0) return 0.0;
    float pw = 2.0 * d * pc.proj.w * pc.size.w * pc.dir.w;   // world size of a mask texel here
    vec3 V = world_dir(P);                        // from the eye to the surface point
    vec3 W = pc.cam[0].xyz + V, ray = normalize(V);
    // The 2x2 cells around the ground point under a droplet's mid height (parallax).
    float C = pc.params.y, T = pc.params.z, t = pc.params.x;
    float lift = 1.0 / max(-ray.z, 0.28);   // ground distance per unit of height along the ray
    vec2 g = W.xy - ray.xy * (0.02 * lift);
    float reach = 0.1 + 0.045 * lift;       // the largest ring, or a droplet's throw and height
    ivec2 base = ivec2(floor(g / C - 0.5));
    float rings = 0.0, drops = 0.0;
    for (int j = 0; j < 4; j++) {
        ivec2 c = base + ivec2(j & 1, j >> 1);
        uvec2 uc = uvec2(c);
        float ph = t / T + hash3(uvec3(uc, 0xFFFFFF71u)).x;
        float ev = floor(ph), age = (ph - ev) * T;
        if (age >= RIPPLE_TIME) continue;
        uint e = uint(int(ev));
        vec3 h = hash3(uvec3(uc, e));
        if (h.z >= pc.params.w) continue;
        vec2 I = (vec2(c) + h.xy) * C;   // the impact
        float dist = length(W.xy - I);
        if (dist > reach) continue;
        vec3 h2 = hash3(uvec3(uc.y, uc.x, e ^ 0x9E3779B9u));
        if (pc.dir.y > 0.0) {
            float a = age / RIPPLE_TIME, grow = 1.0 - (1.0 - a) * (1.0 - a);
            float rmax = min(0.07, 0.4 * C) * (0.7 + 0.6 * h2.x);
            float w0 = 0.003 + 0.004 * a, w = max(w0, 0.75 * pw);
            float e1 = (dist - rmax * grow) / w;
            float ring = exp(-e1 * e1);
            if (age > 0.08) {
                float a2 = (age - 0.08) / RIPPLE_TIME, e2 = (dist - 0.55 * rmax * (1.0 - (1.0 - a2) * (1.0 - a2))) / w;
                ring += 0.45 * exp(-e2 * e2);
            }
            rings += ring * (w0 / w) * (1.0 - a) * (1.0 - a);
        }
        if (age < SPLASH_TIME && pc.dir.x > 0.0) {
            float a = age / SPLASH_TIME;
            int nd = h2.y < 0.4 ? 2 : 3;
            float zd = 4.0 * 0.03 * (0.7 + 0.6 * h2.x) * a * (1.0 - a);   // up and down again
            float rho = 0.05 * (0.75 + 0.5 * h2.y) * a;                    // and out
            for (int m = 0; m < 3; m++) {
                if (m >= nd) break;
                float ang = 6.2831853 * (h2.z + float(m) / float(nd));
                vec3 Q = vec3(I + rho * vec2(cos(ang), sin(ang)), W.z + zd) - pc.cam[0].xyz;   // from the eye
                float along = dot(Q, ray);
                float re = max(DROP_RADIUS, 1.2 * along * pc.proj.w * pc.size.w * pc.dir.w);
                float cov = 1.0 - smoothstep(0.5 * re, re, length(Q - along * ray));
                drops += cov * (DROP_RADIUS * DROP_RADIUS) / (re * re) * (1.0 - a * a);
            }
        }
    }
    float sum = (pc.dir.x * drops + pc.dir.y * rings) * fade;
    if (sum <= 1e-3) return 0.0;
    // The surface: upward-facing (rings: flatter still) and flat.
    vec3 r = view_pos(p + ivec2(2, 0)), l = view_pos(p - ivec2(2, 0));
    vec3 u = view_pos(p - ivec2(0, 2)), dn = view_pos(p + ivec2(0, 2));
    bool use_r = p.x + 2 <= mx.x && (p.x < 2 || abs(r.z - P.z) < abs(P.z - l.z));
    bool use_d = p.y + 2 <= mx.y && (p.y < 2 || abs(dn.z - P.z) < abs(P.z - u.z));
    vec3 n = cross(use_r ? r - P : P - l, use_d ? dn - P : P - u);
    if (dot(n, n) < 1e-20) return 0.0;
    n = normalize(n);
    if (dot(n, P) > 0.0) n = -n;   // towards the camera
    float up = world_dir(n).z;
    if (up < 0.75) return 0.0;
    if (up < 0.95) sum = pc.dir.x * drops * fade;
    int k = int(clamp(0.05 * pc.dir.w / pw, 4.0, 12.0));   // internal pixels
    vec4 off = abs(vec4(dot(view_pos(p + ivec2(k, 0)) - P, n), dot(view_pos(p - ivec2(k, 0)) - P, n),
                        dot(view_pos(p + ivec2(0, k)) - P, n), dot(view_pos(p - ivec2(0, k)) - P, n)));
    float tol = 0.006 + 0.15 * pw / pc.dir.w * float(k);
    return min(sum * (1.0 - smoothstep(tol, 2.0 * tol, max(max(off.x, off.y), max(off.z, off.w)))), 4.0);
}

// MODE 2: the mask composited (bilinear) in the colour; pc.dir.w: the output's scale.
void splash_comp() {
    float m = texture(ao_tex, gl_FragCoord.xy * pc.size.zw).r;
    if (m <= 1e-3) discard;
    o = vec4(vec3(pc.cam[0].w, pc.cam[1].w, pc.cam[2].w) * (m * pc.dir.w), 0.0);
}

void main() {
    if (MODE == 1)
        o = vec4(splash(), 0.0, 0.0, 0.0);
    else if (MODE == 2)
        splash_comp();
    else
        ssao();
}
