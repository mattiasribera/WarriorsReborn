#version 450
// NV2A register combiners for the draw's RPixelShader (src/renderer.h). Semantics follow xemu
// hw/xbox/nv2a/pgraph/glsl/psh.c (https://github.com/xemu-project/xemu) and NV_register_combiners:
// - registers: 0 zero, 1 c0, 2 c1, 3 fog, 4 v0, 5 v1, 8-11 t0-t3, 12 r0, 13 r1, 14 v1+r0 sum and
//   15 E*F (final combiner only); r0.a starts as t0.a;
// - input byte: register in bits 0-3, alpha channel bit 4 (rgb portion: .aaa, alpha portion: .a
//   instead of .b), mapping in bits 5-7;
// - output dword: CD register bits 0-3, AB 4-7, SUM 8-11, CD dot 12, AB dot 13, mux 14, output
//   mapping 15-17, CD blue-to-alpha 18, AB blue-to-alpha 19; mux picks CD when r0.a's MSB is set
//   (>= 0.5, PS_COMBINERCOUNT_MUX_MSB) or its LSB (otherwise), else AB; results clamp to [-1, 1];
// - final combiner: rgb = A*B + (1-A)*C + D, alpha = G, inputs clamped to [0, 1].
// - depth stages (SHADOW, the renderer's shadow_word): the NV2A shadow compare of xemu
//   psh_append_shadowmap: texel x max <func> clamp(r/q, 0, max) for PROJECT3D, texel <func> 0 for
//   PROJECT2D, 1 or 0 in every channel; here the four texels of the bilinear footprint are compared
//   and the results filtered (percentage-closer), outside the surface the border reads 1.0.
// Then the alpha test (D3DCMPFUNC numbering, 0 = off) against a 0..255 reference. The target has
// no alpha channel; the alpha output is the blend source alpha.
//
// The program's structure (stage count and flags, input and output words, final combiner inputs,
// texture modes) arrives as specialization constants, one pipeline variant per structure, so the
// driver compiles each program into straight-line code: every register index below is a constant
// after specialization and the stage loop is unrolled by hand. Interpreting the words at run time
// instead (dynamic register indexing per fragment) is what made the shader slow on CPU drivers
// such as llvmpipe. The per-draw values (stage and final
// constants, fog color) stay in the storage buffer, indexed by a push constant, so draws that only
// differ in constants share a pipeline.
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2) uniform sampler2D tex2;
layout(set = 0, binding = 3) uniform sampler2D tex3;
struct Ps {
    uint combiners;
    uint rgb_in[8];
    uint alpha_in[8];
    uint rgb_out[8];
    uint alpha_out[8];
    uint c0[8];
    uint c1[8];
    uint final_abcd, final_efg, final_c0, final_c1, tex_modes, fog, shadow_func;
    float shadow_depth[2];
};
layout(std430, set = 0, binding = 4) readonly buffer Programs { Ps ps[]; };
// Lamp character shadows (renderer_lamp_shadows; read with the shadow word's bit 14 only): the
// frame's lights in the depth stage's homogeneous coordinates and in the world.
#define LSH_MAX 8
layout(std430, set = 0, binding = 5) readonly buffer LampShadows {
    vec4 inv[4];        // rows: homogeneous world position = inv x (s, t, r, q)
    vec4 params;        // light projection A, B (r = A q - B), strength, lights in use
    vec4 shape;         // caster height, shadow length, march start (world units), directional share
    vec4 stq[LSH_MAX];  // the light's (s, t, r, q)
    vec4 pos[LSH_MAX];  // the light's world position (up +z), radius
    vec4 wt[LSH_MAX];   // x: weight
    vec4 fade;          // the low-light fade: min angle, full angle (degrees; full <= min: none), low length;
                        // w: 1 = the title's own shadow stays under lamps (lamp_shadow `title`)
    vec4 eye;           // the camera (world)
    vec4 wall;          // the wall map (lsh_walls): x0, y0, 1 / cell, 1 = loaded
    vec4 wallz;         // z0, zstep, no wall test nearer the receiver than this (world units: its own wall)
} lsh;
// The level's walls (renderer_lamp_wall_map): per texel the lowest and highest point of the walls
// over it, 0 none, h: world z = z0 + (h - 1) zstep.
layout(set = 0, binding = 8) uniform usampler2D lsh_walls;
#extension GL_GOOGLE_include_directive : require
#define LW_MAP lsh_walls
#include "light_walls.glsl"
// World shadows (renderer_world_shadow; read with the shadow word's bit 15 only): the map of the
// frame's opaque world seen along the light, and its parameters (vk_renderer.cpp WorldShadowGpu).
#define WSH_LAMPS 8
layout(set = 0, binding = 6) uniform sampler2D wsh_map;
layout(std430, set = 0, binding = 7) readonly buffer WorldShadow {
    vec4 row[3];    // clip (x, y, w, 1) -> map NDC x, y, depth
    vec4 wrow[3];   // clip -> world
    vec4 vp;        // the frame's viewport: x0, y0, 1 / width, 1 / height (pixels)
    vec4 k;         // on, depth bias, darkening, map size
    vec4 eye;       // camera (world), lamps in use
    vec4 fade;      // full shadow to x, none from y (world units from the eye); baked luminance z (none) to w (full)
    vec4 lampk;     // x: the share of the shadow a lamp removes at its centre
    vec4 lamp[WSH_LAMPS];   // world position, radius
    vec4 lrow[3];   // world -> map (the indoor gate's)
    vec4 gin;       // the indoor gate's input
    vec4 gfwd;
    vec4 gate;      // x: the indoor gate's level (scales the darkening)
} wsh;
#ifdef SOFT
// Soft particles (renderer_soft; compiled with -DSOFT, and -DMSAA for a multisampled target): the
// frame's depth target, read in place (its attachment is read-only in the pass), and the depth
// projection plus 1 / distance after the draw's words.
layout(push_constant) uniform Draw { uint alpha_func; uint alpha_ref; uint program; uint pad; vec4 soft; } draw;
#ifdef MSAA
layout(set = 1, binding = 0) uniform sampler2DMS scene_depth;
#else
layout(set = 1, binding = 0) uniform sampler2D scene_depth;
#endif
// 1: the fade scales colour and alpha (blends with source factor ONE: additive, premultiplied); 0:
// alpha only (SRC_ALPHA blends).
layout(constant_id = 39) const uint SOFT_RGB = 0u;
#elif defined(CHAR_LIT)
// Character lighting: word 3 is the world-lit draw's light mask (renderer_draw_world_lit; 0 for
// the character draws). With EMIT too (a masked lit draw), the emission mask is in set 2 (set 1
// holds the lights) and the emission words follow, as below.
#ifdef EMIT
layout(set = 2, binding = 0) uniform sampler2D emit_mask;
layout(push_constant) uniform Draw { uint alpha_func; uint alpha_ref; uint program; uint lights; vec4 emit; } draw;
#elif defined(RELIEF)
// Relief (renderer_set_relief; built with -DCHAR_LIT -DRELIEF): the normal map (binding 0) and the
// specular map (binding 1) in set 2; after the draw's words: normal strength, specular strength,
// stage + 4 x flags (bit 0 a normal map, bit 1 a specular map, bit 2 --dry-highlights: the specular
// map's B / A material) and the baked-light relief weight.
layout(set = 2, binding = 0) uniform sampler2D relief_map;
layout(set = 2, binding = 1) uniform sampler2D spec_map;
#ifdef MAT
// --eyes, --cloth: after the relief's words the eyes' openness (renderer_set_eye_open), 0, 0, 0
layout(push_constant) uniform Draw { uint alpha_func; uint alpha_ref; uint program; uint lights; vec4 relief; vec4 mat; } draw;
#else
layout(push_constant) uniform Draw { uint alpha_func; uint alpha_ref; uint program; uint lights; vec4 relief; } draw;
#endif
#elif defined(MAT)
layout(push_constant) uniform Draw { uint alpha_func; uint alpha_ref; uint program; uint lights; vec4 unused; vec4 mat; } draw;
#else
layout(push_constant) uniform Draw { uint alpha_func; uint alpha_ref; uint program; uint lights; } draw;
#endif
#elif defined(EMIT)
// Neon glow (renderer_set_emission; compiled with -DEMIT): the emission mask (L8: .r) in set 1,
// then after the draw's words the strength (with the flicker), the bloom weight and the stage it
// lights (a float); word 3 is unused (it is the light mask of the lit variant).
layout(set = 1, binding = 0) uniform sampler2D emit_mask;
layout(push_constant) uniform Draw { uint alpha_func; uint alpha_ref; uint program; uint unused; vec4 emit; } draw;
#else
layout(push_constant) uniform Draw { uint alpha_func; uint alpha_ref; uint program; } draw;
#endif

// Structure of the program (renderer.h RPixelShader fields of the same names).
layout(constant_id = 0) const uint COMBINERS = 1u;
layout(constant_id = 1) const uint RGB_IN0 = 0u;
layout(constant_id = 2) const uint RGB_IN1 = 0u;
layout(constant_id = 3) const uint RGB_IN2 = 0u;
layout(constant_id = 4) const uint RGB_IN3 = 0u;
layout(constant_id = 5) const uint RGB_IN4 = 0u;
layout(constant_id = 6) const uint RGB_IN5 = 0u;
layout(constant_id = 7) const uint RGB_IN6 = 0u;
layout(constant_id = 8) const uint RGB_IN7 = 0u;
layout(constant_id = 9) const uint ALPHA_IN0 = 0u;
layout(constant_id = 10) const uint ALPHA_IN1 = 0u;
layout(constant_id = 11) const uint ALPHA_IN2 = 0u;
layout(constant_id = 12) const uint ALPHA_IN3 = 0u;
layout(constant_id = 13) const uint ALPHA_IN4 = 0u;
layout(constant_id = 14) const uint ALPHA_IN5 = 0u;
layout(constant_id = 15) const uint ALPHA_IN6 = 0u;
layout(constant_id = 16) const uint ALPHA_IN7 = 0u;
layout(constant_id = 17) const uint RGB_OUT0 = 0u;
layout(constant_id = 18) const uint RGB_OUT1 = 0u;
layout(constant_id = 19) const uint RGB_OUT2 = 0u;
layout(constant_id = 20) const uint RGB_OUT3 = 0u;
layout(constant_id = 21) const uint RGB_OUT4 = 0u;
layout(constant_id = 22) const uint RGB_OUT5 = 0u;
layout(constant_id = 23) const uint RGB_OUT6 = 0u;
layout(constant_id = 24) const uint RGB_OUT7 = 0u;
layout(constant_id = 25) const uint ALPHA_OUT0 = 0u;
layout(constant_id = 26) const uint ALPHA_OUT1 = 0u;
layout(constant_id = 27) const uint ALPHA_OUT2 = 0u;
layout(constant_id = 28) const uint ALPHA_OUT3 = 0u;
layout(constant_id = 29) const uint ALPHA_OUT4 = 0u;
layout(constant_id = 30) const uint ALPHA_OUT5 = 0u;
layout(constant_id = 31) const uint ALPHA_OUT6 = 0u;
layout(constant_id = 32) const uint ALPHA_OUT7 = 0u;
layout(constant_id = 33) const uint FINAL_ABCD = 0u;
layout(constant_id = 34) const uint FINAL_EFG = 0u;
layout(constant_id = 35) const uint TEX_MODES = 1u;
// Shadow compare: bits 0-2 NV097_SET_SHADOW_DEPTH_FUNC, 4-7 stages bound to a depth surface, 8-11
// those with 24-bit depth, 12 soft filtering, 13 shadow range, 14 lamp shadows, 15 world shadows (0 =
// no depth stage).
layout(constant_id = 36) const uint SHADOW = 0u;
// Bloom light mask (vk_renderer.cpp mask_role): an opaque scene draw before the bloom split writes
// alpha 0 (no blending, so its alpha is not otherwise used); 0 = the alpha as computed.
layout(constant_id = 37) const uint MASK_CLEAR = 0u;
// Shadow range (RendererConfig.shadow_range, view units of the light; used with SHADOW bit 13).
layout(constant_id = 38) const float SHADOW_RANGE = 0.0;

layout(location = 0) in vec4 v_d0;
layout(location = 1) in vec4 v_d1;
layout(location = 2) in vec4 v_t0;
layout(location = 3) in vec4 v_t1;
layout(location = 4) in vec4 v_t2;
layout(location = 5) in vec4 v_t3;
layout(location = 6) in float v_fog;   // fog amount (1 - per-vertex fog factor)
layout(location = 0) out vec4 out_color;
#ifdef CHAR_LIT
// Character lighting (renderer_draw_lit; built with -DCHAR_LIT): the interpolated view position and
// normal light the fragment as the lamp pass would (lamp.frag light(): falloff, wrapped Lambert,
// soft knee, the character gain), but with the mesh's smooth normal instead of one from the depth.
#extension GL_GOOGLE_include_directive : require
#include "char_lit.glsl"
layout(location = 7) in vec3 v_view;
layout(location = 8) in vec3 v_nrm;
// Light walls (RLampLights.walls; lit.wall.w set): a view position (w 1) or direction (w 0) in the
// world, and whether light i reaches the fragment at world p with normal n (light_walls.glsl).
vec3 lw_world(vec3 v, float w) { return vec3(dot(lit.wto[0], vec4(v, w)), dot(lit.wto[1], vec4(v, w)), dot(lit.wto[2], vec4(v, w))); }
float lw_light(int i, vec3 p, vec3 n, float jit, float side) {
    return wall_light(p, n, lw_world(lit.pos_r[i].xyz, 1.0), jit, side, lit.wall, lit.wallz);
}
#ifdef RELIEF
// --normal-maps: the shading normal r_n (view space) is the mesh's smooth normal r_ng tilted by the
// tangent-space normal map. The game's vertices have no tangents: the tangent frame comes from the
// screen-space derivatives of the view position and of the map's texture coordinates (the
// "cotangent frame", C. Schueler, ShaderX5 / "Followup: Normal Mapping Without Precomputed
// Tangents" 2013), so it follows any texture coordinate transform the draw applies, and skinned
// meshes (whose positions and normals the compute pre-pass writes) need nothing extra. The map is
// OpenGL-convention (+Y = up the image); the title's texture v grows down the image, so +Y is -B.
// r_spec: the specular map's intensity (R) and gloss (G), 0 without one; with --dry-highlights (flag
// 4) a texel with material (B > 0) takes its shine (B) and gloss (A) instead, and r_dry is set. Set
// up at the top of main, in uniform control flow (derivatives and implicit-LOD lookups before any
// discard).
// r_env: the baked (vertex) light at the texel, which a dry-highlights texel reflects at grazing
// angles (set in main).
vec3 r_n, r_ng;
vec2 r_spec;
bool r_dry;
vec3 r_env;
// --parallax (flag 8, depth lit.opts.y; world draws only): r_par is set, the stage r_pst's lookups
// (sample_stage) read at r_puv with the unshifted derivatives r_duv1 / r_duv2.
bool r_par = false;
uint r_pst = 0u;
vec2 r_puv, r_duv1, r_duv2;
// Parallax occlusion mapping: where the view ray meets the height field (the normal map's alpha,
// 1 = the surface, 0 = `depth` below it) under the texel uv. The texture's tangent frame comes from
// the derivatives: gu, gv are the surface gradients of u and v (texture units per view unit), so a
// ray going d view units deep moves the coordinates by -(gu.V, gv.V) d / (n.V); `depth` is in
// texture units, d = depth / s with s the coordinates' mean scale. The march has 4 to 12 steps by
// the shift's length in pixels, refined linearly between the last two; it fades out where the
// shift is under a pixel or the surface is seen almost edge on, and n.V is taken as at least 0.3
// (the shift at most about 3 x depth: no swimming at grazing angles, little bleeding in atlases).
vec2 relief_parallax(vec2 uv, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2, vec3 n, float lod) {
    vec3 c2 = cross(dp2, n), c1 = cross(n, dp1);
    float det = dot(dp1, c2);
    if (abs(det) < 1e-30) return uv;
    vec3 gu = (c2 * duv1.x + c1 * duv2.x) / det, gv = (c2 * duv1.y + c1 * duv2.y) / det;
    float sc = sqrt(0.5 * (dot(gu, gu) + dot(gv, gv)));
    vec3 V = normalize(-v_view);
    float nv = dot(n, V);
    if (!(sc > 0.0) || nv <= 0.05) return uv;
    vec2 shift = -vec2(dot(gu, V), dot(gv, V)) / (sc * max(nv, 0.3)) * lit.opts.y;
    float px = length(shift) / max(max(length(duv1), length(duv2)), 1e-12);   // the shift in pixels
    float fade = smoothstep(0.5, 1.5, px) * smoothstep(0.05, 0.15, nv);
    if (fade <= 0.0) return uv;
    shift *= fade;
    const float steps = clamp(ceil(px * fade * 0.3), 4.0, 12.0);
    vec2 duv = shift / steps;
    float dl = 1.0 / steps, layer = 0.0;
    vec2 p = uv;
    // The march reads one mip level (the texel's own, as an isotropic lookup would pick): explicit
    // gradients cost several times as much per lookup on some GPUs.
    float d = 1.0 - textureLod(relief_map, p, lod).a, prev = d;
    for (int i = 0; i < 12 && layer < d; i++) {
        p += duv;
        layer += dl;
        prev = d;
        d = 1.0 - textureLod(relief_map, p, lod).a;
    }
    if (layer <= 0.0) return uv;   // the surface's top: no shift
    float after = d - layer, before = prev - (layer - dl);
    float w = after / min(after - before, -1e-6);
    return p - duv * clamp(w, 0.0, 1.0);
}
void relief_setup() {
    const uint word = uint(draw.relief.z);
    const uint st = word & 3u, flags = word >> 2;
    vec4 t = st == 0u ? v_t0 : st == 1u ? v_t1 : st == 2u ? v_t2 : v_t3;
    vec2 uv = t.xy / (t.w != 0.0 ? t.w : 1.0);
    vec3 dp1 = dFdx(v_view), dp2 = dFdy(v_view);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 n = v_nrm;
    float nn = dot(n, n);
    n = nn > 1e-12 ? n * inversesqrt(nn) : vec3(0.0, 0.0, 1.0);
    vec4 nm, sm;
    if ((flags & 8u) != 0u && draw.lights != 0u && lit.opts.y > 0.0) {   // --parallax (uniform per draw)
        r_par = true;
        r_pst = st;
        r_duv1 = duv1;
        r_duv2 = duv2;
        r_puv = relief_parallax(uv, dp1, dp2, duv1, duv2, n, textureQueryLod(relief_map, uv).y);
        nm = textureGrad(relief_map, r_puv, duv1, duv2);
        sm = textureGrad(spec_map, r_puv, duv1, duv2);
    } else {
        nm = texture(relief_map, uv);
        sm = texture(spec_map, uv);
    }
    r_ng = r_n = n;
    r_spec = (flags & 2u) != 0u ? sm.rg : vec2(0.0);
    r_dry = (flags & 4u) != 0u && sm.b > 0.0;
    if (r_dry) r_spec = sm.ba;
    if ((flags & 1u) == 0u) return;
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float m = max(dot(T, T), dot(B, B));
    if (!(m > 1e-30)) return;   // no texture gradient (a constant coordinate): flat
    // The frame comes out scaled by the screen-space area's orientation against n: Vulkan's y-down
    // window flips it (and a mirrored mapping is handled the same way).
    float k = (dot(cross(dp1, dp2), n) < 0.0 ? -1.0 : 1.0) * inversesqrt(m);
    vec3 ts = nm.xyz * 2.0 - 1.0;
    ts.xy *= draw.relief.x;
    vec3 p = T * (k * ts.x) - B * (k * ts.y) + n * max(ts.z, 0.0);
    float pp = dot(p, p);
    r_n = pp > 1e-12 ? p * inversesqrt(pp) : n;
    // Glass, chrome and paint are smooth: the relief estimated from their colour (streaks, dirt) fades
    // out as the material's gloss rises, so their highlights stay clean instead of sparkling.
    if (r_dry) r_n = normalize(mix(r_n, n, smoothstep(0.7, 1.0, r_spec.y)));
}
// Blinn-Phong highlights of the frame's lights at the shading normal, scaled by the specular map
// (R intensity; G gloss: exponent 8 to 512, energy-normalised by (e + 8) / (8 pi)) and the specular
// strength; through the lamp pass's soft knee and the character or world gain. lights: the world
// draw's light mask (0 = character mode: every light in use).
// --dry-highlights (normal_maps.h): the material's highlight, intensity 2 x shine, exponent
// 2^(3 + 7 gloss) (8-1024), reaching DRY_REACH x each light's radius (the falloff stretched over it),
// plus for glossy texels (gloss above 0.5) a soft Phong glint of the light (exponent 16, Fresnel-
// weighted: stronger at grazing angles), the blurred reflection of the lamp's surroundings that
// makes glass and paint read as such; through the soft knee and the gain as below. Glossy texels also
// reflect the baked light around them (r_env), Fresnel-weighted, so a car or a window in a lit spot
// keeps a sheen along its edges with no lamp near.
const float DRY_REACH = 2.0;   // renderer.h RENDERER_DRY_REACH
vec3 relief_dry_specular(uint lights) {
    vec3 V = normalize(-v_view);
    float e = exp2(mix(3.0, 10.0, r_spec.y));
    float norm = (e + 8.0) * 0.0397887;
    float nv = clamp(dot(r_n, V), 0.0, 1.0), fv = 1.0 - nv;
    float glint = smoothstep(0.5, 1.0, r_spec.y) * (0.25 + 0.75 * fv * fv * fv * fv * fv) * 1.4;   // x (16 + 2) / 2pi / 2
    vec3 R = reflect(-V, r_n);
    int count = int(lit.cam.z);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < count && i < CHAR_LIT_MAX; i++) {
        if (lights != 0u && ((lights >> uint(i)) & 1u) == 0u) continue;
        vec4 lr = lit.pos_r[i];
        vec3 v = lr.xyz - v_view;
        float d2 = dot(v, v), r2 = lr.w * lr.w * (DRY_REACH * DRY_REACH);
        if (d2 >= r2) continue;
        float x2 = d2 / r2, a = 1.0 - x2;
        float f = a * a / (1.0 + lit.params.x * x2);   // lamp_lights.h lamp_falloff over the longer reach
        vec3 L = v * inversesqrt(max(d2, 1e-8));
        float ndl = dot(r_n, L);
        if (ndl <= 0.0) continue;
        vec3 H = normalize(L + V);
        float rl = max(dot(R, L), 0.0), rl2 = rl * rl, rl4 = rl2 * rl2, rl8 = rl4 * rl4;
        sum += lit.col[i].rgb * (f * ndl * (norm * pow(max(dot(r_n, H), 0.0), e) + glint * rl8 * rl8));
    }
    sum *= 2.0 * r_spec.x * draw.relief.y;
    sum = sum / (1.0 + 0.5 * sum);
    float fe = smoothstep(0.5, 1.0, r_spec.y) * (0.1 + 0.9 * fv * fv * fv * fv * fv);
    return sum * (lights != 0u ? lit.params.z : lit.cam.w) + r_env * (fe * r_spec.x * draw.relief.y);
}
vec3 relief_specular(uint lights) {
    if (r_spec.x <= 0.0 || draw.relief.y <= 0.0) return vec3(0.0);
    if (r_dry) return relief_dry_specular(lights);
    vec3 V = normalize(-v_view);
    float e = exp2(mix(3.0, 9.0, r_spec.y));
    float norm = (e + 8.0) * 0.0397887;
    int count = int(lit.cam.z);
    vec3 sum = vec3(0.0);
    bool walls = lit.wall.w > 0.5;
    vec3 pw = walls ? lw_world(v_view, 1.0) : vec3(0.0), nw = walls ? lw_world(r_ng, 0.0) : vec3(0.0);
    int tested = 0;
    for (int i = 0; i < count && i < CHAR_LIT_MAX; i++) {
        if (lights != 0u && ((lights >> uint(i)) & 1u) == 0u) continue;
        vec4 lr = lit.pos_r[i];
        vec3 v = lr.xyz - v_view;
        float d2 = dot(v, v), r2 = lr.w * lr.w;
        if (d2 >= r2) continue;
        float x2 = d2 / r2, a = 1.0 - x2;
        float f = a * a / (1.0 + lit.params.x * x2);   // lamp_lights.h lamp_falloff
        vec3 L = v * inversesqrt(max(d2, 1e-8));
        float ndl = dot(r_n, L);
        if (ndl <= 0.0) continue;
        vec3 H = normalize(L + V);
        vec3 h = lit.col[i].rgb * (f * ndl * norm * pow(max(dot(r_n, H), 0.0), e));
        // Light walls: no highlight from a light behind a wall (visible ones only, LW_TESTED at most).
        if (walls && tested < LW_TESTED && max(h.r, max(h.g, h.b)) * r_spec.x * draw.relief.y > LW_MIN) {
            tested++;
            if (lw_light(i, pw, nw, 0.5, 0.0) < 0.5) continue;
        }
        sum += h;
    }
    sum *= r_spec.x * draw.relief.y;
    sum = sum / (1.0 + 0.5 * sum);
    return sum * (lights != 0u ? lit.params.z : lit.cam.w);
}
#endif
#ifdef HAIR
#include "hair.glsl"
#endif
#ifdef SKIN
#include "skin.glsl"
#endif
#ifdef MAT
#include "mat.glsl"
#endif
vec3 char_light() {
    vec3 n = v_nrm;
    float nn = dot(n, n);
    n = nn > 1e-12 ? n * inversesqrt(nn) : vec3(0.0, 0.0, 1.0);
#ifdef RELIEF
    n = r_n;
#endif
#ifdef HAIR
    if (h_w > 0.0) hair_frame(n);
#endif
#ifdef SKIN
    if (s_w > 0.0) skin_frame(n);
#endif
#ifdef MAT
    if (m_any) mat_frame(n);
#endif
    int count = int(lit.cam.z);
    vec3 sum = vec3(0.0);
    bool walls = lit.wall.w > 0.5;
    vec3 pw = walls ? lw_world(v_view, 1.0) : vec3(0.0), nw = walls ? lw_world(n, 0.0) : vec3(0.0);
    int tested = 0;
    for (int i = 0; i < count && i < CHAR_LIT_MAX; i++) {
        vec4 lr = lit.pos_r[i];
        vec3 v = lr.xyz - v_view;
        float d2 = dot(v, v), r2 = lr.w * lr.w;
        if (d2 >= r2) continue;
        float x2 = d2 / r2, a = 1.0 - x2;
        float f = a * a / (1.0 + lit.params.x * x2);   // lamp_lights.h lamp_falloff
        float ndl = dot(n, v * inversesqrt(max(d2, 1e-8)));
        float lam = clamp((ndl + lit.params.y) / (1.0 + lit.params.y), 0.0, 1.0);
        float term = f * lam * lam * (3.0 - 2.0 * lam);   // smooth terminator: no hard edge where the wrap ends
        // Light walls: a character behind a wall from the light gets none of it (one way per pixel).
#if defined(HAIR) || defined(SKIN)
        float wl = 1.0;   // the walls' share, for the hair's highlights too
#endif
#ifdef SKIN
        // skin: the light through it reaches the side away from the light too
        if (walls && tested < LW_TESTED && max(term, s_w > 0.0 ? f : 0.0) * max(lit.col[i].r, max(lit.col[i].g, lit.col[i].b)) > LW_MIN) {
#else
        if (walls && tested < LW_TESTED && term * max(lit.col[i].r, max(lit.col[i].g, lit.col[i].b)) > LW_MIN) {
#endif
            tested++;
#if defined(HAIR) || defined(SKIN)
            wl = lw_light(i, pw, nw, 0.5, 0.0);
            term *= wl;
#else
            term *= lw_light(i, pw, nw, 0.5, 0.0);
#endif
        }
        sum += lit.col[i].rgb * term;
#ifdef HAIR
        if (h_w > 0.0) h_spec += lit.col[i].rgb * (f * wl) * hair_lobes(v * inversesqrt(max(d2, 1e-8)), ndl);
#endif
#ifdef SKIN
        if (s_w > 0.0) sum += skin_light(lit.col[i].rgb, f * wl, term, v * inversesqrt(max(d2, 1e-8)), ndl);
#endif
#ifdef MAT
        if (m_any) mat_light(lit.col[i].rgb, f * wl, v * inversesqrt(max(d2, 1e-8)), ndl);
#endif
    }
    sum = sum / (1.0 + 0.5 * sum);   // the lamp pass's soft knee
    return sum * lit.cam.w;
}
#include "world_lit.glsl"
#endif

// Registers by number; accessed through switches so that constant numbers fold into plain
// variables (no array in scratch memory).
vec4 rc0, rc1, rfog, rv0, rv1, rt0, rt1, rt2, rt3, rr0, rr1, rsum, ref;

vec4 reg(uint i) {
    switch (i) {
    case 1u: return rc0;
    case 2u: return rc1;
    case 3u: return rfog;
    case 4u: return rv0;
    case 5u: return rv1;
    case 8u: return rt0;
    case 9u: return rt1;
    case 10u: return rt2;
    case 11u: return rt3;
    case 12u: return rr0;
    case 13u: return rr1;
    case 14u: return rsum;
    case 15u: return ref;
    default: return vec4(0.0);
    }
}
// Writable registers (v0, v1, t0-t3, r0, r1); others ignore the write.
void set_rgb(uint i, vec3 x) {
    switch (i) {
    case 4u: rv0.rgb = x; break;
    case 5u: rv1.rgb = x; break;
    case 8u: rt0.rgb = x; break;
    case 9u: rt1.rgb = x; break;
    case 10u: rt2.rgb = x; break;
    case 11u: rt3.rgb = x; break;
    case 12u: rr0.rgb = x; break;
    case 13u: rr1.rgb = x; break;
    default: break;
    }
}
void set_alpha(uint i, float x) {
    switch (i) {
    case 4u: rv0.a = x; break;
    case 5u: rv1.a = x; break;
    case 8u: rt0.a = x; break;
    case 9u: rt1.a = x; break;
    case 10u: rt2.a = x; break;
    case 11u: rt3.a = x; break;
    case 12u: rr0.a = x; break;
    case 13u: rr1.a = x; break;
    default: break;
    }
}

vec4 argb(uint c) { return vec4((c >> 16) & 0xFFu, (c >> 8) & 0xFFu, c & 0xFFu, c >> 24) / 255.0; }

bool shadow_cmp(float texel, float ref) {
    switch (SHADOW & 7u) {
    case 1u: return texel < ref;
    case 2u: return texel == ref;
    case 3u: return texel <= ref;
    case 4u: return texel > ref;
    case 5u: return texel != ref;
    case 6u: return texel >= ref;
    case 7u: return true;
    default: return false;
    }
}
vec4 gather_stage(uint i, vec2 st) {
    if (i == 0u) return textureGather(tex0, st, 0);
    if (i == 1u) return textureGather(tex1, st, 0);
    if (i == 2u) return textureGather(tex2, st, 0);
    return textureGather(tex3, st, 0);
}
vec2 stage_size(uint i) {
    if (i == 0u) return vec2(textureSize(tex0, 0));
    if (i == 1u) return vec2(textureSize(tex1, 0));
    if (i == 2u) return vec2(textureSize(tex2, 0));
    return vec2(textureSize(tex3, 0));
}
// One bilinear percentage-closer tap: the four compares of the footprint at st, weighted.
float shadow_tap(uint i, vec2 st, float ref, float maxd) {
    // Gather order: x (u0, v1), y (u1, v1), z (u1, v0), w (u0, v0) of the bilinear footprint.
    vec4 d = floor(gather_stage(i, st) * maxd + 0.5);
    vec4 c = vec4(shadow_cmp(d.x, ref), shadow_cmp(d.y, ref), shadow_cmp(d.z, ref), shadow_cmp(d.w, ref));
    if ((SHADOW & 0x2000u) != 0u && ((SHADOW & 7u) == 4u || (SHADOW & 7u) == 6u)) {
        // Shadow range: with the light projection r = A q - B of the draw, a stored depth d lies
        // at view depth B / (A - d), so the receiver is B (ref - d) / ((A - ref)(A - d)) view units
        // behind it; from SHADOW_RANGE to 1.5 x SHADOW_RANGE behind, the compare fades to lit.
        float a = ps[draw.program].shadow_depth[0], b = ps[draw.program].shadow_depth[1];
        if (a > 0.0) {
            vec4 behind = b * (ref - d) / ((a - ref) * (a - d));
            c = max(c, smoothstep(SHADOW_RANGE, 1.5 * SHADOW_RANGE, behind));
        }
    }
    vec2 f = fract(st * stage_size(i) - 0.5);
    return mix(mix(c.w, c.z, f.x), mix(c.x, c.y, f.x), f.y);
}
vec4 shadow_stage(uint i, vec4 t, uint mode) {
    float maxd = ((SHADOW >> (8u + i)) & 1u) != 0u ? 16777215.0 : 65535.0;
    float q = t.w != 0.0 ? t.w : 1.0;
    vec2 st = t.xy / q;
    float ref = mode == 2u ? clamp(t.z / q, 0.0, maxd) : 0.0;
    if ((SHADOW & 0x1000u) == 0u) return vec4(shadow_tap(i, st, ref, maxd));
    // Soft (RendererConfig.shadow_soft): 3x3 bilinear taps one texel apart, averaged (a tent over
    // 4x4 texels).
    vec2 texel = 1.0 / stage_size(i);
    float sum = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) sum += shadow_tap(i, st + vec2(x, y) * texel, ref, maxd);
    return vec4(sum / 9.0);
}

#define LSH_FALLOFF_K 4.0
#define LSH_DOMINANT vec2(0.95, 1.0)
#define LSH_RISE vec2(0.5, 1.0)
// Lamp shadows: the share of the occlusion at one ray point (st, ref: its depth-stage coordinates),
// over the bilinear footprint: a stored caster nearer the light than the point by less than the
// caster height (view units of the light: B (ref - d) / ((A - ref)(A - d))) holds the point inside
// a character. Nothing stored (the border, 1.0) never occludes. top: the occluding texels' nearest
// stored depth (the caster's top), when the share is above 0.
float lamp_occ_tap(uint i, vec2 st, float ref, float maxd, out float top) {
    float a = lsh.params.x, b = lsh.params.y;
    vec4 d = floor(gather_stage(i, st) * maxd + 0.5);
    vec4 behind = b * (ref - d) / ((a - ref) * (a - d));
    vec4 c = step(vec4(0.02), behind) * (1.0 - step(vec4(lsh.shape.x), behind));
    vec4 dc = mix(vec4(maxd), d, c);
    top = min(min(dc.x, dc.y), min(dc.z, dc.w));
    vec2 f = fract(st * stage_size(i) - 0.5);
    return mix(mix(c.w, c.z, f.x), mix(c.x, c.y, f.x), f.y);
}
// The lamp's brightness at a point v away from it (the lamp falloff, LSH_FALLOFF_K) for a light of
// radius r and weight wt.
float lamp_bright(vec3 v, float r, float wt) {
    float x2 = min(dot(v, v) / (r * r), 1.0);
    return wt * (1.0 - x2) * (1.0 - x2) / (1.0 + LSH_FALLOFF_K * x2);
}
// Light j's share of the shadow of the caster at world point c: one lamp per character. Each light
// above c (from LSH_RISE.x to LSH_RISE.y higher) has its brightness there; lights below
// LSH_DOMINANT.x of the brightest cast nothing, and the shares of the others are normalised (they
// add up to 1), so a character exactly between two equal lamps gets each one's shadow at half strength
// and walking from one lamp to the next crossfades. Chosen at the caster, not at the ground point: all
// of a character's shadow comes from the same lamp, whichever lamp is nearer each part of the ground.
float lamp_share(int j, vec3 c) {
    int n = int(lsh.params.w);
    float b[LSH_MAX];
    float top = 0.0;
    for (int k = 0; k < LSH_MAX; k++) {
        b[k] = 0.0;
        if (k >= n) continue;
        vec3 v = lsh.pos[k].xyz - c;
        b[k] = lamp_bright(v, lsh.pos[k].w, lsh.wt[k].x) * smoothstep(LSH_RISE.x, LSH_RISE.y, v.z);
        top = max(top, b[k]);
    }
    if (top <= 0.0) return 0.0;
    float sum = 0.0, mine = 0.0;
    for (int k = 0; k < LSH_MAX && k < n; k++) {
        float s = smoothstep(LSH_DOMINANT.x, LSH_DOMINANT.y, b[k] / top);
        sum += s;
        if (k == j) mine = s;
    }
    return sum > 0.0 ? mine / sum : 0.0;
}
// The factor on the vertex diffuse colour of a receiver fragment whose depth stage i has the
// coordinates t (lit: the title's own compare there, 1 = lit). Each light within its radius of the
// fragment has a shadow weight (its weight, fading out from half the radius to the radius, and in
// from LSH_RISE.x to LSH_RISE.y above the fragment: a light on the ground throws no character
// shadow). For each such light a ray from the fragment towards it is marched through the shadow map
// as a height field; where it first meets a caster, that caster's lamp share (lamp_share, at the
// caster's middle: its stored top less LSH_MID, once per ray) scales the occlusion, so each character
// casts the shadow of its own dominant lamp only. The darkest of the lights' shadows is kept (overlaps do not stack).
// Low lights (lamp_low_fade): the light's elevation seen from that caster's base scales the share
// and the shadow's length, so a fire or lamp low beside a character throws no long wedge.
// The march's steps are at most LSH_SPACING world units apart along the ground (a character's outline
// in the light's top-down view is about 0.25 units across): with fewer, wider steps each step that
// lands on the character gave its own copy of the shadow, a row of copies with lit gaps between them.
// LSH_STEPS is the fewest steps, LSH_STEPS_MAX the most (a 3.75-unit march at 0.12).
#define LSH_STEPS 12
#define LSH_STEPS_MAX 32
#define LSH_SPACING 0.12
#define LSH_MID 0.9
#define LSH_REACH vec2(0.15, 0.3)
// A light's shadow on a face fades in as the light rises from the face's plane (the cosine of the
// angle between the face's normal and the light, from x to y): behind the face, none.
#define LSH_FACING vec2(0.0, 0.1)
// A light's weight from which it takes the title's shadow's place (lamp_shadow `title`): none below
// x, all from y.
#define LSH_TITLE vec2(0.05, 0.3)
// The low-light fade of a light at v from a caster's base (lsh.fade): 0 below the min angle above
// the horizon, 1 from the full angle (smoothstep between); 1 when the fade is off.
float lamp_low_fade(vec3 v) {
    if (lsh.fade.y <= lsh.fade.x) return 1.0;
    float elev = degrees(atan(v.z, length(v.xy)));
    return smoothstep(lsh.fade.x, lsh.fade.y, elev);
}
// The receiver face's normal (world), turned towards the camera, from the screen-space derivatives
// of its world position (from its depth-stage coordinates t): 0 where they give no plane (a constant
// position) or the position is unknown. In uniform control flow only.
vec3 lamp_face(vec4 t) {
    vec4 pw = vec4(dot(lsh.inv[0], t), dot(lsh.inv[1], t), dot(lsh.inv[2], t), dot(lsh.inv[3], t));
    vec3 p = pw.xyz / (abs(pw.w) < 1e-30 ? 1.0 : pw.w);
    vec3 n = cross(dFdx(p), dFdy(p));
    float nn = dot(n, n);
    if (abs(pw.w) < 1e-30 || nn < 1e-24) return vec3(0.0);
    n *= inversesqrt(nn);
    return dot(n, lsh.eye.xyz - p) < 0.0 ? -n : n;
}
// face: the receiver's face normal (lamp_face; 0 = unknown, every light may shadow it). A light
// behind the face (the receiver turned away from it: the near side of a door or wall that light
// is on the far side of) does not light it, so it casts nothing there: without this, the march
// from a door's near side towards a light beyond the door met the characters out there and drew
// their shadows on the door.
// title: how much of the title's own character shadow gives way here (0-1): the strongest reach of a
// light at the fragment (radius fade, height above the fragment and face test, times the low-light
// fade of its elevation seen from the fragment; lights of weight LSH_TITLE.y and more count fully). Where a lamp
// casts the characters' shadows, the title's shadow along its fixed direction is a second, false
// one: main() fades it out by this, and this lamp shadow then
// darkens at full strength there (no dir_share). 0 without lights or with lsh.fade.w set.
float lamp_shadow(uint i, vec4 t, float lit, vec3 face, out float title) {
    title = 0.0;
    float maxd = ((SHADOW >> (8u + i)) & 1u) != 0u ? 16777215.0 : 65535.0;
    vec4 pw = vec4(dot(lsh.inv[0], t), dot(lsh.inv[1], t), dot(lsh.inv[2], t), dot(lsh.inv[3], t));
    if (abs(pw.w) < 1e-30) return 1.0;
    vec3 p = pw.xyz / pw.w;
    int n = int(lsh.params.w);
    // The steps' offset within their interval: with the same offset everywhere, the shadow was the
    // union of one copy of the caster's outline per step, its edges a chain of overlapping copies.
    // Each pixel of a 2 x 2 quad takes one of four evenly spaced offsets and main() averages the
    // quad (lamp_shadow_quad), so every pixel sees four times the steps. (A per-pixel noise offset
    // left the averaging to supersampling, and without it the edges were a hatched, dotted fringe.)
    uvec2 qp = uvec2(gl_FragCoord.xy) & 1u;
    float jit = (float(qp.x + 2u * qp.y) + 0.5) * 0.25;
    // Lights far dimmer here than the brightest one (below LSH_REACH of it) are not marched: the lamp
    // of a character within a shadow's length of the fragment lights the fragment too. Only a cost
    // gate (each marched light costs up to LSH_STEPS_MAX taps); which lamp casts is lamp_share's.
    float weight[LSH_MAX], bright[LSH_MAX];
    float top = 0.0;
    for (int j = 0; j < LSH_MAX; j++) {
        weight[j] = 0.0;
        bright[j] = 0.0;
        if (j >= n) continue;
        vec4 pr = lsh.pos[j];
        vec3 v = pr.xyz - p;
        weight[j] = lsh.wt[j].x * (1.0 - smoothstep(0.5 * pr.w, pr.w, length(v))) * smoothstep(LSH_RISE.x, LSH_RISE.y, v.z);
        if (dot(face, face) > 0.0) weight[j] *= smoothstep(LSH_FACING.x, LSH_FACING.y, dot(face, normalize(v)));
        bright[j] = lamp_bright(v, pr.w, lsh.wt[j].x);
        if (weight[j] > 1e-3) {
            top = max(top, bright[j]);
            // Whether this lamp casts the characters' shadows here, not how bright it is: a level
            // light of ordinary strength (weight 0.45) replaces the title's shadow all the same.
            title = max(title, weight[j] / max(lsh.wt[j].x, 1e-3) * smoothstep(LSH_TITLE.x, LSH_TITLE.y, lsh.wt[j].x) * lamp_low_fade(v));
        }
    }
    if (lsh.fade.w > 0.5) title = 0.0;
    lit = max(lit, title);
    if (top <= 0.0) return 1.0;
    ivec2 wsize = textureSize(lsh_walls, 0);
    float dark = 0.0;
    for (int j = 0; j < LSH_MAX && j < n; j++) {
        float w = weight[j] * smoothstep(LSH_REACH.x, LSH_REACH.y, bright[j] / top);
        if (w <= 1e-3 || w <= dark) continue;
        vec4 pr = lsh.pos[j];
        vec3 v = pr.xyz - p;
        float horiz = length(v.xy);
        // A light right above the fragment throws no ground shadow from here.
        if (horiz <= lsh.shape.z) continue;
        // From `start` away from the fragment until the ray is a caster's height above it, at the
        // light, or `length` away horizontally.
        float t0 = lsh.shape.z / horiz;
        float t1 = min(min(1.0, lsh.shape.x / v.z), lsh.shape.y / horiz);
        if (t0 >= t1) continue;
        vec4 lt = lsh.stq[j];
        int steps = clamp(int(ceil((t1 - t0) * horiz / LSH_SPACING)), LSH_STEPS, LSH_STEPS_MAX);
        float occ = 0.0, share = -1.0, len = lsh.shape.y;
        int walls_to = lsh.wall.w > 0.5 ? 0 : steps;   // the steps before it have no wall (checked)
        bool walled = false;
        for (int k = 0; k < steps; k++) {
            float s = mix(t0, t1, (float(k) + jit) / float(steps));
            vec4 h = mix(t, lt, s);   // the coordinates are linear in the homogeneous world position
            if (h.w <= 0.0) break;
            vec2 st = h.xy / h.w;
            float top;
            float o = lamp_occ_tap(i, st, h.z / h.w, maxd, top);
            if (o <= occ) continue;
            // A wall (the level's collision, lsh_walls) at a point of the ray blocks the light from
            // there on: a character beyond a wall or closed door casts nothing on this side, while what the ray already met keeps its occlusion (a
            // wall behind the character). Not within lsh.wallz.z of the fragment: the fragment's own
            // wall (a door or wall it lies on) fills its cell. Tested only up to a step that would
            // add occlusion (in step order: the same result as testing every step, without the
            // map taps of the rays that meet no caster).
            for (; walls_to <= k; walls_to++) {
                float sw = mix(t0, t1, (float(walls_to) + jit) / float(steps));
                vec4 hw = mix(pw, vec4(pr.xyz, 1.0), sw);
                vec3 q = hw.xyz / hw.w;
                float wtop;
                if (length(q.xy - p.xy) >= lsh.wallz.z && wall_at(q, lsh.wall, lsh.wallz, wsize, wtop)) {
                    walled = true;
                    break;
                }
            }
            if (walled) break;
            if (share < 0.0) {
                // The first caster on the ray (the one whose shadow this is): its top in the world (the
                // stored depth at st), then its middle, rates the lights once per ray; its base (its
                // position at the fragment's height) rates this light's elevation (lamp_low_fade).
                vec4 cw = vec4(dot(lsh.inv[0], vec4(st, top, 1.0)), dot(lsh.inv[1], vec4(st, top, 1.0)),
                               dot(lsh.inv[2], vec4(st, top, 1.0)), dot(lsh.inv[3], vec4(st, top, 1.0)));
                if (abs(cw.w) < 1e-30) break;
                vec3 c = cw.xyz / cw.w;
                float low = lamp_low_fade(vec3(pr.xy - c.xy, pr.z - p.z));
                share = low > 0.0 ? low * lamp_share(j, c - vec3(0.0, 0.0, LSH_MID)) : 0.0;
                if (share <= 0.0) break;   // not this caster's lamp, or a light too low
                len = lsh.shape.y * mix(lsh.fade.z, 1.0, low);
            }
            o *= 1.0 - smoothstep(0.6 * len, len, s * horiz);   // fades away from the character
            occ = max(occ, o * share);
            if (occ >= 0.999 * share) break;   // fully blocked: the rest of the ray cannot add more
        }
        dark = max(dark, w * occ);
    }
    if (dark <= 0.0) return 1.0;
    return 1.0 - lsh.params.z * dark * mix(lsh.shape.w, 1.0, clamp(lit, 0.0, 1.0));
}

// The mean of v over the fragment's 2 x 2 quad (fine derivatives: v's horizontal neighbour is
// v +- dFdxFine(v), then the same down the column of row means). In uniform control flow only.
float quad_mean(float v) {
    vec2 sg = vec2(1.0) - 2.0 * vec2(uvec2(gl_FragCoord.xy) & 1u);   // +1 even, -1 odd pixel
    float row = v + 0.5 * sg.x * dFdxFine(v);
    return row + 0.5 * sg.y * dFdyFine(row);
}

// World shadows: the factor on the vertex diffuse colour (1 = unshadowed) of a receiver fragment whose
// title compare is `title` (1 lit) and vertex diffuse alpha `back` (the title's oD1.w: 1 on faces
// turned from its light, which are never shadowed). Only where the title's own shadow does not
// already darken (its mux at 0.5), so the two never stack. The fragment's clip x, y, w comes from
// its window position and 1 / w; at a shadow's edge four bilinear percentage-closer taps a texel
// apart (a soft edge about three texels wide); faded with the distance from the camera, towards the
// map's edges, near lamps and where the baked colour is already dark.
float world_shadow(float title, float back) {
    if (wsh.k.x < 0.5 || back >= 0.5 || title < 0.5) return 1.0;
    float w = 1.0 / gl_FragCoord.w;
    vec2 ndc = vec2((gl_FragCoord.x - wsh.vp.x) * wsh.vp.z * 2.0 - 1.0, 1.0 - (gl_FragCoord.y - wsh.vp.y) * wsh.vp.w * 2.0);
    vec4 q = vec4(ndc * w, w, 1.0);
    vec3 m = vec3(dot(wsh.row[0], q), dot(wsh.row[1], q), dot(wsh.row[2], q));
    float edge = max(abs(m.x), abs(m.y));
    if (edge >= 1.0) return 1.0;
    vec3 P = vec3(dot(wsh.wrow[0], q), dot(wsh.wrow[1], q), dot(wsh.wrow[2], q));
    float fade = (1.0 - smoothstep(wsh.fade.x, wsh.fade.y, distance(P, wsh.eye.xyz))) * (1.0 - smoothstep(0.9, 1.0, edge));
    fade *= smoothstep(wsh.fade.z, wsh.fade.w, dot(rv0.rgb, vec3(0.299, 0.587, 0.114)));
    float lamp = 0.0;
    int n = int(wsh.eye.w);
    for (int j = 0; j < WSH_LAMPS && j < n; j++) {
        vec3 v = wsh.lamp[j].xyz - P;
        float x2 = min(dot(v, v) / (wsh.lamp[j].w * wsh.lamp[j].w), 1.0);
        lamp = max(lamp, (1.0 - x2) * (1.0 - x2));
    }
    fade *= (1.0 - wsh.lampk.x * lamp) * wsh.gate.x;   // and the indoor gate
    if (fade <= 0.0) return 1.0;
    vec2 uv = m.xy * 0.5 + 0.5;
    float zr = m.z - wsh.k.y;
    // The 2 x 2 texels around the point first: all lit or all blocked (most of the frame) is the
    // answer; at a shadow's edge the four taps follow.
    vec4 c0 = step(textureGather(wsh_map, uv, 0), vec4(zr));
    float occ = c0.x + c0.y + c0.z + c0.w;
    if (occ == 0.0) return 1.0;
    if (occ == 4.0) return 1.0 - wsh.k.z * fade;
    occ = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 st = uv + vec2((i & 1) == 0 ? -0.5 : 0.5, (i & 2) == 0 ? -0.5 : 0.5) / wsh.k.w;
        // Gather order: x (u0, v1), y (u1, v1), z (u1, v0), w (u0, v0); occluded where the stored
        // depth is nearer the light than the fragment (the cleared 1.0 never is).
        vec4 c = step(textureGather(wsh_map, st, 0), vec4(zr));
        vec2 f = fract(st * wsh.k.w - 0.5);
        occ += mix(mix(c.w, c.z, f.x), mix(c.x, c.y, f.x), f.y);
    }
    return 1.0 - wsh.k.z * 0.25 * occ * fade;
}

vec4 sample_stage(uint i, vec4 t, uint mode) {
    if (((SHADOW >> (4u + i)) & 1u) != 0u && (mode == 1u || mode == 2u)) return shadow_stage(i, t, mode);
    if (mode == 1u || mode == 2u) {   // project2d / project3d (no volume textures: 2D lookup)
        vec2 st = t.xy / (t.w != 0.0 ? t.w : 1.0);
#ifdef RELIEF
        if (r_par && i == r_pst) {   // --parallax: the relief stage where the view ray meets the height
            if (i == 0u) return textureGrad(tex0, r_puv, r_duv1, r_duv2);
            if (i == 1u) return textureGrad(tex1, r_puv, r_duv1, r_duv2);
            if (i == 2u) return textureGrad(tex2, r_puv, r_duv1, r_duv2);
            return textureGrad(tex3, r_puv, r_duv1, r_duv2);
        }
#endif
        if (i == 0u) return texture(tex0, st);
        if (i == 1u) return texture(tex1, st);
        if (i == 2u) return texture(tex2, st);
        return texture(tex3, st);
    }
    if (mode == 4u) return clamp(t, 0.0, 1.0);   // pass-through
    if (mode == 5u) {                             // clip plane: default compare mode, kill if < 0
        if (any(lessThan(t, vec4(0.0)))) discard;
        return vec4(0.0);
    }
    if (mode == 3u) return vec4(1.0);             // cube maps are not sampled
    return vec4(0.0);                             // none and the dot/bump modes
}

vec4 map_in(vec4 x, uint m) {
    switch (m) {
    case 0u: return max(x, 0.0);                       // unsigned identity
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);          // unsigned invert
    case 2u: return 2.0 * max(x, 0.0) - 1.0;           // expand normal
    case 3u: return -2.0 * max(x, 0.0) + 1.0;          // expand negate
    case 4u: return max(x, 0.0) - 0.5;                 // half-bias normal
    case 5u: return -max(x, 0.0) + 0.5;                // half-bias negate
    case 6u: return x;                                 // signed identity
    default: return -x;                                // signed negate
    }
}
vec3 in_rgb(uint b) {
    vec4 r = reg(b & 15u);
    return map_in((b & 16u) != 0u ? r.aaaa : r, (b >> 5) & 7u).rgb;
}
float in_alpha(uint b) {
    vec4 r = reg(b & 15u);
    return map_in(vec4((b & 16u) != 0u ? r.a : r.b), (b >> 5) & 7u).x;
}
vec4 map_out(vec4 x, uint m) {
    switch (m) {
    case 1u: x = x - 0.5; break;            // bias
    case 2u: x = x * 2.0; break;            // shift left 1
    case 3u: x = (x - 0.5) * 2.0; break;    // shift left 1 with bias
    case 4u: x = x * 4.0; break;            // shift left 2
    case 6u: x = x * 0.5; break;            // shift right 1
    default: break;
    }
    return clamp(x, -1.0, 1.0);
}

// One general combiner stage s with its input/output words.
void stage(uint p, uint s, uint ri, uint ai, uint ro, uint ao) {
    rc0 = argb(ps[p].c0[s]);
    rc1 = argb(ps[p].c1[s]);
    bool mux_cd = (COMBINERS & 0x100u) != 0u ? rr0.a >= 0.5 : (uint(rr0.a * 255.0 + 0.5) & 1u) == 1u;
    vec3 A = in_rgb(ri >> 24), B = in_rgb(ri >> 16), C = in_rgb(ri >> 8), D = in_rgb(ri);
    vec3 ab = (ro & 0x2000u) != 0u ? vec3(dot(A, B)) : A * B;
    vec3 cd = (ro & 0x1000u) != 0u ? vec3(dot(C, D)) : C * D;
    vec3 sum = (ro & 0x4000u) != 0u ? (mux_cd ? cd : ab) : ab + cd;
    uint rm = (ro >> 15) & 7u;
    ab = map_out(vec4(ab, 0.0), rm).rgb;
    cd = map_out(vec4(cd, 0.0), rm).rgb;
    sum = map_out(vec4(sum, 0.0), rm).rgb;
    float aA = in_alpha(ai >> 24), aB = in_alpha(ai >> 16), aC = in_alpha(ai >> 8), aD = in_alpha(ai);
    float aab = aA * aB, acd = aC * aD;
    float asum = (ao & 0x4000u) != 0u ? (mux_cd ? acd : aab) : aab + acd;
    uint am = (ao >> 15) & 7u;
    aab = map_out(vec4(aab), am).x;
    acd = map_out(vec4(acd), am).x;
    asum = map_out(vec4(asum), am).x;
    // All inputs were read above; write the rgb, then the alpha portions.
    set_rgb(ro & 15u, cd);
    set_rgb((ro >> 4) & 15u, ab);
    set_rgb((ro >> 8) & 15u, sum);
    set_alpha(ao & 15u, acd);
    set_alpha((ao >> 4) & 15u, aab);
    set_alpha((ao >> 8) & 15u, asum);
    if ((ro & 0x40000u) != 0u) set_alpha(ro & 15u, cd.b);
    if ((ro & 0x80000u) != 0u) set_alpha((ro >> 4) & 15u, ab.b);
}

// The general and final combiners of program p on the registers as set (rv0, rv1, rt0-rt3): the
// output colour. Stages may write rv0, rv1 and rt0-rt3 (callers that run it again restore them).
vec4 combine(uint p) {
    rc0 = rc1 = rsum = ref = vec4(0.0);
    rr0 = vec4(0.0, 0.0, 0.0, rt0.a);
    rr1 = vec4(0.0);
    const uint count = min(COMBINERS & 0xFFu, 8u);
    if (count > 0u) stage(p, 0u, RGB_IN0, ALPHA_IN0, RGB_OUT0, ALPHA_OUT0);
    if (count > 1u) stage(p, 1u, RGB_IN1, ALPHA_IN1, RGB_OUT1, ALPHA_OUT1);
    if (count > 2u) stage(p, 2u, RGB_IN2, ALPHA_IN2, RGB_OUT2, ALPHA_OUT2);
    if (count > 3u) stage(p, 3u, RGB_IN3, ALPHA_IN3, RGB_OUT3, ALPHA_OUT3);
    if (count > 4u) stage(p, 4u, RGB_IN4, ALPHA_IN4, RGB_OUT4, ALPHA_OUT4);
    if (count > 5u) stage(p, 5u, RGB_IN5, ALPHA_IN5, RGB_OUT5, ALPHA_OUT5);
    if (count > 6u) stage(p, 6u, RGB_IN6, ALPHA_IN6, RGB_OUT6, ALPHA_OUT6);
    if (count > 7u) stage(p, 7u, RGB_IN7, ALPHA_IN7, RGB_OUT7, ALPHA_OUT7);
    vec4 c;
    const uint fa = FINAL_ABCD, fe = FINAL_EFG;
    if (fa != 0u || fe != 0u) {
        rc0 = argb(ps[p].final_c0);
        rc1 = argb(ps[p].final_c1);
        vec3 v1 = (fe & 0x40u) != 0u ? 1.0 - clamp(rv1.rgb, 0.0, 1.0) : rv1.rgb;
        vec3 r0 = (fe & 0x20u) != 0u ? 1.0 - clamp(rr0.rgb, 0.0, 1.0) : rr0.rgb;
        vec3 s = v1 + r0;
        rsum = vec4((fe & 0x80u) != 0u ? clamp(s, 0.0, 1.0) : s, 0.0);
        vec3 E = clamp(in_rgb(fe >> 24), 0.0, 1.0), F = clamp(in_rgb(fe >> 16), 0.0, 1.0);
        ref = vec4(E * F, 0.0);
        vec3 A = clamp(in_rgb(fa >> 24), 0.0, 1.0), B = clamp(in_rgb(fa >> 16), 0.0, 1.0);
        vec3 C = clamp(in_rgb(fa >> 8), 0.0, 1.0), D = clamp(in_rgb(fa), 0.0, 1.0);
        c.rgb = clamp(A * B + (1.0 - A) * C + D, 0.0, 1.0);
        c.a = clamp(in_alpha(fe >> 8), 0.0, 1.0);
    } else {
        c = clamp(rr0, 0.0, 1.0);
    }
    return c;
}

void main() {
#ifdef RELIEF
    relief_setup();
#endif
#ifdef HAIR
    hair_setup();
#endif
#ifdef SKIN
    skin_setup();
#endif
#ifdef MAT
    mat_setup();
#endif
    uint p = draw.program;
    rc0 = rc1 = rsum = ref = vec4(0.0);
    rt0 = sample_stage(0u, v_t0, TEX_MODES & 31u);
    rt1 = sample_stage(1u, v_t1, (TEX_MODES >> 5) & 31u);
    rt2 = sample_stage(2u, v_t2, (TEX_MODES >> 10) & 31u);
    rt3 = sample_stage(3u, v_t3, (TEX_MODES >> 15) & 31u);
    rfog = argb(ps[p].fog);
    rfog.a *= 1.0 - clamp(v_fog, 0.0, 1.0);
    rv0 = clamp(v_d0, 0.0, 1.0);
#ifdef RELIEF
    // The game's own light (baked into the vertex colour) has no direction: it is taken as light from
    // the hemisphere around the mesh normal, whose share at a tilted normal is (1 + cos) / 2, so
    // grooves and slopes of the relief darken a little and flat parts keep their colour exactly.
    rv0.rgb *= mix(1.0, 0.5 + 0.5 * clamp(dot(r_n, r_ng), 0.0, 1.0), draw.relief.w);
    r_env = rv0.rgb;
#endif
#ifdef CHAR_LIT
    // The added light's share of the lit vertex colour (world draws; 0 without light there).
    vec3 added = vec3(0.0);
    if (draw.lights != 0u) {
        const vec3 own = rv0.rgb;
        vec3 walled;
        vec3 sum = world_light(draw.lights, walled);
        if (lit.wall.w > 0.5) {
            // Light walls: the share of the light the walls let through, averaged over the 2 x 2 quad
            // (its pixels marched with four step offsets and sideways shifts: a soft wall edge), times
            // this pixel's own light (its normal's detail kept). Not after a clip-plane stage's
            // discard (derivatives; one condition per use, see lamp_face's call).
            vec3 mo = sum, mw = walled;
            if ((TEX_MODES & 31u) != 5u && ((TEX_MODES >> 5) & 31u) != 5u && ((TEX_MODES >> 10) & 31u) != 5u &&
                ((TEX_MODES >> 15) & 31u) != 5u) {
                mo = vec3(quad_mean(sum.r), quad_mean(sum.g), quad_mean(sum.b));
                mw = vec3(quad_mean(walled.r), quad_mean(walled.g), quad_mean(walled.b));
            }
            sum *= mix(vec3(1.0), mw / max(mo, vec3(1e-6)), greaterThan(mo, vec3(1e-6)));
        }
        rv0.rgb = world_light_apply(own, world_light_knee(sum));
        added = mix(vec3(0.0), 1.0 - own / max(rv0.rgb, vec3(1e-6)), greaterThan(rv0.rgb, own));
    }
#endif
    float title_gone = 0.0;   // the title's shadow giving way to a lamp's (lamp_shadow `title`)
    if ((SHADOW & 0x4000u) != 0u) {   // lamp shadows on the first depth stage in PROJECT3D mode
        uint ls = (SHADOW & 0x10u) != 0u ? 0u : (SHADOW & 0x20u) != 0u ? 1u : (SHADOW & 0x40u) != 0u ? 2u : 3u;
        if (((TEX_MODES >> (5u * ls)) & 31u) == 2u) {
            vec4 lt = ls == 0u ? v_t0 : ls == 1u ? v_t1 : ls == 2u ? v_t2 : v_t3;
            vec4 lc = ls == 0u ? rt0 : ls == 1u ? rt1 : ls == 2u ? rt2 : rt3;
            // Averaged over the quad: its four pixels marched with the four step offsets (a
            // clip-plane stage may discard pixels first, so those draws keep their own value).
            // The receiver's face too (derivatives: not after a clip-plane stage's discard). (Written
            // as one condition per use: a shared bool of these spec-constant tests crashed the NVIDIA
            // 580 compiler.)
            vec3 face = vec3(0.0);
            if ((TEX_MODES & 31u) != 5u && ((TEX_MODES >> 5) & 31u) != 5u && ((TEX_MODES >> 10) & 31u) != 5u &&
                ((TEX_MODES >> 15) & 31u) != 5u)
                face = lamp_face(lt);
            float f = lamp_shadow(ls, lt, lc.x, face, title_gone);
            if ((TEX_MODES & 31u) != 5u && ((TEX_MODES >> 5) & 31u) != 5u && ((TEX_MODES >> 10) & 31u) != 5u &&
                ((TEX_MODES >> 15) & 31u) != 5u)
                f = quad_mean(f);
            rv0.rgb *= f;
        }
    }
    if ((SHADOW & 0x8000u) != 0u) {   // world shadows: receivers of the title's map (its first depth stage, PROJECT3D)
        uint ws = (SHADOW & 0x10u) != 0u ? 0u : (SHADOW & 0x20u) != 0u ? 1u : (SHADOW & 0x40u) != 0u ? 2u : 3u;
        if (((TEX_MODES >> (5u * ws)) & 31u) == 2u) {
            vec4 wc = ws == 0u ? rt0 : ws == 1u ? rt1 : ws == 2u ? rt2 : rt3;
            rv0.rgb *= world_shadow(wc.x, clamp(v_d1.a, 0.0, 1.0));
        }
    }
    rv1 = clamp(v_d1, 0.0, 1.0);
    const vec4 in_v0 = rv0, in_v1 = rv1, in_t0 = rt0, in_t1 = rt1, in_t2 = rt2, in_t3 = rt3;
    vec4 c = combine(p);
    // The title's character shadow (its depth stage's compare, the first one) darkens the colour
    // the combiners make, the light the port adds included: under a strong lamp the title's shadow
    // stayed as dark as anywhere. A shadow removes only the light its caster blocks, the title's own
    // (the level's shadow light); the port's lights have shadows of their own (--lamp-shadows). So
    // where the compare shadows the fragment, the combiners run again with it lit (`open`), and the
    // added light's share of the colour comes from there (main, after the lighting); lit fragments
    // and draws without a depth stage keep `open` = c exactly. --title-shadow-full: the old look.
    // Under a lamp that casts the characters' shadows (title_gone), the title's shadow itself fades
    // towards `open` too: one shadow per character, from the real light.
    vec4 open = c;
    bool want_open = title_gone > 0.0;
#ifdef CHAR_LIT
    want_open = want_open || lit.opts.x < 0.5;
#endif
    if ((SHADOW & 0xF0u) != 0u && want_open) {
        uint ts = (SHADOW & 0x10u) != 0u ? 0u : (SHADOW & 0x20u) != 0u ? 1u : (SHADOW & 0x40u) != 0u ? 2u : 3u;
        uint tm = (TEX_MODES >> (5u * ts)) & 31u;
        vec4 tc = ts == 0u ? in_t0 : ts == 1u ? in_t1 : ts == 2u ? in_t2 : in_t3;
        if ((tm == 1u || tm == 2u) && min(min(tc.x, tc.y), min(tc.z, tc.w)) < 1.0) {
            const vec4 k_v0 = rv0, k_v1 = rv1, k_t0 = rt0, k_t1 = rt1, k_t2 = rt2, k_t3 = rt3;
            rv0 = in_v0;
            rv1 = in_v1;
            rt0 = ts == 0u ? vec4(1.0) : in_t0;
            rt1 = ts == 1u ? vec4(1.0) : in_t1;
            rt2 = ts == 2u ? vec4(1.0) : in_t2;
            rt3 = ts == 3u ? vec4(1.0) : in_t3;
            open = combine(p);
            rv0 = k_v0;   // the first run's registers for what follows (the emission's texel)
            rv1 = k_v1;
            rt0 = k_t0;
            rt1 = k_t1;
            rt2 = k_t2;
            rt3 = k_t3;
        }
    }
    c.rgb = mix(c.rgb, open.rgb, title_gone);
    uint a = uint(c.a * 255.0 + 0.5), r = draw.alpha_ref;
    bool pass;
    switch (draw.alpha_func) {
    case 1u: pass = false; break;
    case 2u: pass = a < r; break;
    case 3u: pass = a == r; break;
    case 4u: pass = a <= r; break;
    case 5u: pass = a > r; break;
    case 6u: pass = a != r; break;
    case 7u: pass = a >= r; break;
    default: pass = true; break;
    }
    if (!pass) discard;
    out_color = c;
#ifdef CHAR_LIT
    // The added light, not darkened by the title's shadow (`open`: the colour as if the title's
    // compare were lit; = c where it is). Characters: c (1 + L) + (open - c) L, so c + open L.
    // World draws: the added share of the lit colour comes from `open`.
    if (draw.lights == 0u) {
        const vec3 L = char_light();
        out_color.rgb = out_color.rgb * (1.0 + L) + (open.rgb - c.rgb) * L;
#ifdef HAIR
        out_color.rgb = hair_apply(out_color.rgb, c.rgb, normalize(v_nrm));
#endif
#ifdef SKIN
        out_color.rgb = skin_apply(out_color.rgb, normalize(v_nrm));
#endif
#ifdef MAT
        out_color.rgb = mat_apply(out_color.rgb, c.rgb);
#endif
    } else {
        out_color.rgb += (open.rgb - c.rgb) * added;
    }
#endif
#ifdef RELIEF
    // Highlights are not tinted by the texture; fog hides them as it hides the surface.
#if defined(HAIR) && defined(MAT)
    // hair texels take the hair's highlights instead of the relief's (no plastic shine), eyes their glint
    out_color.rgb += relief_specular(draw.lights) * (1.0 - clamp(v_fog, 0.0, 1.0)) * (1.0 - min(h_w, 1.0)) * (1.0 - min(m_eye, 1.0));
#elif defined(MAT)
    out_color.rgb += relief_specular(draw.lights) * (1.0 - clamp(v_fog, 0.0, 1.0)) * (1.0 - min(m_eye, 1.0));
#elif defined(HAIR)
    // hair texels take the hair's highlights instead of the relief's (no plastic shine)
    out_color.rgb += relief_specular(draw.lights) * (1.0 - clamp(v_fog, 0.0, 1.0)) * (1.0 - min(h_w, 1.0));
#else
    out_color.rgb += relief_specular(draw.lights) * (1.0 - clamp(v_fog, 0.0, 1.0));
#endif
#endif
#ifdef EMIT
    // After the final combiner (its fog mix) and any lighting: the emitting texels keep at least
    // their own colour.
    uint est = uint(draw.emit.z);
    vec4 et = est == 0u ? v_t0 : est == 1u ? v_t1 : est == 2u ? v_t2 : v_t3;
    vec4 ec = est == 0u ? rt0 : est == 1u ? rt1 : est == 2u ? rt2 : rt3;
    float em = texture(emit_mask, et.xy / (et.w != 0.0 ? et.w : 1.0)).r;
    // A lit texel shows at least its own colour (through fog and darkness), then half of it again
    // screened on top, so the tube reads hotter than paint of the same colour.
    vec3 e = clamp(ec.rgb * (em * draw.emit.x), 0.0, 1.0);
    vec3 base = max(out_color.rgb, e);
    out_color.rgb = base + 0.5 * e * (1.0 - base);
#endif
#ifdef SOFT
    // Fade by how far the scene lies behind the fragment, in view depth (d = b / (a - z)); nothing
    // drawn there (depth 1): no fade.
    float zs = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
    if (zs < 1.0) {
        float ds = draw.soft.y / max(draw.soft.x - zs, 1e-6), df = draw.soft.y / max(draw.soft.x - gl_FragCoord.z, 1e-6);
        float f = clamp((ds - df) * draw.soft.z, 0.0, 1.0);
        out_color.a *= f;
        if (SOFT_RGB != 0u) out_color.rgb *= f;
    }
#endif
    if (MASK_CLEAR != 0u) out_color.a = 0.0;
#ifdef EMIT
    if (MASK_CLEAR != 0u) out_color.a = clamp(em * draw.emit.y, 0.0, 1.0);   // the bloom light mask
#endif
}
