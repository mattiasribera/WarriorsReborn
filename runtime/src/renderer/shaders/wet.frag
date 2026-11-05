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
// Puddles (--rain-hd-puddles) when pc.dir.w > 0 (their strength; 0: every mode exactly as without
// them): params.w the time (s), cam[1..3].xyz the camera's right, up and back axes in the world
// (z up) with the eye in cam[1..3].w. Mode 0: world-anchored patches of the wet ground (value
// noise on world x, y; with the cover map also low spots, ground below its surroundings) mirror the
// view about a normal tilted by rings of landing drops and reflect more than the bright parts; their
// share goes out in alpha above 1 (a = wet + puddle x strength). Mode 1 blurs them less, mode 2
// darkens them more.
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

// ---- Puddles ----
// Integer hash (as ssao_comp.frag): three values in [0, 1).
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
// Smooth value noise in [0, 1] on the world's ground plane.
float vnoise(vec2 x) {
    vec2 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    uvec2 c = uvec2(ivec2(i));
    float a = hash3(uvec3(c, 0x7F4A7C15u)).x, b = hash3(uvec3(c + uvec2(1, 0), 0x7F4A7C15u)).x;
    float d = hash3(uvec3(c + uvec2(0, 1), 0x7F4A7C15u)).x, e = hash3(uvec3(c + uvec2(1, 1), 0x7F4A7C15u)).x;
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}
// World position (z up) of view position P.
vec3 world_pos(vec3 P) {
    return vec3(pc.cam[1].w, pc.cam[2].w, pc.cam[3].w) + P.x * pc.cam[1].xyz + P.y * pc.cam[2].xyz + P.z * pc.cam[3].xyz;
}
// A world direction in view space.
vec3 view_dir(vec3 w) { return vec3(dot(w, pc.cam[1].xyz), dot(w, pc.cam[2].xyz), dot(w, pc.cam[3].xyz)); }
// Ripples: a grid of PUD_CELL world cells, one drop per cell and PUD_PERIOD seconds at a random place
// and phase; its ring widens to 0.4 cells and fades. Returns the slope of the water surface (world
// x, y) at w: the rings' height gradient.
const float PUD_CELL = 0.35, PUD_PERIOD = 0.9, PUD_RING = 0.025;
vec2 ripple_slope(vec2 w, float t) {
    vec2 g = vec2(0.0);
    ivec2 base = ivec2(floor(w / PUD_CELL - 0.5));
    for (int j = 0; j < 4; j++) {
        ivec2 c = base + ivec2(j & 1, j >> 1);
        uvec2 uc = uvec2(c);
        float ph = t / PUD_PERIOD + hash3(uvec3(uc, 0x2545F491u)).x;
        float ev = floor(ph), a = ph - ev;
        vec3 h = hash3(uvec3(uc, uint(int(ev))));
        vec2 d = w - (vec2(c) + h.xy) * PUD_CELL;
        float r = length(d), x = (r - 0.4 * PUD_CELL * a) / PUD_RING;
        g += (-2.0 * x * exp(-x * x) * (1.0 - a) * (1.0 - a)) * d / max(r, 1e-3);
    }
    return g;
}
#ifdef RAIN_COVER
const float RAIN_COVER_UNITS = 48.0;   // the map's width in world units (rain.h RAIN_COVER_RANGE)
// Low spots: how far the cover map's surface lies above P around it (four points 0.6 units away,
// each counted up to 0.25 units: kerbs, gutters, the foot of walls), 0-1.
float rain_low(vec3 P) {
    if (cover.k.x < 0.5) return 0.0;
    vec4 Q = vec4(P, 1.0);
    vec3 m = vec3(dot(cover.row[0], Q), dot(cover.row[1], Q), dot(cover.row[2], Q));
    if (m.x <= 0.02 || m.y <= 0.02 || m.x >= 0.98 || m.y >= 0.98) return 0.0;
    float s = 0.6 / RAIN_COVER_UNITS, sum = 0.0;
    const vec2 off[4] = vec2[4](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
    for (int i = 0; i < 4; i++) {
        float g = textureLod(cover_map, m.xy + off[i] * s, 0.0).r;
        if (g < 0.99998) sum += clamp((m.z - g) * cover.k.y, 0.0, 0.25);
    }
    return clamp(sum / 0.5, 0.0, 1.0);
}
#endif
// The puddle at world point W (view position P) on wet ground: 0 dry asphalt, 1 standing water.
float puddle(vec3 W, vec3 P) {
    vec2 g = W.xy;
    float m = 0.62 * vnoise(g * 0.42) + 0.38 * vnoise(g * 1.15 + vec2(17.3, 5.1));
#ifdef RAIN_COVER
    m += 0.22 * rain_low(P);
#endif
    return smoothstep(0.55, 0.62, m);
}
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
    // Puddles: their share, and the water's normal tilted by the ripples (near the camera only).
    float pud = 0.0;
    vec3 N = U;
    if (pc.dir.w > 0.0) {
        vec3 W = world_pos(P);
        pud = puddle(W, P) * wet;
        if (pud > 0.0) {
            vec2 sl = ripple_slope(W.xy, pc.params.w) * (0.3 * (1.0 - smoothstep(8.0, 16.0, -P.z)));
            N = normalize(mix(U, normalize(view_dir(vec3(-sl, 1.0))), pud));
            o.a = wet + pud * pc.dir.w;
        }
    }
    // The mirrored ray (about world up: the ground plane, not its bumps), Schlick fresnel of water.
    vec3 V = normalize(P), R = reflect(V, N);
    float F = 0.04 + 0.96 * pow(1.0 - clamp(dot(-V, N), 0.0, 1.0), 5.0);
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
    if (pud > 0.0) bright = mix(bright, 1.0, 0.7 * pud);   // standing water mirrors the rest too
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
    if (pc.dir.w > 0.0) {   // puddles: alpha above 1 in them, blurred less there
        float sig = 18.0 * (1.0 - 0.85 * clamp(c0.a - 1.0, 0.0, 1.0));
        for (int y = -6; y <= 6; y++)
            for (int x = -1; x <= 1; x++) {
                vec4 t = texelFetch(tex, clamp(p + ivec2(x, y), ivec2(0), mx), 0);
                float w = exp(-float(y * y) / sig) * (x == 0 ? 1.0 : 0.6);
                sum += t.rgb * w;
                wsum += min(t.a, 1.0) * w;
            }
        o = vec4(sum / max(wsum, 1e-4) * min(c0.a, 1.0), c0.a);
        return;
    }
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
    if (pc.dir.w > 0.0)   // puddles (alpha above 1) darker still
        o = vec4(r.rgb, min((min(r.a, 1.0) + 0.6 * max(r.a - 1.0, 0.0)) * pc.params.y, 0.95));
    else
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
