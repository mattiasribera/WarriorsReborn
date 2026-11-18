// --eyes, --cloth (RendererConfig.eyes / .cloth; geom.frag built with -DCHAR_LIT -DSKIN -DMAT, plus
// -DRELIEF and / or -DHAIR): the skin mask's texture in set 2, binding 3 holds three masks stacked
// (src/hair.h hair_cache_mat_with): the skin's (read by skin.glsl through mat_uv), the eyes' (R
// weight, G / B the texel's offset from its eye's centre in the eye's half extents) and the
// clothes' (R leather, G denim, B cotton). lit.skin.y: the eyes' strength, lit.skin.z: the cloth's.
//
// Eyes: the painted eye sits on a flat, low-poly face. Its normal is bent over an eyeball (by the
// texel's offset from the eye's centre, along the surface's u and v directions: the cotangent frame
// of the derivatives, as the relief's), so each light of the frame (char_light) leaves one small,
// sharp, wet glint that moves over the eye as the light or the head moves; a faint catchlight from
// the camera's side (above and to the left) scaled by the draw's own (baked) light at the texel, so
// dark scenes keep dark eyes (the glint at least about a pixel and a half wide, so a small eye
// still shows it); and the lid edges (the eye's rim, the upper one more) slightly darker. Kept
// subtle: the glint and catchlight dim on an eye too small for a point
// (their spot would cover it), and together they add at most EYE_CAP of the light a white texel
// gets there (the texel's own lit colour over its albedo), so eye whites keep the title's
// brightness. A blink (draw.mat.x, renderer_set_eye_open: the eye-lid joint's turn) fades it all
// out: the shut lid is drawn over the eye's texels.
//
// Cloth, per light: leather a medium-gloss Blinn-Phong highlight on a normal bumped by the skin
// texture's own detail (its luminance as a height), so creases break it, and the creases (darker
// than the texels around them) a little darker, plus a broad sheen of the draw's own (baked) light
// from above on the same bumped normal (leather reads as leather with the lamp behind it too); denim a very low, broad sheen broken by a fine
// diagonal twill (faded out once a thread would be under a pixel); cotton matte with a faint sheen
// at grazing angles tinted by the cloth (a velvet-like rim, Oren-Nayar-lite). Only the frame's
// lights add light; texels of weight 0 are drawn as the skin variant.
const float EYE_GLINT = 1.0, EYE_E = 420.0;       // the lights' glint: peak (x light, falloff), exponent (at most: see m_e)
const float EYE_BULGE = 1.4;                      // the eyeball: how far the normal bends at the eye's rim
const float EYE_CATCH = 0.25;                     // the catchlight's peak (x the baked light, at most 1.5)
const float EYE_SPOT_E = 24.0;                    // below this exponent (an eye under ~15 pixels) the spot dims with its size
const float EYE_CAP = 0.40;                       // the glint and catchlight: at most this x the light at the texel
const float EYE_LID = 0.25;                       // the lid edges' darkening
const vec3 EYE_CATCH_DIR = vec3(-0.35, 0.45, 0.82);   // view space: the camera's upper left
const float LEATHER_SPEC = 0.55, LEATHER_E = 26.0;    // leather's highlight
const float LEATHER_BUMP = 3.0;                   // the detail's slope per unit of luminance per texel
const float LEATHER_CREASE = 0.55;                // creases: darkening per unit of relative detail
const float LEATHER_ENV = 0.45, LEATHER_ENV_E = 10.0; // the sheen of the draw's own light from above
const float DENIM_SHEEN = 0.10, DENIM_E = 6.0, DENIM_TWILL = 0.6;
const float COTTON_VELVET = 0.22;
float m_eye = 0.0, m_le = 0.0, m_de = 0.0, m_co = 0.0;   // weights x strengths
bool m_any = false;
vec2 m_off = vec2(0.0);              // the eye texel's offset from its eye's centre
vec3 m_tu = vec3(1.0, 0.0, 0.0), m_tv = vec3(0.0, 1.0, 0.0);   // the surface's u and v directions (view space)
vec2 m_grad = vec2(0.0);             // the texture's luminance slope (per texel)
float m_cav = 1.0, m_detail = 0.0, m_twill = 0.5;
float m_e = EYE_E;                   // the glint's exponent: at least about a pixel and a half wide
vec3 m_alb = vec3(0.0);
vec3 m_ne, m_nb, m_n, m_v;           // eyeball normal, bumped normal, shading normal, view direction
vec3 m_glint = vec3(0.0), m_spec = vec3(0.0), m_sheen = vec3(0.0);

float mat_luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// At the top of main, after skin_setup (uniform control flow: derivatives first).
void mat_setup() {
    vec2 uv = v_t0.xy;
    vec2 gx = dFdx(uv), gy = dFdy(uv);
    vec3 dp1 = dFdx(v_view), dp2 = dFdy(v_view);
    vec2 g3x = gx * vec2(1.0, 1.0 / 3.0), g3y = gy * vec2(1.0, 1.0 / 3.0);
    if (lit.skin.y > 0.0) {
        vec4 e = textureGrad(skin_map, mat_uv(uv, 1.0), g3x, g3y);
        m_eye = e.r * lit.skin.y * draw.mat.x;
        m_off = e.gb * 2.0 - 1.0;
        // an eye a few pixels wide: the glint's half width (~sqrt(2 ln 2 / e) radians) is kept at
        // least 1.5 pixels of the eyeball's bend, so it never falls between pixels
        float step = EYE_BULGE * max(length(dFdx(m_off)), length(dFdy(m_off))) * 1.5;
        m_e = min(EYE_E, 1.386 / max(step * step, 1e-6));
    }
    if (lit.skin.z > 0.0) {
        vec4 c = textureGrad(skin_map, mat_uv(uv, 2.0), g3x, g3y);
        m_le = c.r * lit.skin.z;
        m_de = c.g * lit.skin.z;
        m_co = c.b * lit.skin.z;
    }
    m_any = m_eye > 0.0 || m_le + m_de + m_co > 0.0;
    if (!m_any) return;
    // the surface's u and v directions (the gradients of u and v on it)
    vec3 n = v_nrm;
    float nn = dot(n, n);
    n = nn > 1e-12 ? n * inversesqrt(nn) : vec3(0.0, 0.0, 1.0);
    vec3 c2 = cross(dp2, n), c1 = cross(n, dp1);
    float det = dot(dp1, c2);
    if (abs(det) > 1e-30) {
        vec3 gu = (c2 * gx.x + c1 * gy.x) / det, gv = (c2 * gx.y + c1 * gy.y) / det;
        if (dot(gu, gu) > 1e-30) m_tu = normalize(gu);
        if (dot(gv, gv) > 1e-30) m_tv = normalize(gv);
    }
    m_alb = textureGrad(tex0, uv, gx, gy).rgb;
    if (m_le + m_de + m_co > 0.0) {
        // the texel against its four neighbours one texel away: slope, crease, detail
        vec2 ts = vec2(textureSize(tex0, 0)), o = 1.0 / ts;
        float lu = mat_luma(textureGrad(tex0, uv + vec2(o.x, 0.0), gx, gy).rgb), ld = mat_luma(textureGrad(tex0, uv - vec2(o.x, 0.0), gx, gy).rgb);
        float lv = mat_luma(textureGrad(tex0, uv + vec2(0.0, o.y), gx, gy).rgb), lw = mat_luma(textureGrad(tex0, uv - vec2(0.0, o.y), gx, gy).rgb);
        float l0 = mat_luma(m_alb), lb = 0.25 * (lu + ld + lv + lw);
        m_grad = 0.5 * vec2(lu - ld, lv - lw) / max(lb, 0.03);
        float r = clamp(l0 / max(lb, 0.03), 0.5, 1.5);
        m_detail = clamp((r - 1.0) * 4.0, -1.0, 1.0);
        m_cav = clamp(1.0 + LEATHER_CREASE * (r - 1.0), 0.75, 1.05);
        // the twill: diagonal threads, three per texel, faded out under a pixel
        float s = dot(uv * ts, vec2(1.0, 1.0)) * 3.0;
        float fw = length(vec2(dot(gx * ts, vec2(1.0)), dot(gy * ts, vec2(1.0)))) * 3.0;
        float fade = 1.0 - smoothstep(0.4, 1.0, fw);
        m_twill = 0.5 + 0.5 * sin(6.2831853 * s) * fade;
    }
}
// In char_light, once per fragment, at its shading normal n.
void mat_frame(vec3 n) {
    m_n = n;
    m_v = normalize(-v_view);
    m_ne = normalize(n + EYE_BULGE * (m_off.x * m_tu + m_off.y * m_tv));
    m_nb = normalize(n - LEATHER_BUMP * (m_grad.x * m_tu + m_grad.y * m_tv));
}
float mat_pow(float x, float e) { return exp2(e * log2(max(x, 1e-4))); }
// One light (col: its colour; fw: its falloff x walls' share; l: unit direction; ndl = n . l).
void mat_light(vec3 col, float fw, vec3 l, float ndl) {
    vec3 h = normalize(l + m_v);
    if (m_eye > 0.0) {
        float vis = smoothstep(-0.1, 0.2, dot(m_ne, l)) * smoothstep(-0.3, 0.1, ndl);
        m_glint += col * (fw * vis * mat_pow(dot(m_ne, h), m_e));
    }
    if (m_le + m_de + m_co > 0.0) {
        float vis = smoothstep(-0.05, 0.25, ndl);
        float f1 = 1.0 - max(dot(h, m_v), 0.0), f2 = f1 * f1;
        float fres = 0.25 + 0.75 * f2 * f2 * f1;
        float s = 0.0;
        if (m_le > 0.0) s += m_le * LEATHER_SPEC * (0.6 + fres) * mat_pow(dot(m_nb, h), LEATHER_E) * smoothstep(-0.1, 0.3, dot(m_nb, l));
        if (m_de > 0.0) s += m_de * DENIM_SHEEN * mat_pow(dot(m_n, h), DENIM_E) * (1.0 - DENIM_TWILL + 2.0 * DENIM_TWILL * m_twill);
        m_spec += col * (fw * vis * s);
        if (m_co > 0.0) {
            float rim = 1.0 - abs(dot(m_n, m_v));
            m_sheen += col * (fw * m_co * COTTON_VELVET * rim * rim * rim * smoothstep(-0.2, 0.4, ndl));
        }
    }
}
// After the lights and skin_apply (col: the lit colour; base: the combined colour without the
// lights, the draw's own light on the texel): fog hides it all as it hides the surface.
vec3 mat_apply(vec3 col, vec3 base) {
    if (!m_any) return col;
    float fog = 1.0 - clamp(v_fog, 0.0, 1.0);
    if (m_eye > 0.0) {
        float k = min(m_eye, 1.0);
        float lid = 1.0 - EYE_LID * smoothstep(0.55, 1.0, length(m_off)) * (0.6 + 0.4 * clamp(-m_off.y, 0.0, 1.0));
        col *= mix(1.0, lid, k * fog);
        vec3 g = m_glint / (1.0 + 0.5 * m_glint) * lit.cam.w * EYE_GLINT;
        float light = clamp(mat_luma(base) / max(mat_luma(m_alb), 0.05), 0.0, 1.5);
        float c = EYE_CATCH * light * mat_pow(dot(m_ne, normalize(EYE_CATCH_DIR + m_v)), m_e);
        vec3 add = (g + vec3(c)) * min(m_e / EYE_SPOT_E, 1.0);
        // at most EYE_CAP of the light a white texel gets here: a point, never a white eye
        float cap = EYE_CAP * clamp(mat_luma(col) / max(mat_luma(m_alb), 0.05), 0.0, 4.0);
        float peak = max(add.r, max(add.g, add.b));
        if (peak > cap) add *= cap / peak;
        col += add * m_eye * fog;
    }
    if (m_le + m_de + m_co > 0.0) {
        col *= mix(1.0, m_cav, min(m_le, 1.0) * fog);
        if (m_le > 0.0) {   // leather: a broad sheen of the draw's own light from above, broken by the creases
            float light = clamp(mat_luma(base) / max(mat_luma(m_alb), 0.05), 0.0, 1.5);
            float f1 = 1.0 - abs(dot(m_nb, m_v)), f2 = f1 * f1;
            float env = LEATHER_ENV * light * (0.35 + 0.65 * f2) * mat_pow(dot(m_nb, normalize(lit.hair.xyz + m_v)), LEATHER_ENV_E);
            col += env * m_le * mix(vec3(1.0), m_alb / max(max(m_alb.r, max(m_alb.g, m_alb.b)), 0.04), 0.4) * fog;
        }
        vec3 sp = m_spec / (1.0 + 0.5 * m_spec) * lit.cam.w;
        vec3 sh = m_sheen / (1.0 + 0.5 * m_sheen) * lit.cam.w;
        col += (sp * (0.85 + 0.15 * m_detail) + sh * m_alb * 2.0) * fog;
    }
    return col;
}
