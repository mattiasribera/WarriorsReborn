#version 450
#extension GL_GOOGLE_include_directive : require
// Wet ground (--rain-hd-wet, renderer_rain_wet), chosen by the specialization constant MODE:
// 0: trace at the reduced size (pc.dir.x internal pixels per texel): every upward-facing, flat pixel
//    mirrors its view ray about world up and marches it through the depth in screen space; what it
//    hits (the colour reduced to this size, binding 1) is kept by its brightness and weighted by
//    fresnel and fades. Out: rgb the reflection (times the wet mask), a the wet mask.
// 1: blur of mode 0's output (binding 1), mostly vertical: the long, soft streaks of wet asphalt.
//    Only wet texels contribute; the mask (alpha) stays this texel's own.
// 2: composite at the internal size (binding 1 bilinear) with blend ONE / ONE_MINUS_SRC_ALPHA:
//    dst x (1 - darken x wet) + reflection. Dry pixels are discarded.
// Push constants (ssao_common.glsl): proj, size as for SSAO; params: reflection gain, darken, reach
// (view units), 0; dir: internal pixels per reduced texel, reduced width, height, 0; cam[0].xyz:
// world up in view space (x right, y up, z towards the viewer).
// Characters (the rain's character mask, ssao_common.glsl) never get wet: modes 0 and 2 skip them.
#define CHAR_MASK
#include "ssao_common.glsl"
layout(constant_id = 0) const int MODE = 0;
layout(set = 0, binding = 1) uniform sampler2D tex;
layout(location = 0) out vec4 o;
#ifdef RAIN_COVER   // --rain-hd-cover: covered ground stays dry (mode 0, rain_cover.glsl)
#define COVER_SET 1
#include "rain_cover.glsl"
#endif

const int STEPS = 20, REFINE = 4;
// Wet ground is flat over about this many view units (10 cm) around a pixel: the curved tops of
// characters (shoulders, heads) and the rounded tops of props are not, and stay dry.
const float FLAT_REACH = 0.1;

// Internal pixel coordinates (continuous) of a view position in front of the camera.
vec2 to_pix(vec3 v) {
    float d = -v.z;
    vec2 ndc = vec2(v.x / (d * pc.proj.z), v.y / (d * pc.proj.w));
    return vec2((ndc.x * 0.5 + 0.5) * pc.size.x, (0.5 - 0.5 * ndc.y) * pc.size.y);
}
// Interleaved gradient noise: the march's start offset per texel (the blur hides the pattern).
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

void trace() {
    o = vec4(0.0);
    ivec2 mx = ivec2(pc.size.xy) - 1, p = min(ivec2(gl_FragCoord.xy * pc.dir.x), mx);
    if (depth_at(p) >= 1.0 || is_char(p)) return;
    vec3 P = view_pos(p);
    // The normal from the depths two pixels away (as the splashes): upward-facing only.
    vec3 n = surface_normal(p, P);
    if (dot(n, n) == 0.0) return;
    vec3 U = pc.cam[0].xyz;
    float wet = smoothstep(0.8, 0.92, dot(n, U));
    if (wet <= 0.0) return;
    // And flat (the splashes' test, at FLAT_REACH): on each axis the nearer of the two sides counts,
    // so a top curving away on both sides is dry while the floor at a wall's foot or a kerb (one
    // side off the plane) stays wet.
    float px = 2.0 * -P.z * pc.proj.w * pc.size.w;   // view units per internal pixel here
    int k = int(clamp(FLAT_REACH / px, 4.0, 64.0));
    vec4 off = surface_offsets(p, P, n, k);
    wet *= surface_flat(max(min(off.x, off.y), min(off.z, off.w)), px, k);
#ifdef RAIN_COVER
    wet *= rain_open(P);
#endif
    if (wet <= 0.0) return;
    o.a = wet;
    // The mirrored ray (about world up: the ground plane, not its bumps), Schlick fresnel of water.
    vec3 V = normalize(P), R = reflect(V, U);
    float F = 0.04 + 0.96 * pow(1.0 - clamp(dot(-V, U), 0.0, 1.0), 5.0);
    float face = 1.0 - smoothstep(0.0, 0.6, R.z);   // rays towards the camera leave what the screen shows
    if (face <= 0.0) return;
    float L = pc.params.z;
    if (R.z > 0.0) L = min(L, 0.95 * (-0.05 - P.z) / R.z);   // stay in front of the camera
    if (L <= 0.0) return;
    vec3 E = P + R * L;
    vec2 s0 = to_pix(P), dS = to_pix(E) - s0;
    // Clip the segment to the screen.
    float tmax = 1.0;
    if (dS.x > 1e-4) tmax = min(tmax, (pc.size.x - 1.0 - s0.x) / dS.x);
    else if (dS.x < -1e-4) tmax = min(tmax, -s0.x / dS.x);
    if (dS.y > 1e-4) tmax = min(tmax, (pc.size.y - 1.0 - s0.y) / dS.y);
    else if (dS.y < -1e-4) tmax = min(tmax, -s0.y / dS.y);
    float len = length(dS);
    if (tmax * len < 2.0 * pc.dir.x) return;
    float k0 = 1.0 / -P.z, k1 = 1.0 / -E.z;   // 1 / depth is linear on the screen
    float t0 = 1.5 * pc.dir.x / len, j = ign(gl_FragCoord.xy), prev = t0, hit = -1.0;
    for (int i = 0; i < STEPS; i++) {
        float f = (float(i) + j) / float(STEPS);
        float t = t0 + (tmax - t0) * f * sqrt(f);   // denser near the surface
        float z = depth_at(ivec2(s0 + dS * t));
        if (z < 1.0) {
            float rd = 1.0 / mix(k0, k1, t), sd = view_depth(z);
            if (rd > sd && rd - sd < 0.2 + 0.06 * sd) {   // behind the surface there, within its thickness
                hit = t;
                break;
            }
        }
        prev = t;
    }
    if (hit < 0.0) return;
    float lo = prev, hi = hit;
    for (int i = 0; i < REFINE; i++) {
        float t = 0.5 * (lo + hi), z = depth_at(ivec2(s0 + dS * t));
        if (z < 1.0 && 1.0 / mix(k0, k1, t) > view_depth(z)) hi = t;
        else lo = t;
    }
    vec2 uv = (s0 + dS * hi) * pc.size.zw;
    vec3 c = textureLod(tex, uv, 0.0).rgb;
    float bright = smoothstep(0.12, 0.7, dot(c, vec3(0.299, 0.587, 0.114)));   // the bright parts
    vec2 e = min(uv, 1.0 - uv);
    float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
    float far = 1.0 - smoothstep(0.5, 1.0, hi);   // towards the end of the ray
    o.rgb = c * (bright * F * pc.params.x * edge * far * face * wet);
}

void blur() {
    ivec2 p = ivec2(gl_FragCoord.xy), mx = ivec2(pc.dir.yz) - 1;
    vec4 c0 = texelFetch(tex, p, 0);
    if (c0.a <= 0.0) {
        o = vec4(0.0);
        return;
    }
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -6; y <= 6; y++)
        for (int x = -1; x <= 1; x++) {
            vec4 t = texelFetch(tex, clamp(p + ivec2(x, y), ivec2(0), mx), 0);
            float w = exp(-float(y * y) / 18.0) * (x == 0 ? 1.0 : 0.6);
            sum += t.rgb * w;
            wsum += t.a * w;
        }
    o = vec4(sum / max(wsum, 1e-4) * c0.a, c0.a);
}

void comp() {
    vec4 r = texture(tex, gl_FragCoord.xy / (pc.dir.x * pc.dir.yz));
    if (r.a <= 1e-3 || is_char(ivec2(gl_FragCoord.xy))) discard;
    // Texels traced on a character are dry (mode 0): next to one they are left out of the bilinear
    // filter, so the ground stays as wet as its own texels up to the silhouette (no dry rim).
    vec2 f = gl_FragCoord.xy / pc.dir.x - 0.5;
    ivec2 b = ivec2(floor(f)), tmx = ivec2(pc.dir.yz) - 1, mx = ivec2(pc.size.xy) - 1;
    vec2 fr = f - vec2(b);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    bool skipped = false;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            ivec2 t = clamp(b + ivec2(i, j), ivec2(0), tmx);
            if (is_char(min(ivec2((vec2(t) + 0.5) * pc.dir.x), mx))) {
                skipped = true;
                continue;
            }
            float w = (i == 1 ? fr.x : 1.0 - fr.x) * (j == 1 ? fr.y : 1.0 - fr.y);
            sum += texelFetch(tex, t, 0) * w;
            wsum += w;
        }
    if (skipped) {
        if (wsum <= 1e-4) discard;
        r = sum / wsum;
        if (r.a <= 1e-3) discard;
    }
    o = vec4(r.rgb, r.a * pc.params.y);
}

void main() {
    if (MODE == 1)
        blur();
    else if (MODE == 2)
        comp();
    else
        trace();
}
