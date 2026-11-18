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
// Light streaks (--wet-light-streaks) when pc.cam[0].w > 0 (their gain; 0: every mode exactly as
// without them): binding 3 the lights (Streaks, renderer.h RRainWet.light), cam[1..3] the camera in
// the world as for the puddles. Mode 0 adds light_streaks (the lights mirrored about each wet
// pixel's own height) by max with the traced reflection, and where the trace finds nothing the fog
// colour (the miss fallback); mode 1's streaks lengthen with the view distance.
// Materials (--materials) when sl.mat.w > 0 (their strength; 0: every mode exactly as without them):
// binding 4 the level's top-down material map (renderer.h RMaterialMap), cam[1..3] the camera in the
// world. Mode 0 looks up the surface material under each wet pixel (by world x, y and its height)
// and scales the reflection, the darkening (the mask), the puddles and the streaks' gloss and
// wetness by that material's look (MAT_LOOK).
// Neon reflections (--neon-reflections) when sl.damp.y > 0 (with the streaks; 0: every mode exactly
// as without them): lights with col.w > 0 are signs, whose streak widens by their extent (col.w, view
// units) seen from the pixel, and mode 0 counts a saturated hit (red and blue tubes) up to its
// strongest channel instead of its luma alone.
// Damp streets (--damp-streets) when sl.damp.x > 0 (0: every mode exactly as without them; always
// with materials): a dry night street. Mode 0 keeps the wet mask only where the material map places
// asphalt (or a little on concrete) as the top surface (DAMP_LOOK; nothing under a second, higher
// surface or where the map has none), reflects only the brightest parts, adds a soft sheen where lamps light the ground, turns the puddles into a
// few damp patches gathered along kerbs without ripples and softens the streaks; mode 1 blurs less.
// Stochastic screen-space reflections (--reflections sssr) when the specialization constant SSSR is 1
// (0: every mode exactly as without them, sssr.glsl): mode 0 samples the
// reflected ray from a GGX distribution of the surface's roughness (its material's gloss; standing
// water nearly a mirror) and marches it through the depth hierarchy (binding 6, sssr_hiz.frag) instead
// of the 20 steps; mode 3 (new) accumulates mode 0's output over frames (binding 7 the previous
// frame's result, reprojected by the camera of the previous frame, clamped to this frame's
// neighbourhood); mode 1 blurs SSSR_BLUR as far.
// Stage 2 of SSSR when sl.glass.x > 0 (its strength; 0: SSSR exactly as without
// it): binding 8 the surface pass's material texels (sssr_surf.frag: face normal, shine, gloss) and
// binding 9 their depth. Mode 0 first traces glossy materials seen there (glass_at: glass, shop
// windows, car paint, chrome) with their own normal and a roughness from their gloss, weighted by a
// Schlick Fresnel term; their texels carry alpha GLASS_A + the share of the colour the reflection
// replaces, and modes 1-3 keep them apart from the ground's. In a dry frame (sl.glass.y 1) only the
// glass reflects. The ground's roughness also follows its own texture's gloss there. sl.sssr.w rays
// per texel (1 or 2) are averaged.
#define CHAR_MASK
#include "ssao_common.glsl"
layout(constant_id = 0) const int MODE = 0;
layout(constant_id = 1) const int SSSR = 0;
layout(set = 0, binding = 1) uniform sampler2D tex;
layout(location = 0) out vec4 o;
#ifdef RAIN_COVER   // --rain-hd-cover: covered ground stays dry (mode 0, rain_cover.glsl)
#define COVER_SET 1
#include "rain_cover.glsl"
#endif

const int STEPS = 20, REFINE = 4;

// ---- Light streaks (--wet-light-streaks) ----
const int WET_LIGHTS = 32;   // renderer.h RENDERER_WET_LIGHTS
layout(set = 0, binding = 3, std140) uniform Streaks {
    vec4 k;                    // lights, lobe width along the view (slope units), falloff (light radii), miss fallback
    vec4 fog;                  // rgb: what a ray that finds nothing reflects
    vec4 pos_r[WET_LIGHTS];    // view position, radius (view units)
    vec4 col[WET_LIGHTS];      // colour x intensity
    vec4 mat;                  // materials: map x0, y0 (world), 1 / cell, strength (0: none)
    vec4 matz;                 // map z0, zstep, width, height (texels)
    vec4 damp;                 // damp streets: x on (1); y neon reflections' share (0: none), 0, 0
    vec4 wall;                 // the level's wall map (light_walls.glsl): x0, y0, 1 / cell, 1 = loaded (0: none)
    vec4 wallz;                // z0, zstep, the receiver's own wall (world units)
    vec4 sssr;                 // --reflections sssr: frame number (noise), march budget, history valid (1), 0
    vec4 prev[3];              // the previous frame's camera axes in the world (right, up, back) with its eye in w
    vec4 prevp;                // its 1 / px, 1 / py, 0, 0
    vec4 glass;                // stage 2: glass strength (0: none), 1 = a dry frame (only glass), 1 = the surface pass ran, 0
} sl;
bool streaks_on() { return pc.cam[0].w > 0.0; }
// Occluded streaks: binding 5 the level's wall map (renderer_lamp_wall_map; a 1x1 empty image stands
// in when sl.wall.w is 0).
layout(set = 0, binding = 5) uniform usampler2D wall_map;
#define LW_MAP wall_map
#include "light_walls.glsl"

// ---- Stochastic screen-space reflections (--reflections sssr) ----
layout(set = 0, binding = 6) uniform sampler2D sssr_hiz;   // the min-depth hierarchy (1x1 stand-in when classic)
layout(set = 0, binding = 7) uniform sampler2D hist;       // mode 3: the previous frame's accumulated reflection
layout(set = 0, binding = 8) uniform sampler2D surf_col;   // stage 2: the surface pass's normals and materials (stand-in without)
layout(set = 0, binding = 9) uniform sampler2D surf_depth; // and their depth
#include "sssr.glsl"

// ---- Materials (--materials) ----
layout(set = 0, binding = 4) uniform usampler2D mat_map;
// The look per material class (src/materials.h MAT_*): x reflection, y darkening (the wet mask),
// z puddles (above 1 more, 0 none), w the streaks' gloss; MAT_WET the streaks' wetness. Unknown
// (no surface near the pixel's height) is 1: the look without materials.
const vec4 MAT_LOOK[9] = vec4[9](vec4(1.0, 1.0, 1.0, 1.0),     // unknown
                                 vec4(1.3, 0.85, 1.3, 1.25),   // asphalt: glossy, the most puddles
                                 vec4(1.0, 0.8, 0.8, 1.0),     // concrete
                                 vec4(0.3, 1.0, 0.9, 0.35),    // dirt, gravel: dark matte mud
                                 vec4(0.08, 0.25, 0.0, 0.25),  // grass: barely wet
                                 vec4(0.6, 0.7, 0.35, 0.6),    // wood
                                 vec4(1.3, 0.5, 0.2, 1.6),     // metal
                                 vec4(0.7, 0.8, 0.4, 0.7),     // brick
                                 vec4(1.4, 0.4, 0.0, 2.0));    // glass
const float MAT_WET[9] = float[9](1.0, 1.0, 0.9, 0.35, 0.08, 0.55, 1.0, 0.7, 1.0);
const float MAT_TOL = 1.0;   // world units: how far the collision surface may lie from the pixel
// The class of the map texel t whose surface lies nearest the height z (within the tolerance), 0
// none; out-of-map texels are unknown.
uint mat_class(ivec2 t, float z) {
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(sl.matz.zw)))) return 0u;
    uvec2 v = texelFetch(mat_map, t, 0).rg;
    uint c = 0u;
    float best = MAT_TOL + sl.matz.y;
    for (int l = 0; l < 2; l++) {
        uint h = v[l] & 0xFFFu;
        if (h == 0u) continue;
        float d = abs(z - (sl.matz.x + float(h - 1u) * sl.matz.y));
        if (d <= best) {
            best = d;
            c = min(v[l] >> 12, 8u);
        }
    }
    return c;
}
// The look at world point W, bilinear over the four nearest texels and scaled by the strength:
// x..w as MAT_LOOK; *wet_out the streaks' wetness. Exactly 1 where every texel is unknown.
vec4 mat_look(vec3 W, out float wet_out) {
    vec2 g = (W.xy - sl.mat.xy) * sl.mat.z - 0.5;
    ivec2 b = ivec2(floor(g));
    vec2 f = g - vec2(b);
    vec4 d = vec4(0.0);
    float dw = 0.0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            uint c = mat_class(b + ivec2(i, j), W.z);
            if (c == 0u) continue;
            float w = (i == 1 ? f.x : 1.0 - f.x) * (j == 1 ? f.y : 1.0 - f.y);
            d += w * (MAT_LOOK[c] - 1.0);
            dw += w * (MAT_WET[c] - 1.0);
        }
    wet_out = max(1.0 + sl.mat.w * dw, 0.0);
    return max(1.0 + sl.mat.w * d, vec4(0.0));
}

// ---- Damp streets (--damp-streets) ----
bool damp_on() { return sl.damp.x > 0.0; }
// How damp each material class gets on a dry street (src/materials.h MAT_*): asphalt and tar fully,
// concrete a little, everything else (and unknown) not at all.
const float DAMP_LOOK[9] = float[9](0.0, 1.0, 0.35, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
// The dampness of map texel t at height z: its class's when the surface nearest z (within the
// tolerance, as mat_class) is the texel's top one, fading out from DAMP_Z_NEAR to DAMP_Z_FAR above or
// below it (the top of a crate or a bench is not the street; litter a few centimetres up is); 0
// without such a surface or when it is the second one (something solid lies over it: a roof, a
// walkway, a bridge).
const float DAMP_Z_NEAR = 0.12, DAMP_Z_FAR = 0.3;
float damp_texel(ivec2 t, float z) {
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(sl.matz.zw)))) return 0.0;
    uvec2 v = texelFetch(mat_map, t, 0).rg;
    int layer = -1;
    float best = MAT_TOL + sl.matz.y;
    for (int l = 0; l < 2; l++) {
        uint h = v[l] & 0xFFFu;
        if (h == 0u) continue;
        float d = abs(z - (sl.matz.x + float(h - 1u) * sl.matz.y));
        if (d <= best) {
            best = d;
            layer = l;
        }
    }
    return layer == 0 ? DAMP_LOOK[min(v[0] >> 12, 8u)] * (1.0 - smoothstep(DAMP_Z_NEAR, DAMP_Z_FAR, best)) : 0.0;
}
// How surely world point W lies on the map's own ground (a damp class's top surface): 1 within
// DAMP_ON_NEAR of the nearest of the four texels' heights, 0 from DAMP_ON_FAR (the bottom of a wall
// standing on it is at most that much of its face).
const float DAMP_ON_NEAR = 0.05, DAMP_ON_FAR = 0.1;
float damp_on_ground(vec3 W) {
    ivec2 b = ivec2(floor((W.xy - sl.mat.xy) * sl.mat.z - 0.5));
    float best = 1e9;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            ivec2 t = b + ivec2(i, j);
            if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(sl.matz.zw)))) continue;
            uint v = texelFetch(mat_map, t, 0).r, h = v & 0xFFFu;
            if (h == 0u || DAMP_LOOK[min(v >> 12, 8u)] <= 0.0) continue;
            best = min(best, abs(W.z - (sl.matz.x + float(h - 1u) * sl.matz.y)));
        }
    return 1.0 - smoothstep(DAMP_ON_NEAR, DAMP_ON_FAR, best);
}
// The dampness at world point W, bilinear over the four nearest texels (soft edges).
float damp_at(vec3 W) {
    vec2 g = (W.xy - sl.mat.xy) * sl.mat.z - 0.5;
    ivec2 b = ivec2(floor(g));
    vec2 f = g - vec2(b);
    return mix(mix(damp_texel(b, W.z), damp_texel(b + ivec2(1, 0), W.z), f.x),
               mix(damp_texel(b + ivec2(0, 1), W.z), damp_texel(b + ivec2(1, 1), W.z), f.x), f.y);
}
// Kerbs: how much of the ground DAMP_KERB_REACH around W (four points) has its top surface a little
// higher (DAMP_KERB_LOW-DAMP_KERB_HIGH world units: a kerb, a raised pavement, a step), 0-1. Water
// gathers there, so the damp patches do too.
const float DAMP_KERB_REACH = 0.7, DAMP_KERB_LOW = 0.04, DAMP_KERB_HIGH = 0.45;
float damp_kerb(vec3 W) {
    const vec2 off[4] = vec2[4](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
    float s = 0.0;
    for (int i = 0; i < 4; i++) {
        ivec2 t = ivec2(floor((W.xy + off[i] * DAMP_KERB_REACH - sl.mat.xy) * sl.mat.z));
        if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(sl.matz.zw)))) continue;
        uint h = texelFetch(mat_map, t, 0).r & 0xFFFu;
        if (h == 0u) continue;
        float dz = sl.matz.x + float(h - 1u) * sl.matz.y - W.z;
        s += smoothstep(DAMP_KERB_LOW, 0.1, dz) * (1.0 - smoothstep(DAMP_KERB_HIGH - 0.15, DAMP_KERB_HIGH, dz));
    }
    return clamp(s, 0.0, 1.0);
}
// Damp patches: far fewer than the rain's puddles in the open, more along kerbs. Streaks: DAMP_GLOSS
// of the material's gloss (their brightness), their lobe DAMP_ALONG as long along the view as the
// rain's and DAMP_ACROSS of that across it (short, wide, soft glints by the lamps, not a mirror's long
// lines).
// The trace keeps only hits brighter than DAMP_BRIGHT_FROM-TO luma (and DAMP_PATCH_MIRROR of the rest
// in damp patches, whose streaks are DAMP_PATCH_GLOSS tighter).
// The ground's tests are looser than the rain's (the material map has placed asphalt there, at this
// height: damp_texel): facing up DAMP_UP_FROM-TO, and the flatness allows steps of DAMP_FLAT_STEP
// (litter, kerb-side gutters, cracks: their edges cut blocky holes into the streaks before) plus
// DAMP_FLAT_DEPTH per view unit of distance (the depth's own steps far away) and DAMP_FLAT_SLACK
// times the rain's tolerance; dry texels on a damp row are filled (damp_fill, DAMP_FILL_DEPTH).
const float DAMP_FLAT_STEP = 0.08;
const float DAMP_UP_FROM = 0.6, DAMP_UP_TO = 0.85, DAMP_FLAT_DEPTH = 0.002, DAMP_FLAT_SLACK = 2.0, DAMP_FILL_DEPTH = 0.05;
const float DAMP_BRIGHT_FROM = 0.45, DAMP_BRIGHT_TO = 0.95, DAMP_PATCH_MIRROR = 0.1, DAMP_PATCH_GLOSS = 1.3;
const float DAMP_PATCH_BIAS = -0.28, DAMP_PATCH_KERB = 0.3, DAMP_GLOSS = 1.0, DAMP_ALONG = 0.6, DAMP_ACROSS = 0.55;
// Sheen: damp asphalt in a pool of lamp light (brighter than the ground around it, four texels
// DAMP_SHEEN_REACH away, by DAMP_SHEEN_FROM-TO luma) gets a soft gloss of DAMP_SHEEN of its own
// colour, in world-anchored patches; evenly lit or unlit asphalt only darkens, so the lit pools
// glint while the frame as a whole gets no brighter.
const float DAMP_SHEEN = 0.3, DAMP_SHEEN_FROM = 0.01, DAMP_SHEEN_TO = 0.08, DAMP_SHEEN_REACH = 24.0;
vec3 t_sheen = vec3(0.0);

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
// The puddle at world point W (view position P) on wet ground: 0 dry asphalt, 1 standing water;
// bias above 0 more of them (materials), below fewer.
float puddle(vec3 W, vec3 P, float bias) {
    vec2 g = W.xy;
    float m = 0.62 * vnoise(g * 0.42) + 0.38 * vnoise(g * 1.15 + vec2(17.3, 5.1));
#ifdef RAIN_COVER
    m += 0.22 * rain_low(P);
#endif
    return smoothstep(0.55, 0.62, m + bias);
}
// Wet ground is flat over about this many view units (10 cm) around a pixel: the curved tops of
// characters (shoulders, heads) and the rounded tops of props are not, and stay dry.
const float FLAT_REACH = 0.1;

// The lights mirrored in the wet ground at view position P: V the unit view direction (eye to P), N
// the surface's normal (world up U, or the puddle's rippled one). For every light above the plane
// through P (its own height: kerbs, steps and car roofs mirror about themselves), a lobe around the
// mirror direction of the half vector's slopes, wide along the view's direction on the ground (the
// long streak towards the camera) and narrow across it, times a Schlick fresnel and a smooth falloff
// over sl.k.z light radii. The lobe widens with the view distance (far streaks are longer) and by
// rough (a world-anchored variation, about 1).
// Per-pixel inputs a material pass can set: gloss 1 is wet asphalt, above 1 smoother (standing water:
// a tighter, brighter lobe), below 1 matter (dirt, grass: wider and dimmer); wetness 0-1 scales it.
// A lamp seen in water is far brighter than the frame's white (STREAK_HDR x its colour); the wet
// sheen's fresnel starts at STREAK_F0 (oily water on asphalt, above clean water's 0.04).
const float STREAK_HDR = 6.0, STREAK_F0 = 0.15;
// A sign (col.w > 0, half its extent) seen from the ground spreads its mirror image over about that
// extent over the distance: the half vector's slope over SIGN_SPREAD of it.
const float SIGN_SPREAD = 0.5;

// Occluded streaks: a light whose way down to the reflecting ground is
// blocked leaves no streak there. Two tests, both stable from frame to frame:
// - walls: a wall or closed door of the level's wall map between the ground point and the light
//   (light_walls.glsl, the same map as the lamp lights; open doors are gone from it; wall_open);
// - the screen: the light's own position, when it is on screen, covered by something nearer (a wall,
//   a prop, a person): OCC_TAPS depth taps on a ring of OCC_RADIUS view units round it, the share of
//   them that lie more than OCC_MARGIN (plus OCC_MARGIN_K of the distance) in front of the light fades
//   the streak smoothly (a light half behind a pole keeps half its streak; no popping). A light off
//   screen (above its top) has only the wall test.
// The cost cap: only the two strongest lights at the ground point are tested (the second when it
// adds at least OCC_SHARE of the first's light), only where they add more than OCC_MIN.
const float OCC_RADIUS = 0.35, OCC_MARGIN = 1.0, OCC_MARGIN_K = 0.03, OCC_SHARE = 0.25, OCC_MIN = 1.0 / 512.0;
const int OCC_TAPS = 9;
const vec2 OCC_RING[OCC_TAPS] = vec2[OCC_TAPS](vec2(0.0), vec2(1.0, 0.0), vec2(0.7071, 0.7071), vec2(0.0, 1.0), vec2(-0.7071, 0.7071),
                                              vec2(-1.0, 0.0), vec2(-0.7071, -0.7071), vec2(0.0, -1.0), vec2(0.7071, -0.7071));
// Internal pixel coordinates (continuous) of a view position in front of the camera.
vec2 to_pix(vec3 v) {
    float d = -v.z;
    vec2 ndc = vec2(v.x / (d * pc.proj.z), v.y / (d * pc.proj.w));
    return vec2((ndc.x * 0.5 + 0.5) * pc.size.x, (0.5 - 0.5 * ndc.y) * pc.size.y);
}
// Interleaved gradient noise: the march's start offset per texel (the blur hides the pattern).
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
// The walls between the ground point p and the light l (world): every map cell the way crosses, in
// order (a 2D DDA: a one-cell wall is never stepped over, as wall_light's even steps over long ways
// could), at most OCC_WALL_CELLS of them from the ground point (beyond: open); the receiver's own
// cells (wallz.z) and the light's own cell are not tested, and near the light (LW_NEAR) only walls
// reaching LW_ABOVE above it block (the lamp's own post or bracket), as for the lamp lights.
const int OCC_WALL_CELLS = 160;
float wall_open(vec3 p, vec3 l) {
    vec2 a = (p.xy - sl.wall.xy) * sl.wall.z, b = (l.xy - sl.wall.xy) * sl.wall.z, d = b - a;   // in cells
    float len = length(d), own = sl.wallz.z * sl.wall.z;
    if (len <= own) return 1.0;
    ivec2 size = textureSize(wall_map, 0), c = ivec2(floor(a)), ce = ivec2(floor(b)), st = ivec2(d.x < 0.0 ? -1 : 1, d.y < 0.0 ? -1 : 1);
    vec2 dir = d / len, inv = 1.0 / max(abs(dir), vec2(1e-6));
    vec2 tm = vec2(st.x > 0 ? float(c.x) + 1.0 - a.x : a.x - float(c.x), st.y > 0 ? float(c.y) + 1.0 - a.y : a.y - float(c.y)) * inv;
    float t = 0.0, horiz = len / sl.wall.z;
    for (int k = 0; k < OCC_WALL_CELLS; k++) {
        if (c == ce) return 1.0;   // the light's own cell
        float tn = min(min(tm.x, tm.y), len), mid = 0.5 * (t + tn);
        if (mid > own) {
            float s = mid / len, top;
            if (wall_at(mix(p, l, s), sl.wall, sl.wallz, size, top) && ((1.0 - s) * horiz >= LW_NEAR || top >= l.z + LW_ABOVE)) return 0.0;
        }
        t = tn;
        if (tm.x < tm.y) {
            tm.x += inv.x;
            c.x += st.x;
        } else {
            tm.y += inv.y;
            c.y += st.y;
        }
    }
    return 1.0;
}
// How much of light i reaches the ground at view position P: 1 open, 0 blocked.
float light_open(int i, vec3 P) {
    vec3 L = sl.pos_r[i].xyz;
    float v = 1.0;
    if (sl.wall.w > 0.0) v = wall_open(world_pos(P), world_pos(L));
    float d = -L.z;
    if (v <= 0.0 || d < 0.1) return v;
    vec2 c = to_pix(L);
    if (any(lessThan(c, vec2(0.0))) || any(greaterThanEqual(c, pc.size.xy))) return v;   // off screen: the walls alone
    float r = clamp(OCC_RADIUS / (d * pc.proj.w) * 0.5 * pc.size.y, 1.5, 12.0), lim = d - OCC_MARGIN - OCC_MARGIN_K * d;
    float hid = 0.0, n = 0.0;
    for (int k = 0; k < OCC_TAPS; k++) {
        vec2 q = c + OCC_RING[k] * r;
        if (any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, pc.size.xy))) continue;
        float z = depth_at(ivec2(q));
        n += 1.0;
        if (z < 1.0 && view_depth(z) < lim) hid += 1.0;
    }
    return v * (1.0 - smoothstep(0.1, 0.9, hid / n));
}

vec3 light_streaks(vec3 P, vec3 V, vec3 N, vec3 U, float gloss, float wetness, float rough) {
    int n = min(int(sl.k.x), WET_LIGHTS);
    if (n <= 0 || wetness <= 0.0) return vec3(0.0);
    float g = max(gloss, 0.05);
    float along = sl.k.y * rough * (1.0 + 1.5 * smoothstep(4.0, 40.0, -P.z)) / g, across = 0.22 * along;
    if (damp_on()) {   // damp streets: shorter along the view, wider across it (a soft glint, not a mirror)
        along *= DAMP_ALONG;
        across = DAMP_ACROSS * along;
    }
    vec3 T = V - dot(V, U) * U;   // the view's direction along the ground
    float tl = length(T);
    if (tl < 1e-4) return vec3(0.0);   // looking straight down: no streak direction (and no streak)
    T /= tl;
    vec3 B = cross(U, T);
    float ia = 1.0 / (along * along), ib = 1.0 / (across * across);
    vec3 sum = vec3(0.0);
    // The two strongest lights here (for the occlusion tests after the loop).
    vec3 c0 = vec3(0.0), c1 = vec3(0.0);
    float y0 = 0.0, y1 = 0.0;
    int i0 = -1, i1 = -1;
    for (int i = 0; i < n; i++) {
        vec3 D = sl.pos_r[i].xyz - P;
        if (dot(D, U) <= 0.05) continue;   // at or below this surface
        float dist = length(D), x = dist / (sl.k.z * sl.pos_r[i].w);
        if (x >= 1.0) continue;
        vec3 H = normalize(D / dist - V);
        float hn = dot(H, N);
        if (hn <= 1e-3) continue;
        float sx = dot(H, T) / hn, sy = dot(H, B) / hn;
        float ja = ia, jb = ib;
        if (sl.col[i].w > 0.0) {   // a sign: an extended light
            float e = SIGN_SPREAD * sl.col[i].w / dist;
            ja = 1.0 / (along * along + e * e);
            jb = 1.0 / (across * across + e * e);
        }
        float lobe = exp(-(sx * sx * ja + sy * sy * jb));
        float F = STREAK_F0 + (1.0 - STREAK_F0) * pow(1.0 - clamp(dot(-V, H), 0.0, 1.0), 5.0);
        float fall = 1.0 - x * x;
        vec3 c = sl.col[i].rgb * (lobe * F * fall * fall);
        sum += c;
        float y = c.r + c.g + c.b;
        if (y > y0) {
            c1 = c0, y1 = y0, i1 = i0;
            c0 = c, y0 = y, i0 = i;
        } else if (y > y1) {
            c1 = c, y1 = y, i1 = i;
        }
    }
    // Occlusion: what the blocked part of the strongest lights added comes off again (nothing when
    // every tested light is open: the sum is then exactly the one above).
    if (i0 >= 0 && y0 > OCC_MIN) {
        sum -= c0 * (1.0 - light_open(i0, P));
        if (i1 >= 0 && y1 > OCC_MIN && y1 >= OCC_SHARE * y0) sum -= c1 * (1.0 - light_open(i1, P));
        sum = max(sum, vec3(0.0));
    }
    return sum * (STREAK_HDR * pc.cam[0].w * wetness * min(g, 1.5));
}

// The trace's pixel for finish (set before its first call).
vec3 t_P, t_V, t_N, t_U;
float t_F, t_wet, t_pud, t_rough;
// The material's reflection, streak gloss and wetness (1 without materials).
float t_refl = 1.0, t_gloss = 1.0, t_mwet = 1.0;
ivec2 t_p = ivec2(0);   // SSSR: the trace's internal pixel
// The trace's result: ssr its reflection (already weighted), cov how much of it the trace found
// (0: nothing, a miss). Without light streaks exactly that reflection; with them the fog colour
// fills what the trace did not find (standing water most) and the lights are added by max, plus a
// share of the smaller: a lamp the trace also finds on screen is not counted twice.
void finish(vec3 ssr, float cov) {
    if (!streaks_on()) {
        o.rgb = ssr * t_refl;
        if (damp_on()) o.rgb += t_sheen;
        return;
    }
    vec3 fb = sl.fog.rgb * (t_F * pc.params.x * sl.k.w * (0.2 * t_wet + 0.8 * t_pud) * (1.0 - cov));
    vec3 r = (ssr + fb) * t_refl;
    // Standing water: a tighter, brighter lobe (damp patches: only a little).
    vec3 l = light_streaks(t_P, t_V, t_N, t_U, mix(1.0, damp_on() ? DAMP_PATCH_GLOSS : 2.5, t_pud / max(t_wet, 1e-3)) * t_gloss, t_wet * t_mwet, t_rough);
    o.rgb = max(r, l) + 0.35 * min(r, l);
    if (damp_on()) o.rgb += t_sheen;
}

// How much of a hit's colour c the wet ground reflects (pud: the puddle's share there).
float hit_bright(vec3 c, float pud) {
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    if (streaks_on() && sl.damp.y > 0.0) {   // neon reflections: a saturated hit counts up to its strongest channel
        float m = max(c.r, max(c.g, c.b)), sat = m > 0.0 ? 1.0 - min(c.r, min(c.g, c.b)) / m : 0.0;
        luma = mix(luma, max(luma, m), sl.damp.y * sat);
    }
    // The bright parts (damp streets: only the very brightest, lamps, neon, fires; no mirror of the
    // scene on dry ground).
    float bright = damp_on() ? smoothstep(DAMP_BRIGHT_FROM, DAMP_BRIGHT_TO, luma) : smoothstep(0.12, 0.7, luma);
    if (pud > 0.0) bright = mix(bright, 1.0, (damp_on() ? DAMP_PATCH_MIRROR : 0.7) * pud);   // standing water mirrors the rest too
    return bright;
}

// SSSR's trace of wet view position P (V the unit view direction, N the surface's normal, F its
// fresnel, wet the mask, pud the puddle's share): sl.sssr.w rays (1 or 2) whose reflected direction is
// sampled from the GGX distribution of the ground's roughness (SSSR_ROUGH, the damp streets'
// SSSR_ROUGH_DAMP, over the material's streak gloss; stage 2: times SSSR_SURF_MATTE to
// SSSR_SURF_GLOSSY by the gloss of the ground's own texture where the surface pass has it; standing
// water SSSR_ROUGH_WATER), each marched through the hierarchy (sssr_ray), the hits weighted as the
// classic trace's and averaged.
const float SSSR_ROUGH = 0.04, SSSR_ROUGH_DAMP = 0.1, SSSR_ROUGH_WATER = 0.01, SSSR_ROUGH_MIN = 0.005, SSSR_ROUGH_MAX = 0.4, SSSR_MIRROR = 0.02;
const float SSSR_SURF_MATTE = 1.5, SSSR_SURF_GLOSSY = 0.6;
float ndc_depth(vec3 v) { return pc.proj.x - pc.proj.y / max(-v.z, 1e-6); }
// One ray from view position P (V the view direction, N the normal) of GGX roughness rough with the
// random pair u, marched through the hierarchy from level lvl: the hit's colour in rgb and how far to
// trust it in a (the validation, towards the end of the ray, rays towards the camera fade); 0 for a
// miss. The ground starts at level 0 (the internal size) below SSSR_MIRROR roughness or when the trace
// runs at the internal size, else at level 1; glass always at level 0 (a wall seen edge on changes its
// depth a lot from pixel to pixel: at level 1 a ray leaving it meets its own nearer half as a hit).
int sssr_level(float rough) { return rough < SSSR_MIRROR || pc.dir.x < 1.5 ? 0 : 1; }
vec4 sssr_ray(vec3 P, vec3 V, vec3 N, float rough, vec2 u, int lvl) {
    vec3 R = sssr_sample_reflection(V, N, rough, u);
    if (dot(R, N) <= 0.01) R = reflect(V, N);   // a sample below the surface: the mirror direction
    float face = 1.0 - smoothstep(0.0, 0.6, R.z);   // rays towards the camera leave what the screen shows
    float L = pc.params.z;
    if (R.z > 0.0) L = min(L, 0.95 * (-0.05 - P.z) / R.z);   // stay in front of the camera
    if (face <= 0.0 || L <= 0.0) return vec4(0.0);
    vec3 E = P + R * L;
    vec3 a = vec3(to_pix(P) * pc.size.zw, ndc_depth(P)), d = vec3(to_pix(E) * pc.size.zw, ndc_depth(E)) - a;
    bool found;
    vec3 h = sssr_march(a, d, pc.size.xy, lvl, int(sl.sssr.y), found);
    float sz = depth_at(ivec2(h.xy * pc.size.xy));
    float conf = found ? sssr_validate(h, a.xy, pc.size.xy, sz, 0.2 + 0.06 * view_depth(sz)) : 0.0;
    if (conf <= 0.0) return vec4(0.0);
    float t = length(h.xy - a.xy) / max(length(d.xy), 1e-6);
    float far = 1.0 - smoothstep(0.5, 1.0, t);   // towards the end of the ray
    return vec4(textureLod(tex, h.xy, 0.0).rgb, conf * far * face);
}
int sssr_rays() { return clamp(int(sl.sssr.w), 1, 2); }
// The k-th of the texel's random pairs (salt: per use), moving on each frame (R2 sequence).
vec2 sssr_u(uint salt, int k) {
    return fract(hash3(uvec3(uvec2(gl_FragCoord.xy), salt)).xy + sl.sssr.x * vec2(0.75487767, 0.56984029) + float(k) * vec2(0.5));
}

// ---- Stage 2: the surface pass (glass, per-material roughness) ----
vec3 oct_decode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
// The view position of internal pixel q at NDC depth z.
vec3 view_at(ivec2 q, float z) {
    float d = view_depth(z);
    vec2 ndc = vec2((float(q.x) + 0.5) * pc.size.z * 2.0 - 1.0, 1.0 - (float(q.y) + 0.5) * pc.size.w * 2.0);
    return vec3(ndc.x * d * pc.proj.z, ndc.y * d * pc.proj.w, -d);
}
// The surface pass's material texel at internal pixel q where the frame shows it: its depth lies at
// or in front of the frame's depth z there (within SURF_TOL + SURF_TOL_K of the distance: a
// transparent window drawn without depth counts, one hidden behind a wall or a person does not).
// *n its view normal, *sg its shine and gloss, *d its NDC depth; false without one.
const float SURF_TOL = 0.02, SURF_TOL_K = 0.01;
bool surf_at(ivec2 q, float z, out vec3 n, out vec2 sg, out float d) {
    n = vec3(0.0);
    sg = vec2(0.0);
    d = 1.0;
    if (sl.glass.z <= 0.0) return false;
    q = clamp(q, ivec2(0), ivec2(pc.size.xy) - 1);
    d = texelFetch(surf_depth, q, 0).r;
    if (d >= 1.0) return false;
    if (z < 1.0) {
        float zv = view_depth(z);
        if (view_depth(d) > zv + SURF_TOL + SURF_TOL_K * zv) return false;
    }
    vec4 c = texelFetch(surf_col, q, 0);
    if (c.b <= 0.0) return false;
    n = oct_decode(c.rg * 2.0 - 1.0);
    sg = c.ba;
    return true;
}
// Glass (and the other glossy materials): how strongly a material of shine and gloss sg mirrors the
// scene (0-1: glass, chrome and car paint high, sheet metal a little, brick, concrete and asphalt not),
// and its GGX roughness from the gloss (glass about 0.02, painted metal 0.1, matte 0.3).
const float GLASS_SHINE_FROM = 0.25, GLASS_SHINE_TO = 0.7, GLASS_GLOSS_FROM = 0.45, GLASS_GLOSS_TO = 0.8;
float glass_k(vec2 sg) { return smoothstep(GLASS_SHINE_FROM, GLASS_SHINE_TO, sg.x) * smoothstep(GLASS_GLOSS_FROM, GLASS_GLOSS_TO, sg.y); }
float gloss_rough(float gloss) { return clamp(0.01 + 0.5 * pow(1.0 - clamp(gloss, 0.0, 1.0), 1.5), SSSR_ROUGH_MIN, SSSR_ROUGH_MAX); }
// A glass texel's alpha: GLASS_A + the share of the colour its reflection replaces (0-0.95); the
// ground's stays below 3 (wet + puddles).
const float GLASS_A = 8.0;
bool is_glass(float a) { return a >= GLASS_A - 0.5; }
// The glass at internal pixel q: a material texel seen there (surf_at) glossy enough (glass_k above
// GLASS_MIN), and in a wet frame not facing up (above GLASS_UP: wet ground, whose reflection is the
// ground's); *P its view position, *N its normal, *k glass_k, *rough its roughness.
const float GLASS_MIN = 0.02, GLASS_UP = 0.7;
bool glass_at(ivec2 q, out vec3 P, out vec3 N, out float k, out float rough) {
    P = vec3(0.0);
    k = rough = 0.0;
    vec2 sg;
    float d;
    if (!surf_at(q, depth_at(q), N, sg, d)) return false;
    k = glass_k(sg);
    if (k <= GLASS_MIN || (sl.glass.y <= 0.0 && dot(N, pc.cam[0].xyz) > GLASS_UP)) return false;
    P = view_at(q, d);
    rough = gloss_rough(sg.y);
    return true;
}
// The glass's reflection at view position P (normal N, glass_k k, roughness rough): the frame's lights
// mirrored in it (glass_lights, added), and sssr_rays() rays,
// what they hit, times a Schlick Fresnel term (F0 GLASS_F0 + GLASS_F0_K x k: glass to chrome) x k x
// the strength (at most GLASS_MAX): GLASS_KEEP of that share of the glass's own colour (its painted
// interior, its sill's highlight) is replaced by the reflection, and where the hit is bright (its
// strongest channel GLASS_HDR_FROM-TO: lamps, neon, lit windows, clipped by the 8-bit frame) up to
// GLASS_HDR times more of it is added: the bright street shows in the glass, the dark street does not
// blacken it.
const float GLASS_F0 = 0.04, GLASS_F0_K = 0.08, GLASS_HDR = 2.0, GLASS_HDR_FROM = 0.5, GLASS_HDR_TO = 0.95, GLASS_MAX = 0.9, GLASS_KEEP = 0.4;
// The frame's lamps and neon signs (the streaks' lights, sl.k.x of them, also off screen) mirrored in
// the glass at P (V the view direction, N the normal): for each light in front of the glass a lobe
// around its mirror image, exp(-tan^2 / s^2) of the half vector's tilt from N, s the roughness (at
// least GLASS_LIGHT_ROUGH: a lamp is not a point) widened by a sign's extent seen from P, times a
// falloff over GLASS_LIGHT_REACH x the light's radius (a mirror shows a lamp far from it too); the two
// strongest tested for walls and the screen as the streaks (light_open). Times GLASS_LIGHT_GAIN (a
// mirrored lamp is far brighter than the frame's white).
const float GLASS_LIGHT_ROUGH = 0.05, GLASS_LIGHT_REACH = 4.0, GLASS_LIGHT_GAIN = 3.0;
vec3 glass_lights(vec3 P, vec3 V, vec3 N, float rough) {
    int n = min(int(sl.k.x), WET_LIGHTS);
    vec3 sum = vec3(0.0), c0 = vec3(0.0), c1 = vec3(0.0);
    float y0 = 0.0, y1 = 0.0;
    int i0 = -1, i1 = -1;
    float s2 = max(rough, GLASS_LIGHT_ROUGH) * max(rough, GLASS_LIGHT_ROUGH);
    for (int i = 0; i < n; i++) {
        vec3 D = sl.pos_r[i].xyz - P;
        if (dot(D, N) <= 0.05) continue;   // behind the glass
        float dist = length(D), x = dist / (GLASS_LIGHT_REACH * sl.pos_r[i].w);
        if (x >= 1.0) continue;
        vec3 H = normalize(D / dist - V);
        float hn = dot(H, N);
        if (hn <= 1e-3) continue;
        float t2 = (1.0 - hn * hn) / (hn * hn), w2 = s2;
        if (sl.col[i].w > 0.0) {   // a sign: an extended light
            float e = SIGN_SPREAD * sl.col[i].w / dist;
            w2 += e * e;
        }
        float fall = 1.0 - x * x;
        vec3 c = sl.col[i].rgb * (exp(-t2 / w2) * fall * fall);
        sum += c;
        float y = c.r + c.g + c.b;
        if (y > y0) {
            c1 = c0, y1 = y0, i1 = i0;
            c0 = c, y0 = y, i0 = i;
        } else if (y > y1) {
            c1 = c, y1 = y, i1 = i;
        }
    }
    if (i0 >= 0 && y0 > OCC_MIN) {
        sum -= c0 * (1.0 - light_open(i0, P));
        if (i1 >= 0 && y1 > OCC_MIN && y1 >= OCC_SHARE * y0) sum -= c1 * (1.0 - light_open(i1, P));
        sum = max(sum, vec3(0.0));
    }
    return sum * GLASS_LIGHT_GAIN;
}
void glass_trace(vec3 P, vec3 N, float k, float rough) {
    vec3 V = normalize(P);
    float F0 = GLASS_F0 + GLASS_F0_K * k;
    float F = F0 + (1.0 - F0) * pow(1.0 - clamp(dot(-V, N), 0.0, 1.0), 5.0);
    int n = sssr_rays();
    vec3 sum = vec3(0.0);
    float cov = 0.0;
    for (int i = 0; i < n; i++) {
        vec4 h = sssr_ray(P, V, N, rough, sssr_u(0x6A55E1u, i), 0);
        float m = max(h.r, max(h.g, h.b));
        sum += h.rgb * ((GLASS_KEEP + GLASS_HDR * smoothstep(GLASS_HDR_FROM, GLASS_HDR_TO, m)) * h.a);
        cov += h.a;
    }
    float w = clamp(F * k * sl.glass.x, 0.0, GLASS_MAX) / float(n);
    o = vec4(sum * w, GLASS_A + min(cov * w * GLASS_KEEP, 0.95));
    if (sl.k.x > 0.0) o.rgb += glass_lights(P, V, N, rough) * (F * k * sl.glass.x);   // the lamps and signs, added
}

void sssr_trace(vec3 P, vec3 V, vec3 N, float F, float wet, float pud) {
    float rough = clamp((damp_on() ? SSSR_ROUGH_DAMP : SSSR_ROUGH) / max(t_gloss, 0.05), SSSR_ROUGH_MIN, SSSR_ROUGH_MAX);
    if (sl.glass.z > 0.0) {   // stage 2: the ground's own texture's gloss
        vec3 sn;
        vec2 sg;
        float sd;
        if (surf_at(t_p, depth_at(t_p), sn, sg, sd))
            rough = clamp(rough * mix(SSSR_SURF_MATTE, SSSR_SURF_GLOSSY, smoothstep(0.15, 0.6, sg.y)), SSSR_ROUGH_MIN, SSSR_ROUGH_MAX);
    }
    rough = mix(rough, SSSR_ROUGH_WATER, clamp(pud / max(wet, 1e-3), 0.0, 1.0));
    int n = sssr_rays();
    vec3 sum = vec3(0.0);
    float cov = 0.0;
    for (int i = 0; i < n; i++) {
        vec4 h = sssr_ray(P, V, N, rough, sssr_u(0x51ED27u, i), sssr_level(rough));
        sum += h.rgb * (hit_bright(h.rgb, pud) * h.a);
        cov += h.a;
    }
    finish(sum * (F * pc.params.x * wet / float(n)), cov / float(n));
}

void trace() {
    o = vec4(0.0);
    ivec2 mx = ivec2(pc.size.xy) - 1, p = min(ivec2(gl_FragCoord.xy * pc.dir.x), mx);
    if (SSSR > 0) {
        t_p = p;
        if (sl.glass.x > 0.0) {   // stage 2: glass first; a dry frame has nothing else
            vec3 GP, GN;
            float gk, gr;
            if (glass_at(p, GP, GN, gk, gr)) {
                glass_trace(GP, GN, gk, gr);
                return;
            }
            if (sl.glass.y > 0.0) return;
        }
    }
    if (depth_at(p) >= 1.0 || is_char(p)) return;
    vec3 P = view_pos(p);
    // Damp streets: only asphalt (and a little concrete) in the open; tested first (four map texels),
    // so the rest of the screen costs little.
    float damp = 1.0, on_map = 0.0;
    if (damp_on()) {
        damp = damp_at(world_pos(P));
        if (damp <= 0.0) return;
        on_map = damp_on_ground(world_pos(P));
    }
    // The normal from the depths two pixels away (as the splashes): upward-facing only.
    vec3 U = pc.cam[0].xyz;
    vec3 n = surface_normal(p, P);
    if (dot(n, n) == 0.0) {
        if (on_map <= 0.0) return;
        n = U;
    }
    // (Damp streets: a looser, softer test, and the map's own ground counts as facing up: the depth's
    // normal breaks at every fleck of litter or crack, which cut blocky holes into the streaks.)
    float wet = damp_on() ? max(smoothstep(DAMP_UP_FROM, DAMP_UP_TO, dot(n, U)), on_map) : smoothstep(0.8, 0.92, dot(n, U));
    if (wet <= 0.0) return;
    if (on_map > 0.0) n = normalize(mix(n, U, on_map));
    // And flat (the splashes' test, at FLAT_REACH): on each axis the nearer of the two sides counts,
    // so a top curving away on both sides is dry while the floor at a wall's foot or a kerb (one
    // side off the plane) stays wet.
    float px = 2.0 * -P.z * pc.proj.w * pc.size.w;   // view units per internal pixel here
    int k = int(clamp(FLAT_REACH / px, 4.0, 64.0));
    vec4 off = surface_offsets(p, P, n, k);
    float uneven = max(min(off.x, off.y), min(off.z, off.w));
    if (damp_on()) uneven = max(uneven - DAMP_FLAT_STEP - DAMP_FLAT_DEPTH * -P.z, 0.0) / DAMP_FLAT_SLACK;
    wet *= surface_flat(uneven, px, k);
#ifdef RAIN_COVER
    wet *= rain_open(P);
#endif
    if (wet <= 0.0) return;
    // Materials: the look of the surface under this pixel (1 without them).
    vec4 look = vec4(1.0);
    if (sl.mat.w > 0.0) {
        look = mat_look(world_pos(P), t_mwet);
        t_refl = look.x;
        t_gloss = look.w;
    }
    if (damp_on()) {
        wet *= damp;
        t_gloss *= DAMP_GLOSS;
        vec2 uv0 = (vec2(p) + 0.5) * pc.size.zw, du = DAMP_SHEEN_REACH * pc.dir.x * pc.size.zw;
        vec3 self = textureLod(tex, uv0, 0.0).rgb, Y = vec3(0.299, 0.587, 0.114);
        float around = 0.25 * (dot(textureLod(tex, uv0 + vec2(du.x, 0.0), 0.0).rgb, Y) + dot(textureLod(tex, uv0 - vec2(du.x, 0.0), 0.0).rgb, Y) +
                               dot(textureLod(tex, uv0 + vec2(0.0, du.y), 0.0).rgb, Y) + dot(textureLod(tex, uv0 - vec2(0.0, du.y), 0.0).rgb, Y));
        float lit = smoothstep(DAMP_SHEEN_FROM, DAMP_SHEEN_TO, dot(self, Y) - around);
        t_sheen = self * (DAMP_SHEEN * lit * wet * mix(0.6, 1.4, vnoise(world_pos(P).xy * 0.6 + vec2(7.3, 29.1))));
    }
    o.a = wet * look.y;
    // Puddles: their share, and the water's normal tilted by the ripples (near the camera only).
    float pud = 0.0;
    vec3 N = U;
    if (pc.dir.w > 0.0 && look.z > 0.0) {
        vec3 W = world_pos(P);
        float bias = 0.15 * (look.z - 1.0);
        if (damp_on()) bias += DAMP_PATCH_BIAS + DAMP_PATCH_KERB * damp_kerb(W);
        pud = puddle(W, P, bias) * min(look.z, 1.0) * wet;
        if (pud > 0.0) {
            if (!damp_on()) {   // no drops on a dry street: no ripples
                vec2 sl = ripple_slope(W.xy, pc.params.w) * (0.3 * (1.0 - smoothstep(8.0, 16.0, -P.z)));
                N = normalize(mix(U, normalize(view_dir(vec3(-sl, 1.0))), pud));
            }
            o.a = wet * look.y + pud * pc.dir.w;
        }
    }
    // The mirrored ray (about world up: the ground plane, not its bumps), Schlick fresnel of water.
    vec3 V = normalize(P), R = reflect(V, N);
    float F = 0.04 + 0.96 * pow(1.0 - clamp(dot(-V, N), 0.0, 1.0), 5.0);
    if (streaks_on()) {
        t_P = P, t_V = V, t_N = N, t_U = U, t_F = F, t_wet = wet, t_pud = pud;
        t_rough = mix(0.75, 1.35, vnoise(world_pos(P).xy * 0.45 + vec2(41.7, 13.2)));   // world-anchored roughness
    }
    if (SSSR > 0) {
        sssr_trace(P, V, N, F, wet, pud);
        return;
    }
    float face = 1.0 - smoothstep(0.0, 0.6, R.z);   // rays towards the camera leave what the screen shows
    if (face <= 0.0) {
        finish(vec3(0.0), 0.0);
        return;
    }
    float L = pc.params.z;
    if (R.z > 0.0) L = min(L, 0.95 * (-0.05 - P.z) / R.z);   // stay in front of the camera
    if (L <= 0.0) {
        finish(vec3(0.0), 0.0);
        return;
    }
    vec3 E = P + R * L;
    vec2 s0 = to_pix(P), dS = to_pix(E) - s0;
    // Clip the segment to the screen.
    float tmax = 1.0;
    if (dS.x > 1e-4) tmax = min(tmax, (pc.size.x - 1.0 - s0.x) / dS.x);
    else if (dS.x < -1e-4) tmax = min(tmax, -s0.x / dS.x);
    if (dS.y > 1e-4) tmax = min(tmax, (pc.size.y - 1.0 - s0.y) / dS.y);
    else if (dS.y < -1e-4) tmax = min(tmax, -s0.y / dS.y);
    float len = length(dS);
    if (tmax * len < 2.0 * pc.dir.x) {
        finish(vec3(0.0), 0.0);
        return;
    }
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
    if (hit < 0.0) {
        finish(vec3(0.0), 0.0);
        return;
    }
    float lo = prev, hi = hit;
    for (int i = 0; i < REFINE; i++) {
        float t = 0.5 * (lo + hi), z = depth_at(ivec2(s0 + dS * t));
        if (z < 1.0 && 1.0 / mix(k0, k1, t) > view_depth(z)) hi = t;
        else lo = t;
    }
    vec2 uv = (s0 + dS * hi) * pc.size.zw;
    vec3 c = textureLod(tex, uv, 0.0).rgb;
    float bright = hit_bright(c, pud);
    vec2 e = min(uv, 1.0 - uv);
    float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
    float far = 1.0 - smoothstep(0.5, 1.0, hi);   // towards the end of the ray
    finish(c * (bright * F * pc.params.x * edge * far * face * wet), edge * far * face);
}

// Damp streets: a dry texel between two damp ones on its row at about its depth (DAMP_FILL_DEPTH
// of it) takes their mean: the odd texel the mask's tests drop in the middle of the ground (flecks
// of litter, the depth's steps far away) leaves no hole in a streak, while the foot of a wall, a
// kerb's face or a person (dry on their own row too) stays dry.
vec4 damp_fill(ivec2 p, ivec2 mx) {
    vec4 l = texelFetch(tex, clamp(p - ivec2(1, 0), ivec2(0), mx), 0), r = texelFetch(tex, clamp(p + ivec2(1, 0), ivec2(0), mx), 0);
    if (l.a <= 0.0 || r.a <= 0.0) return vec4(0.0);
    if (SSSR > 0 && (is_glass(l.a) || is_glass(r.a))) return vec4(0.0);   // stage 2: glass is not ground
    float d = view_depth(depth_at(ivec2((vec2(p) + 0.5) * pc.dir.x)));
    float dl = view_depth(depth_at(ivec2((vec2(p - ivec2(1, 0)) + 0.5) * pc.dir.x)));
    float dr = view_depth(depth_at(ivec2((vec2(p + ivec2(1, 0)) + 0.5) * pc.dir.x)));
    if (abs(dl - d) > DAMP_FILL_DEPTH * d || abs(dr - d) > DAMP_FILL_DEPTH * d) return vec4(0.0);
    return 0.5 * (l + r);
}
// SSSR's blur: the classic one's taps (puddles blurred less), SSSR_BLUR as wide along the view: the
// rough ground's spread is in the sampled rays already, this only smooths what the temporal
// accumulation leaves.
const float SSSR_BLUR = 0.6;
void sssr_blur(ivec2 p, ivec2 mx, vec4 c0, float st) {
    float sig = (damp_on() ? 12.0 : 18.0) * SSSR_BLUR;
    if (pc.dir.w > 0.0) sig *= 1.0 - 0.85 * clamp(c0.a - 1.0, 0.0, 1.0);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -6; y <= 6; y++)
        for (int x = -1; x <= 1; x++) {
            vec4 t = texelFetch(tex, clamp(p + ivec2(x, int(round(float(y) * st))), ivec2(0), mx), 0);
            if (is_glass(t.a)) continue;   // stage 2: glass is not ground
            float w = exp(-float(y * y) / max(sig, 0.5)) * (x == 0 ? 1.0 : 0.6);
            sum += t.rgb * w;
            wsum += min(t.a, 1.0) * w;
        }
    o = vec4(sum / max(wsum, 1e-4) * min(c0.a, 1.0), c0.a);
}

// Stage 2: a glass texel's blur, over the glass texels of a 5 x 5 window only, a Gaussian as wide as
// GLASS_BLUR x its roughness (texels; a clean window stays sharp), colour and coverage alike.
const float GLASS_BLUR = 8.0;
void glass_blur(ivec2 p, ivec2 mx, vec4 c0) {
    ivec2 q = min(ivec2((vec2(p) + 0.5) * pc.dir.x), ivec2(pc.size.xy) - 1);
    vec3 n;
    vec2 sg;
    float d;
    float rough = surf_at(q, depth_at(q), n, sg, d) ? gloss_rough(sg.y) : 0.0;
    float sig = max(GLASS_BLUR * rough, 0.3);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            vec4 t = texelFetch(tex, clamp(p + ivec2(x, y), ivec2(0), mx), 0);
            if (!is_glass(t.a)) continue;
            float w = exp(-float(x * x + y * y) / (2.0 * sig * sig));
            sum += vec4(t.rgb, t.a - GLASS_A) * w;
            wsum += w;
        }
    sum /= max(wsum, 1e-6);
    o = vec4(sum.rgb, GLASS_A + sum.a);
}

void blur() {
    ivec2 p = ivec2(gl_FragCoord.xy), mx = ivec2(pc.dir.yz) - 1;
    vec4 c0 = texelFetch(tex, p, 0);
    if (SSSR > 0 && is_glass(c0.a)) {
        glass_blur(p, mx, c0);
        return;
    }
    if (c0.a <= 0.0 && damp_on()) c0 = damp_fill(p, mx);
    if (c0.a <= 0.0) {
        o = vec4(0.0);
        return;
    }
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    // Light streaks: the taps spread with the view distance (longer streaks far away), 1 without them.
    float st = 1.0;
    if (streaks_on()) st = mix(0.75, 2.0, smoothstep(6.0, 40.0, view_depth(depth_at(ivec2((vec2(p) + 0.5) * pc.dir.x)))));
    if (SSSR > 0) {
        sssr_blur(p, mx, c0, st);
        return;
    }
    float sig0 = damp_on() ? 12.0 : 18.0;   // damp streets: tighter reflections
    if (pc.dir.w > 0.0) {   // puddles: alpha above 1 in them, blurred less there
        float sig = sig0 * (1.0 - 0.85 * clamp(c0.a - 1.0, 0.0, 1.0));
        for (int y = -6; y <= 6; y++)
            for (int x = -1; x <= 1; x++) {
                vec4 t = texelFetch(tex, clamp(p + ivec2(x, streaks_on() ? int(round(float(y) * st)) : y), ivec2(0), mx), 0);
                float w = exp(-float(y * y) / sig) * (x == 0 ? 1.0 : 0.6);
                sum += t.rgb * w;
                wsum += min(t.a, 1.0) * w;
            }
        o = vec4(sum / max(wsum, 1e-4) * min(c0.a, 1.0), c0.a);
        return;
    }
    for (int y = -6; y <= 6; y++)
        for (int x = -1; x <= 1; x++) {
            vec4 t = texelFetch(tex, clamp(p + ivec2(x, streaks_on() ? int(round(float(y) * st)) : y), ivec2(0), mx), 0);
            float w = (damp_on() ? exp(-float(y * y) / 12.0) : exp(-float(y * y) / 18.0)) * (x == 0 ? 1.0 : 0.6);
            sum += t.rgb * w;
            wsum += t.a * w;
        }
    o = vec4(sum / max(wsum, 1e-4) * c0.a, c0.a);
}

// SSSR's composite: as comp's below, but the reduced texels are filtered bilinearly by hand and only
// those of this pixel's own kind count (stage 2: glass_at here, at the internal size, or the ground),
// characters' texels left out as there. A glass pixel replaces its reflection's share of the colour
// (alpha GLASS_A + that share); the ground as comp's.
void comp_sssr() {
    ivec2 ip = ivec2(gl_FragCoord.xy), mx = ivec2(pc.size.xy) - 1;
    if (is_char(ip)) discard;
    bool g = false;
    if (sl.glass.x > 0.0) {
        vec3 P, N;
        float k, rough;
        g = glass_at(ip, P, N, k, rough);
    }
    vec2 f = gl_FragCoord.xy / pc.dir.x - 0.5;
    ivec2 b = ivec2(floor(f)), tmx = ivec2(pc.dir.yz) - 1;
    vec2 fr = f - vec2(b);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            ivec2 t = clamp(b + ivec2(i, j), ivec2(0), tmx);
            vec4 v = texelFetch(tex, t, 0);
            if (is_glass(v.a) != g || (!g && is_char(min(ivec2((vec2(t) + 0.5) * pc.dir.x), mx)))) continue;
            float w = (i == 1 ? fr.x : 1.0 - fr.x) * (j == 1 ? fr.y : 1.0 - fr.y);
            sum += v * w;
            wsum += w;
        }
    if (wsum <= 1e-4) discard;
    vec4 r = sum / wsum;
    if (g) {
        float a = r.a - GLASS_A;
        if (a <= 1e-3 && max(r.r, max(r.g, r.b)) <= 1e-4) discard;
        o = vec4(r.rgb, clamp(a, 0.0, 0.95));
        return;
    }
    if (r.a <= 1e-3) discard;
    if (pc.dir.w > 0.0)   // puddles (alpha above 1) darker still
        o = vec4(r.rgb, min((min(r.a, 1.0) + 0.6 * max(r.a - 1.0, 0.0)) * pc.params.y, 0.95));
    else
        o = vec4(r.rgb, r.a * pc.params.y);
}

void comp() {
    if (SSSR > 0) {
        comp_sssr();
        return;
    }
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

// SSSR's temporal accumulation (mode 3) of mode 0's output (binding 1) with the previous frame's
// (also the classic trace's puddle texels, alpha above 1)
// result (binding 7, the reduced size): the surface point of this texel is taken back through the
// previous frame's camera (sl.prev) to where the history saw it; that history is clamped to the mean
// +- TEMPORAL_CLAMP standard deviations of this frame's 3x3 neighbourhood of the same kind (wet ground
// or, stage 2, glass: the reprojection is the surface's, not the reflection's, so the clamp keeps a
// moving reflection from smearing), and blended with this frame's by TEMPORAL_KEEP (puddles, nearly
// mirrors and so less noisy: TEMPORAL_KEEP_WATER; glass TEMPORAL_KEEP_GLASS). Variance guided: the
// further the clamp had to move the history (in standard deviations), the less of it is kept (down to
// TEMPORAL_KEEP_MOVED of it), so what changed (a lamp switched, a car passing) shows at once while a
// steady reflection keeps averaging its noise away. Without a valid history (the first frame, a camera
// cut: sl.sssr.z 0), off the previous screen or where the history was dry or of the other kind: this
// frame's alone. The ground's mask (alpha) is always this frame's.
const float TEMPORAL_CLAMP = 1.25, TEMPORAL_KEEP = 0.9, TEMPORAL_KEEP_WATER = 0.7, TEMPORAL_KEEP_GLASS = 0.85, TEMPORAL_KEEP_MOVED = 0.5;
void temporal() {
    ivec2 p = ivec2(gl_FragCoord.xy), mx = ivec2(pc.dir.yz) - 1;
    vec4 c = texelFetch(tex, p, 0);
    o = c;
    if (c.a <= 0.0 || sl.sssr.z < 0.5) return;
    if (SSSR == 0 && c.a <= 1.0) return;   // classic: only the puddles accumulate
    bool g = is_glass(c.a);
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float n = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec4 t = texelFetch(tex, clamp(p + ivec2(i, j), ivec2(0), mx), 0);
            if (t.a <= 0.0 || is_glass(t.a) != g) continue;
            m1 += t.rgb;
            m2 += t.rgb * t.rgb;
            n += 1.0;
        }
    m1 /= n;
    vec3 sd = sqrt(max(m2 / n - m1 * m1, vec3(0.0)));
    ivec2 q = min(ivec2((vec2(p) + 0.5) * pc.dir.x), ivec2(pc.size.xy) - 1);
    vec3 d = world_pos(view_pos(q)) - vec3(sl.prev[0].w, sl.prev[1].w, sl.prev[2].w);
    vec3 v = vec3(dot(d, sl.prev[0].xyz), dot(d, sl.prev[1].xyz), dot(d, sl.prev[2].xyz));
    if (v.z > -0.05) return;
    vec2 ndc = vec2(v.x / (-v.z * sl.prevp.x), v.y / (-v.z * sl.prevp.y));
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y) * pc.size.xy / (pc.dir.x * pc.dir.yz);   // the reduced image covers a little more
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return;
    vec4 h = textureLod(hist, uv, 0.0);
    if (h.a <= 0.0 || is_glass(h.a) != g) return;
    vec3 hc = clamp(h.rgb, m1 - TEMPORAL_CLAMP * sd, m1 + TEMPORAL_CLAMP * sd);
    float moved = length(hc - h.rgb) / (length(sd) + 1.0 / 255.0);
    float keep = g ? TEMPORAL_KEEP_GLASS : mix(TEMPORAL_KEEP, TEMPORAL_KEEP_WATER, clamp(c.a - 1.0, 0.0, 1.0)) * clamp(h.a / c.a, 0.0, 1.0);
    keep *= mix(1.0, TEMPORAL_KEEP_MOVED, smoothstep(0.0, 2.0, moved));
    // (A glass texel's alpha is its reflection's coverage, stochastic like its colour: averaged alike.)
    o = vec4(mix(c.rgb, hc, keep), g ? GLASS_A + mix(c.a - GLASS_A, clamp(h.a - GLASS_A, 0.0, 0.95), keep) : c.a);
}

void main() {
    if (MODE == 3)
        temporal();
    else if (MODE == 1)
        blur();
    else if (MODE == 2)
        comp();
    else
        trace();
}
