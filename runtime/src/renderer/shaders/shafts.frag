#version 450
#extension GL_GOOGLE_include_directive : require
// --light-shafts (renderer_light_shafts): the lamps' light scattered in the air, chosen by the
// specialization constant MODE:
// 0: the in-scattered light at the working size (SCALE internal pixels per texel, texel t holds
//    internal pixel SCALE t + SCALE / 2 - 1) into an R16G16B16A16 image: rgb the light, a the view
//    depth (1e4 on the far plane). The view ray of the texel runs from the camera to the visible
//    surface (at most pc.params.z view units): a wall, a character or the ground in front of a
//    light's volume hides what lies behind it. Each light's volume is bounded by a sphere; the part
//    of the ray inside it is sampled STEPS times, offset by a screen-fixed interleaved gradient
//    noise (no banding, steady while the camera moves). A lamp (kind 0) is a cone below it,
//    light_shafts.h light_shafts_cone, plus a small glow round the lamp head; a fire (kind 1) a
//    round glow rising above it. Each sample is weighted by a Henyey-Greenstein phase (looking
//    towards a lamp through its light is brighter), the sum x pc.params.y (density) goes through a
//    soft knee (L / (1 + L)): many cones do not wash the scene out.
// 1: the composite at the internal size: the 3x3 nearest texels, a tent weight times how close their
//    view depth is to this pixel's (edges of walls and characters stay sharp), drawn with blend
//    ONE / ONE (added; alpha, the bloom light mask, unchanged) with a +-half-step dither.
#include "ssao_common.glsl"
layout(constant_id = 0) const int MODE = 0;
layout(constant_id = 1) const int SCALE = 2;
layout(constant_id = 2) const int STEPS = 8;
layout(set = 0, binding = 1) uniform sampler2D shaft_tex;   // MODE 1: mode 0's image
#define SHAFT_MAX 16
layout(set = 0, binding = 2) uniform Shafts {
    vec4 pos_len[SHAFT_MAX];    // right-handed view position of the light, volume length (view units)
    vec4 col_kind[SHAFT_MAX];   // colour x intensity, kind (0 lamp cone, 1 fire glow)
    vec4 axis[SHAFT_MAX];       // unit view-space axis: world down (lamp) or up (fire)
} sh;
layout(location = 0) out vec4 o;
// pc.params: lights in use, density, longest ray (view units), -. pc.dir: -, -, working width, height.
const float COS_OUTER = 0.766;   // light_shafts.h LIGHT_SHAFTS_COS_OUTER
const float COS_INNER = 0.940;   // LIGHT_SHAFTS_COS_INNER
const float NEAR_K = 3.0;        // LIGHT_SHAFTS_NEAR_K
const float GLOW = 0.12;         // lamp head glow radius, share of the length
const float GLOW_GAIN = 0.6;
const float HG = 0.35;           // forward scattering

float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

float phase(float c) {
    float g2 = HG * HG;
    return (1.0 - g2) / pow(1.0 + g2 - 2.0 * HG * c, 1.5);
}

float cone(vec3 v, vec3 ax, float len) {   // light_shafts.h light_shafts_cone
    float h = dot(v, ax);
    if (h <= 0.0 || h >= len) return 0.0;
    float edge = smoothstep(COS_OUTER, COS_INNER, h / max(length(v), 1e-4));
    float x = h / len, a = 1.0 - x;
    return edge * a * a / (1.0 + NEAR_K * x * x);
}

void scatter() {
    ivec2 t = ivec2(gl_FragCoord.xy);
    ivec2 p = clamp(t * SCALE + (SCALE / 2 - 1), ivec2(0), ivec2(pc.size.xy) - 1);
    float z = depth_at(p);
    float d = z >= 1.0 ? 1e4 : view_depth(z);
    vec2 ndc = vec2((float(p.x) + 0.5) * pc.size.z * 2.0 - 1.0, 1.0 - (float(p.y) + 0.5) * pc.size.w * 2.0);
    vec3 rd = vec3(ndc.x * pc.proj.z, ndc.y * pc.proj.w, -1.0);   // the point at view depth 1
    float rl = length(rd);
    rd /= rl;
    float far = min(d * rl, pc.params.z);
    float jit = ign(vec2(t));
    vec3 sum = vec3(0.0);
    int count = int(pc.params.x);
    for (int i = 0; i < count && i < SHAFT_MAX; i++) {
        vec3 A = sh.pos_len[i].xyz;
        float len = sh.pos_len[i].w;
        vec3 ax = sh.axis[i].xyz;
        bool fire = sh.col_kind[i].w > 0.5;
        // Bounding sphere: the cone (and the glow above its apex), or the fire's ball.
        vec3 C = fire ? A + ax * (0.3 * len) : A + ax * (0.5 * len);
        float R = fire ? len : max(0.5 * len / COS_OUTER, (0.5 + GLOW * 2.0) * len);
        float b = dot(rd, C), c = dot(C, C) - R * R, disc = b * b - c;
        if (disc <= 0.0) continue;
        float s = sqrt(disc);
        float t0 = max(b - s, 0.0), t1 = min(b + s, far);
        if (t1 <= t0) continue;
        float dt = (t1 - t0) / float(STEPS), acc = 0.0;
        for (int k = 0; k < STEPS; k++) {
            vec3 v = rd * (t0 + (float(k) + jit) * dt) - A;
            float r2 = dot(v, v);
            float dens;
            if (fire) {
                float q = max(1.0 - r2 / (len * len), 0.0);
                dens = q * q * (0.6 + 0.4 * smoothstep(-0.3, 1.0, dot(v, ax) / len));
            } else {
                float g = GLOW * len;
                dens = cone(v, ax, len) + GLOW_GAIN * exp(-r2 / (g * g));
            }
            if (dens <= 0.0) continue;
            acc += dens * phase(dot(v, -rd) * inversesqrt(max(r2, 1e-6)));
        }
        sum += sh.col_kind[i].rgb * (acc * dt);
    }
    sum *= pc.params.y;
    sum = sum / (1.0 + sum);
    o = vec4(sum, d);
}

void comp() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float z = depth_at(p);
    float d = z >= 1.0 ? 1e4 : view_depth(z);
    vec2 h = (vec2(p) - float(SCALE / 2 - 1)) / float(SCALE);
    ivec2 c = ivec2(floor(h + 0.5)), mx = ivec2(pc.dir.zw) - 1;
    float tol = 0.05 * d + 0.05, wsum = 0.0, best = 1e30;
    vec3 sum = vec3(0.0), best_l = vec3(0.0);
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            ivec2 q = clamp(c + ivec2(i, j), ivec2(0), mx);
            vec4 tx = texelFetch(shaft_tex, q, 0);
            float dd = abs(tx.a - d);
            vec2 f = abs(vec2(c + ivec2(i, j)) - h);
            float w = max(1.5 - f.x, 0.0) * max(1.5 - f.y, 0.0) * max(1.0 - dd / tol, 0.0);
            sum += tx.rgb * w;
            wsum += w;
            if (dd < best) {
                best = dd;
                best_l = tx.rgb;
            }
        }
    vec3 L = wsum > 1e-4 ? sum / wsum : best_l;
    if (max(L.r, max(L.g, L.b)) < 1.0 / 1024.0) discard;
    o = vec4(L + (ign(vec2(p)) - 0.5) / 255.0, 0.0);
}

void main() {
    if (MODE == 0) scatter();
    else comp();
}
