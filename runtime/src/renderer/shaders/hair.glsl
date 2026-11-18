// --hair (renderer_set_hair; geom.frag built with -DCHAR_LIT -DHAIR, and -DRELIEF for a draw with
// relief maps too): the skin's hair mask (src/hair.h) in set 2, binding 2, sampled at the stage-0
// (skin) coordinates: R weight, G height along the hair (0 the lowest, 1 the crown), B / A the
// strand direction in texel space (0.5 + 0.5 (du, dv)). lit.hair: world up in view space,
// strength.
//
// Kajiya-Kay ("Rendering fur with three dimensional textures", 1989) with Scheuermann's two
// shifted lobes ("Hair rendering and shading", GDC 2004): the strands run along T, the surface's
// world-down direction (world up projected on the tangent plane; the painted hair falls down the
// head), and a light of half vector H lights them as sin(T, H)^e. The primary lobe (tight, the
// light's colour) uses T tilted towards the tips, the secondary (wider, tinted by the hair, broken
// into glints) T tilted towards the roots, both jittered by the strand detail: the skin's own
// luminance across the strand direction (its painted strands), plus a little noise of one or two
// strands per texel that fades out once a strand would be under a pixel. The lower layers (the
// height) and the gaps between strands are darker. Everything scales with the weight and strength;
// a texel of weight 0 is drawn exactly as without hair.
layout(set = 2, binding = 2) uniform sampler2D hair_map;
const float HAIR_PRIMARY = 0.45;     // the primary lobe's peak (x the light's colour and falloff)
const float HAIR_SECONDARY = 0.55;   // the secondary one's (x the hair's tint)
const float HAIR_E1 = 80.0, HAIR_E2 = 20.0;
const float HAIR_SHIFT1 = -0.10, HAIR_SHIFT2 = 0.16, HAIR_JITTER = 0.12;
const float HAIR_SKY = 0.60;         // the sheen of the draw's own (baked) light from above
const float HAIR_LOW = 0.70;         // the lowest hair's brightness (the crown 1)
const float HAIR_GAP = 0.30;         // darkening in the gaps between strands
float h_w = 0.0;                     // weight x strength; 0: not hair
float h_h = 1.0;                     // height along the hair
float h_s = 0.0;                     // strand detail, -1 (a gap) .. 1 (a strand)
float h_glint = 1.0;                 // the secondary lobe's break-up
vec3 h_tint = vec3(0.0);             // the hair's hue (its texel's colour, brightest channel 1)
vec3 h_t1, h_t2, h_v;                // shifted strand directions, view direction (hair_frame)
float h_tw = 0.0;                    // the strand direction's reliability (0 at the crown)
vec3 h_spec = vec3(0.0);             // the lights' highlights (char_light)

float hair_hash(float x) { return fract(sin(x * 12.9898) * 43758.5453); }
float hair_noise(float x) {
    float i = floor(x), f = fract(x);
    return mix(hair_hash(i), hair_hash(i + 1.0), f * f * (3.0 - 2.0 * f)) * 2.0 - 1.0;
}
float hair_luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// At the top of main (uniform control flow: derivatives first; the taps only on hair texels).
void hair_setup() {
    vec2 uv = v_t0.xy;
    vec2 gx = dFdx(uv), gy = dFdy(uv);
    vec4 m = textureGrad(hair_map, uv, gx, gy);
    h_w = m.r * lit.hair.w;
    if (h_w <= 0.0) return;
    h_h = m.g;
    vec2 ts = vec2(textureSize(tex0, 0));
    vec2 d = m.ba * 2.0 - 1.0;
    float dl = length(d);
    d = dl > 1e-3 ? d / dl : vec2(0.0, 1.0);
    vec2 a = vec2(-d.y, d.x);   // across the strands, texel space
    vec2 o = a / ts;
    vec3 c0 = textureGrad(tex0, uv, gx, gy).rgb;
    float l0 = hair_luma(c0), l1 = hair_luma(textureGrad(tex0, uv + o, gx, gy).rgb), l2 = hair_luma(textureGrad(tex0, uv - o, gx, gy).rgb);
    float hp = l0 - 0.5 * (l1 + l2);   // > 0 on a painted strand, < 0 in a gap
    float s = dot(uv * ts, a) * 2.0;    // two strands per texel
    float fw = length(vec2(dot(gx * ts, a), dot(gy * ts, a))) * 2.0;
    float fade = 1.0 - smoothstep(0.5, 1.2, fw);
    float nz = hair_noise(s) * fade;
    h_s = clamp(hp / max(l0, 0.04) * 3.0, -1.0, 1.0) * 0.7 + 0.3 * nz;
    h_glint = 0.55 + 0.45 * (fade > 0.0 ? hair_noise(s * 0.37 + 17.0) * fade : 0.0);
    h_tint = c0 / max(max(c0.r, max(c0.g, c0.b)), 0.04);
}
// In char_light, once per fragment: the strand directions at its shading normal n.
void hair_frame(vec3 n) {
    vec3 up = lit.hair.xyz, t = up - n * dot(n, up);
    float tl = length(t);
    h_tw = smoothstep(0.12, 0.4, tl);
    t = tl > 1e-4 ? t / tl : vec3(0.0, 1.0, 0.0);
    h_t1 = normalize(t + n * (HAIR_SHIFT1 + HAIR_JITTER * h_s));
    h_t2 = normalize(t + n * (HAIR_SHIFT2 + HAIR_JITTER * h_s));
    h_v = normalize(-v_view);
}
float hair_kk(vec3 t, vec3 h, float e) {
    float th = dot(t, h);
    return pow(sqrt(max(1.0 - th * th, 0.0)), e);
}
// One light's highlights (unit direction l, n . l = ndl), before its colour and falloff.
vec3 hair_lobes(vec3 l, float ndl) {
    vec3 h = normalize(l + h_v);
    float vis = smoothstep(-0.15, 0.3, ndl) * h_tw;
    return vis * (HAIR_PRIMARY * hair_kk(h_t1, h, HAIR_E1) + HAIR_SECONDARY * hair_kk(h_t2, h, HAIR_E2) * h_glint * h_tint);
}
// After the lights (col: the lit colour; base: the combined colour without them): the darker lower
// layers and gaps, the highlights (soft knee and character gain as the diffuse light) and the
// sheen of the baked light from above; fog hides them as it hides the surface.
vec3 hair_apply(vec3 col, vec3 base, vec3 n) {
    if (h_w <= 0.0) return col;
    float k = min(h_w, 1.0);
    float occ = mix(HAIR_LOW, 1.0, h_h) * (1.0 - HAIR_GAP * max(-h_s, 0.0));
    col *= mix(1.0, occ, k);
    vec3 hs = h_spec / (1.0 + 0.5 * h_spec) * lit.cam.w;
    vec3 up = lit.hair.xyz;
    float sky = smoothstep(-0.2, 0.5, dot(n, up)) * h_tw;
    hs += HAIR_SKY * hair_luma(base) * sky * hair_kk(h_t1, normalize(up + h_v), HAIR_E2) * mix(vec3(1.0), h_tint, 0.5);
    return col + hs * h_w * occ * (1.0 - clamp(v_fog, 0.0, 1.0));
}
