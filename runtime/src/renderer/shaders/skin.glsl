// --skin (renderer_set_skin; geom.frag built with -DCHAR_LIT -DSKIN, plus -DRELIEF and / or -DHAIR
// for a draw with relief maps or a hair mask too): the skin's skin mask (src/hair.h) in set 2,
// binding 3, sampled at the stage-0 (skin) coordinates: R weight, G thinness (how much light passes
// through: the ears most), B the sweat sheen's weight (the face most). lit.skin.x: strength;
// lit.hair.xyz: world up in view space.
//
// Per light of the frame (char_light), on skin texels only:
// - the diffuse term wraps further in red than in green and blue, so the terminator turns warm and
//   soft as light scattered under the skin comes out past it (a cheap stand-in for pre-integrated
//   skin shading, Penner 2011);
// - light passing through thin parts towards the eye (Barre-Brisebois and Bouchard, "Approximating
//   translucency", GDC 2011): strongest with the light behind the surface, seen at the rims and in
//   the ears, deep red (the texel's colour x SKIN_DEEP), by the light's falloff and walls' share;
// - a broad, low sheen of the light (a film of sweat), a little stronger at grazing angles, and a
//   tighter glint where the sheen weight is high (the face), broken by the skin's own detail.
// Afterwards the skin texture's creases (darker than the texels around them) darken a little more
// and the surfaces facing down a little: more volume without a plastic look. Everything comes from
// the frame's lights (no light, no change but the creases) and scales with the weight and strength;
// a texel of weight 0 is drawn exactly as without skin.
layout(set = 2, binding = 3) uniform sampler2D skin_map;
const vec3 SKIN_WRAP = vec3(0.60, 0.20, 0.10);    // extra wrap per channel (over the Lambert wrap)
const float SKIN_TRANS = 3.0;                     // light through thin parts
const vec3 SKIN_DEEP = vec3(1.0, 0.32, 0.18);     // the colour light takes going through skin
const float SKIN_DISTORT = 0.35;                 // the light through bends towards the normal; cubed (sharper)
const float SKIN_SHEEN = 0.25, SKIN_SHEEN_E = 20.0;   // the broad sheen
const float SKIN_OIL = 0.20, SKIN_OIL_E = 90.0;       // the face's tighter glint
const float SKIN_CAVITY = 0.60;                   // creases: darkening per unit of relative detail
const float SKIN_UNDER = 0.88;                    // the brightness of skin facing straight down
float s_w = 0.0;                     // weight x strength; 0: not skin
float s_thin = 0.0, s_sheen = 0.0;   // the mask's thinness and sheen weight
float s_cav = 1.0;                   // the creases' factor
float s_pore = 0.0;                  // the texel's detail, -1 (a crease) .. 1 (a ridge)
vec3 s_alb = vec3(0.0);              // the skin texel's colour
vec3 s_n = vec3(0.0, 0.0, 1.0), s_v = vec3(0.0, 0.0, 1.0);   // shading normal, view direction (skin_frame)
vec3 s_spec = vec3(0.0), s_trans = vec3(0.0);                // the lights' sheen and light through (char_light)

float skin_luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
#ifdef MAT
// --eyes, --cloth: the mask's texture holds three masks stacked (skin, eyes, cloth); part's
// coordinates (v wrapped, kept half a texel inside the part: no bleeding between them).
vec2 mat_uv(vec2 uv, float part) {
    float h = float(textureSize(skin_map, 0).y) / 3.0;
    return vec2(uv.x, (clamp(fract(uv.y) * h, 0.5, h - 0.5) + part * h) / (3.0 * h));
}
#endif

// At the top of main (uniform control flow: derivatives first; the taps only on skin texels).
void skin_setup() {
    vec2 uv = v_t0.xy;
    vec2 gx = dFdx(uv), gy = dFdy(uv);
#ifdef MAT
    vec4 m = textureGrad(skin_map, mat_uv(uv, 0.0), gx * vec2(1.0, 1.0 / 3.0), gy * vec2(1.0, 1.0 / 3.0));
#else
    vec4 m = textureGrad(skin_map, uv, gx, gy);
#endif
    s_w = m.r * lit.skin.x;
    if (s_w <= 0.0) return;
    s_thin = m.g;
    s_sheen = m.b;
    s_alb = textureGrad(tex0, uv, gx, gy).rgb;
    // the texel against its surroundings (four taps two texels away)
    vec2 o = 2.0 / vec2(textureSize(tex0, 0));
    float lb = 0.25 * (skin_luma(textureGrad(tex0, uv + o, gx, gy).rgb) + skin_luma(textureGrad(tex0, uv - o, gx, gy).rgb) +
                       skin_luma(textureGrad(tex0, uv + vec2(o.x, -o.y), gx, gy).rgb) + skin_luma(textureGrad(tex0, uv + vec2(-o.x, o.y), gx, gy).rgb));
    float r = clamp(skin_luma(s_alb) / max(lb, 0.03), 0.5, 1.5);
    s_cav = clamp(1.0 + SKIN_CAVITY * (r - 1.0), 0.75, 1.08);
    s_pore = clamp((r - 1.0) * 4.0, -1.0, 1.0);
}
// In char_light, once per fragment, at its shading normal n.
void skin_frame(vec3 n) {
    s_n = n;
    s_v = normalize(-v_view);
}
// One light (col: its colour; fw: its falloff x walls' share; term: the plain diffuse term already
// added; l: unit direction; ndl = n . l). Returns the change of the diffuse light; collects the
// light through and the sheen.
vec3 skin_light(vec3 col, float fw, float term, vec3 l, float ndl) {
    vec3 w = lit.params.y + SKIN_WRAP;
    vec3 lam = clamp((ndl + w) / (1.0 + w), 0.0, 1.0);
    vec3 d = fw * lam * lam * (3.0 - 2.0 * lam);
    float back = clamp(dot(s_v, -normalize(l + s_n * SKIN_DISTORT)), 0.0, 1.0);
    back = back * back * back;   // cubed
    float rim = 1.0 - abs(dot(s_n, s_v));
    s_trans += col * (fw * back * (0.3 + 0.7 * rim * rim) * s_thin);
    vec3 h = normalize(l + s_v);
    float lg = log2(max(dot(s_n, h), 1e-4));
    float f1 = 1.0 - max(dot(h, s_v), 0.0), f2 = f1 * f1;
    float fres = f2 * f2 * f1;
    float vis = smoothstep(-0.05, 0.25, ndl);
    s_spec += col * (fw * vis * (SKIN_SHEEN * (1.0 + 2.0 * fres) * exp2(SKIN_SHEEN_E * lg) + SKIN_OIL * s_sheen * exp2(SKIN_OIL_E * lg)));
    return col * (d - term) * min(s_w, 1.0);
}
// After the lights (col: the lit colour; ng: the interpolated normal): the creases and the surfaces
// facing down, then the light through (soft knee and character gain as the diffuse light) and the
// sheen; fog hides them as it hides the surface.
vec3 skin_apply(vec3 col, vec3 ng) {
    if (s_w <= 0.0) return col;
    float k = min(s_w, 1.0), fog = 1.0 - clamp(v_fog, 0.0, 1.0);
    float down = 1.0;
    if (dot(ng, ng) > 1e-6) down = mix(SKIN_UNDER, 1.0, smoothstep(-0.8, 0.0, dot(normalize(ng), lit.hair.xyz)));
    col *= mix(1.0, s_cav * down, k * fog);
    vec3 tr = s_trans / (1.0 + 0.5 * s_trans) * lit.cam.w;
    vec3 sp = s_spec / (1.0 + 0.5 * s_spec) * lit.cam.w;
    return col + (tr * s_alb * SKIN_DEEP * SKIN_TRANS + sp * s_sheen * (0.8 + 0.2 * s_pore)) * s_w * fog;
}
