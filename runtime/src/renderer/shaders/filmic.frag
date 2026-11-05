#version 450
// Filmic look of the 3D scene (--filmic, src/filmic.h): one full-target pass at the end of the
// scene, before the HUD, over a copy of the resolved scene (alpha: the bloom light mask, see
// vk_renderer.cpp mask_role). Per pixel, in linear light (display values ^ 2.2):
// - pseudo-HDR: the light-masked highlights (smoothstep of the brightest channel from 0.35 to 1)
//   scaled up to 1 + hdr, so the tonemap has headroom above the 8-bit scene's white;
// - tonemap: blend x ACES(exposure x) x norm + (1 - blend) x min(x, 1) (Narkowicz's ACES fit,
//   normalized so white maps to 1, exposure solved so mid-grey 0.18 maps to itself;
//   src/filmic.c filmic_tonemap computes the same), back to display values;
// - halation: the bloom glow (level 1 of the chain, already wide) tinted red-orange and screened
//   on, as light scattered back through film's base lights the red layer around bright lights;
// - grade: the 3D LUT (procedural 70s grade or a user .cube), lut_mix of it;
// - vignette and grain (both subtle; grain off by default).
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler3D lut;
layout(set = 0, binding = 2) uniform sampler2D glow;
layout(push_constant) uniform PC {
    vec4 a;   // 1 / target size (2), exposure, white
    vec4 b;   // blend, hdr, lut_mix, halation (0: no glow this frame)
    vec4 c;   // grain, vignette, frame seed, LUT size
    vec4 d;   // norm, target aspect, 0, 0
} pc;
layout(location = 0) out vec4 o;

vec3 aces(vec3 x) { return x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14); }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
// Dave Hoskins' hash12 (MIT): a cheap, pattern-free value per pixel.
float hash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

void main() {
    vec4 s = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    vec2 uv = gl_FragCoord.xy * pc.a.xy;
    vec3 lin = pow(clamp(s.rgb, 0.0, 1.0), vec3(2.2));
    float m = clamp(s.a, 0.0, 1.0);
    lin *= 1.0 + pc.b.y * m * smoothstep(0.35, 1.0, max(lin.r, max(lin.g, lin.b)));
    vec3 x = min(lin, vec3(pc.a.w));
    vec3 t = pc.b.x * aces(x * pc.a.z) * pc.d.x + (1.0 - pc.b.x) * min(x, vec3(1.0));
    vec3 c = pow(clamp(t, 0.0, 1.0), vec3(1.0 / 2.2));
    if (pc.b.w > 0.0) {
        vec3 h = clamp(pc.b.w * luma(texture(glow, uv).rgb) * vec3(1.0, 0.38, 0.14), 0.0, 1.0);
        c = 1.0 - (1.0 - c) * (1.0 - h);
    }
    float n = pc.c.w;
    c = mix(c, texture(lut, c * ((n - 1.0) / n) + 0.5 / n).rgb, pc.b.z);
    if (pc.c.y > 0.0) {   // 1 at the corners, darkening from halfway out
        float r = length((uv - 0.5) * vec2(pc.d.y, 1.0)) / length(vec2(pc.d.y, 1.0) * 0.5);
        c *= 1.0 - 0.25 * pc.c.y * smoothstep(0.5, 1.0, r);
    }
    if (pc.c.x > 0.0) {   // grain: new every frame, most in the mid-tones
        float l = luma(c);
        c += pc.c.x * 0.12 * (hash(gl_FragCoord.xy + pc.c.z * vec2(37.0, 17.0)) - 0.5) * (0.3 + 2.8 * l * (1.0 - l));
    }
    o = vec4(clamp(c, 0.0, 1.0), s.a);
}
