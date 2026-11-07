#version 450
#extension GL_GOOGLE_include_directive : require
// --lamp-lights (renderer_lamp_lights): the lamp halos' point lights added to the opaque world,
// chosen by the specialization constant MODE:
// 0: the light at the working size (ssao_work_size; SCALE internal pixels per texel, texel t holds
//    internal pixel SCALE t + SCALE / 2 - 1) into an R16G16B16A16 image: rgb the light L, a the
//    view depth. Per texel the view position and normal are rebuilt from the depth
//    (ssao_common.glsl), and every light within its radius adds colour x intensity x falloff
//    (lamp_lights.h lamp_falloff) x a wrapped Lambert term (the title's vertex lighting is soft,
//    and a normal from depth is noisy); the first SHADOWED lights (the nearest, highest priority)
//    are tested with a short screen-space ray towards the light; the sum goes through a soft knee
//    (L / (1 + L / 2): overlapping lamps do not wash out). Characters (CHARS: the character
//    mask) get pc.params.y times as much. Pixels of the bloom light mask (neon, fire, additive
//    glow: binding 3) keep only light_keep of it, so emissive pixels do not double up.
// 1: the composite at the internal size: the four nearest texels, bilinear times how close their
//    view depth is to this pixel's (as the ssao composite), drawn with blend DST_COLOR / ONE:
//    dst x (1 + L). The far plane and unlit pixels are discarded (exact colors).
#include "ssao_common.glsl"
layout(constant_id = 0) const int MODE = 0;
layout(constant_id = 1) const int SCALE = 2;
layout(constant_id = 2) const int CHARS = 0;   // the character mask is bound (binding 2)
layout(set = 0, binding = 1) uniform sampler2D lamp_tex;          // MODE 1: the light (mode 0's image)
layout(set = 0, binding = 2) uniform sampler2D char_depth_tex;    // CHARS: depth of the characters alone
layout(set = 0, binding = 3) uniform sampler2D light_mask_tex;    // the scene's alpha: the bloom light mask
#define LAMP_MAX 32
layout(set = 0, binding = 4) uniform Lights {
    vec4 pos_r[LAMP_MAX];   // right-handed view position, radius (view units)
    vec4 col[LAMP_MAX];     // colour x intensity x strength
} lights;
layout(location = 0) out vec4 o;
// pc.params: lights in use, character gain, falloff k (lamp_lights.h LAMP_FALLOFF_K), Lambert wrap.
// pc.dir: only characters (RLampLights.no_world), -, working width, height.
const int SHADOWED = 4;
const int SHADOW_STEPS = 6;

float falloff(float d, float r) {   // lamp_lights.h lamp_falloff
    float x2 = d * d / (r * r);
    if (x2 >= 1.0) return 0.0;
    float a = 1.0 - x2;
    return a * a / (1.0 + pc.params.z * x2);
}
bool on_char(ivec2 p) {
    if (CHARS == 0) return false;
    ivec2 q = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float c = texelFetch(char_depth_tex, q, 0).r;
    if (c >= 1.0) return false;
    float dc = view_depth(c), ds = view_depth(depth_at(q));
    return abs(dc - ds) <= 0.02 + 0.01 * ds;
}
// Internal pixel of a right-handed view position (in front of the camera).
vec2 to_pixel(vec3 v) {
    float d = max(-v.z, 1e-4);
    vec2 ndc = vec2(v.x / (d * pc.proj.z), v.y / (d * pc.proj.w));
    return vec2((ndc.x * 0.5 + 0.5) * pc.size.x, (0.5 - ndc.y * 0.5) * pc.size.y);
}
// How much of the light at L reaches P: a few steps along the segment, occluded where the scene's
// depth lies in front of the ray (within a thickness, so a surface far in front is not a blocker).
float shadow(vec3 P, vec3 L, float d) {
    float len = min(0.8 * d, 2.5);   // short of the lamp itself (its shade would shadow everything near it)
    vec3 dir = (L - P) / d;
    float lit = 1.0;
    for (int i = 1; i <= SHADOW_STEPS; i++) {
        vec3 q = P + dir * (len * (float(i) - 0.5) / float(SHADOW_STEPS));
        if (q.z > -0.05) break;
        vec2 px = to_pixel(q);
        if (any(lessThan(px, vec2(0.0))) || any(greaterThanEqual(px, pc.size.xy))) break;
        float sd = view_depth(depth_at(ivec2(px))), rd = -q.z;
        float gap = rd - sd;
        if (gap > 0.04 + 0.02 * rd && gap < 1.5) lit -= 0.3;
    }
    return max(lit, 0.0);
}

void light() {
    ivec2 t = ivec2(gl_FragCoord.xy);
    ivec2 p = t * SCALE + (SCALE / 2 - 1);
    p = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    float z = depth_at(p);
    if (z >= 1.0) {
        o = vec4(0.0, 0.0, 0.0, 1e4);
        return;
    }
    vec3 P = view_pos(p);
    // pc.dir.x = 1 (RLampLights.no_world): only characters are lit here, the rest per pixel by the
    // world-lit draws.
    bool chr = on_char(p);
    if (pc.dir.x > 0.5 && !chr) {
        o = vec4(0.0, 0.0, 0.0, -P.z);
        return;
    }
    vec3 n = surface_normal(p, P);
    int count = int(pc.params.x);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < count && i < LAMP_MAX; i++) {
        vec4 lr = lights.pos_r[i];
        vec3 v = lr.xyz - P;
        float d2 = dot(v, v);
        if (d2 >= lr.w * lr.w) continue;
        float d = sqrt(d2) + 1e-4;
        float ndl = dot(n, n) > 0.0 ? dot(n, v / d) : 0.5;
        float lam = clamp((ndl + pc.params.w) / (1.0 + pc.params.w), 0.0, 1.0);
        if (lam <= 0.0) continue;
        float f = falloff(d, lr.w) * lam;
        if (i < SHADOWED) f *= shadow(P, lr.xyz, d);
        sum += lights.col[i].rgb * f;
    }
    sum = sum / (1.0 + 0.5 * sum);   // soft knee: many overlapping lamps brighten, they do not wash out
    if (chr) sum *= pc.params.y;
    sum *= 1.0 - smoothstep(0.0, 0.5, texelFetch(light_mask_tex, p, 0).a);
    o = vec4(sum, -P.z);
}

void comp() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float z = depth_at(p);
    if (z >= 1.0) discard;
    float d = view_depth(z);
    vec2 h = (vec2(p) - float(SCALE / 2 - 1)) / float(SCALE);
    ivec2 b = ivec2(floor(h)), mx = ivec2(pc.dir.zw) - 1;
    vec2 fr = h - vec2(b);
    float tol = 0.03 * d + 0.01, wsum = 0.0, best = 1e30;
    vec3 sum = vec3(0.0), best_l = vec3(0.0);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec4 tx = texelFetch(lamp_tex, clamp(b + ivec2(i, j), ivec2(0), mx), 0);
            float dd = abs(tx.a - d);
            float w = (i == 1 ? fr.x : 1.0 - fr.x) * (j == 1 ? fr.y : 1.0 - fr.y) + 1e-3;
            w *= max(1.0 - dd / tol, 0.0);
            sum += tx.rgb * w;
            wsum += w;
            if (dd < best) {
                best = dd;
                best_l = tx.rgb;
            }
        }
    vec3 L = wsum > 1e-4 ? sum / wsum : best_l;
    if (max(L.r, max(L.g, L.b)) < 1.0 / 512.0) discard;
    o = vec4(L, 1.0);
}

void main() {
    if (MODE == 0) light();
    else comp();
}
