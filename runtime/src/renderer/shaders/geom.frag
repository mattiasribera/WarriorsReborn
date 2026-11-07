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
// those with 24-bit depth, 12 soft filtering, 13 shadow range (0 = no depth stage).
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
vec3 char_light() {
    vec3 n = v_nrm;
    float nn = dot(n, n);
    n = nn > 1e-12 ? n * inversesqrt(nn) : vec3(0.0, 0.0, 1.0);
    int count = int(lit.cam.z);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < count && i < CHAR_LIT_MAX; i++) {
        vec4 lr = lit.pos_r[i];
        vec3 v = lr.xyz - v_view;
        float d2 = dot(v, v), r2 = lr.w * lr.w;
        if (d2 >= r2) continue;
        float x2 = d2 / r2, a = 1.0 - x2;
        float f = a * a / (1.0 + lit.params.x * x2);   // lamp_lights.h lamp_falloff
        float ndl = dot(n, v * inversesqrt(max(d2, 1e-8)));
        float lam = clamp((ndl + lit.params.y) / (1.0 + lit.params.y), 0.0, 1.0);
        sum += lit.col[i].rgb * (f * lam * lam * (3.0 - 2.0 * lam));   // smooth terminator: no hard edge where the wrap ends
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

vec4 sample_stage(uint i, vec4 t, uint mode) {
    if (((SHADOW >> (4u + i)) & 1u) != 0u && (mode == 1u || mode == 2u)) return shadow_stage(i, t, mode);
    if (mode == 1u || mode == 2u) {   // project2d / project3d (no volume textures: 2D lookup)
        vec2 st = t.xy / (t.w != 0.0 ? t.w : 1.0);
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

void main() {
    uint p = draw.program;
    rc0 = rc1 = rsum = ref = vec4(0.0);
    rt0 = sample_stage(0u, v_t0, TEX_MODES & 31u);
    rt1 = sample_stage(1u, v_t1, (TEX_MODES >> 5) & 31u);
    rt2 = sample_stage(2u, v_t2, (TEX_MODES >> 10) & 31u);
    rt3 = sample_stage(3u, v_t3, (TEX_MODES >> 15) & 31u);
    rfog = argb(ps[p].fog);
    rfog.a *= 1.0 - clamp(v_fog, 0.0, 1.0);
    rv0 = clamp(v_d0, 0.0, 1.0);
#ifdef CHAR_LIT
    if (draw.lights != 0u) rv0.rgb = world_light_apply(rv0.rgb, draw.lights);
#endif
    rv1 = clamp(v_d1, 0.0, 1.0);
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
    if (draw.lights == 0u) out_color.rgb *= 1.0 + char_light();
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
