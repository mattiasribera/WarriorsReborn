#version 450
#extension GL_GOOGLE_include_directive : require
// Skyline haze and unlit windows (--sky-hd, renderer_skyline): the title's skyline backdrop drawn
// again over itself (depth equal), blend ONE, SRC_ALPHA (dst x a + rgb), rgb only. tex[2].xyz is the
// world position (z up), tex[0].xy the atlas coordinates (u fraction under 0.5: the title's painted
// window). The skyline fades toward the haze colour, and each painted window is either left as the
// title painted it (lit, warm) or dimmed to the facade (unlit): one decision per window, never per
// pixel. No light is added (only the dim, smooth haze): the world drawn after the sky stage blends
// some surfaces (alpha-blended ground with depth writes) over it, so added light would show through
// them onto the street.
#include "sky_common.glsl"
layout(push_constant) uniform PC {
    vec4 haze;     // rgb, strength at full distance
    vec4 params;   // haze distance (view units), windows 0-1 (the lit share's scale), seconds, 0
} pc;
layout(location = 0) in vec4 v_d0;
layout(location = 1) in vec4 v_d1;
layout(location = 2) in vec4 v_t0;
layout(location = 3) in vec4 v_t1;
layout(location = 4) in vec4 v_t2;
layout(location = 5) in vec4 v_t3;
layout(location = 6) in float v_fog;
layout(location = 0) out vec4 out_color;

// A rare window flickers for a few seconds: 0 or 1 now.
float flicker(uint wh, float t) {
    float phase = sky_u01(sky_hash(wh ^ 0x77u));
    uint slot = uint(int(floor(t / 6.0 + phase)));
    if (sky_u01(sky_hash(wh ^ slot * 2654435761u)) >= 0.02) return 1.0;
    return step(0.3, fract(sin(t * 29.0 + float(wh & 1023u)) * 43758.5453));
}

void main() {
    const vec3 P = v_t2.xyz;
    const float t = pc.params.z;
    const float dist = 1.0 / max(abs(gl_FragCoord.w), 1e-6);
    const float haze = pc.haze.w * (1.0 - exp(-dist / pc.params.x));
    float keep = 1.0;
    // Derivatives in uniform control flow: position and atlas coordinates.
    const vec3 dpx = dFdx(P), dpy = dFdy(P);
    const vec2 dux = dFdx(v_t0.xy), duy = dFdy(v_t0.xy);
    if (fract(v_t0.x) < 0.5) {
        // A painted window: one window per quad. Its centre (atlas (0.25, 0.5) of the tile), from
        // the position and atlas derivatives (exact within the quad's plane), identifies it. The
        // centre is snapped to a quarter unit before hashing: window centres sit on round world
        // coordinates, where a plain floor would flip between neighbouring cells from pixel to pixel.
        float det = dux.x * duy.y - dux.y * duy.x;
        vec3 ctr = P;
        if (abs(det) > 1e-12) {
            vec3 dpdu = (dpx * duy.y - dpy * dux.y) / det, dpdv = (dpy * dux.x - dpx * duy.x) / det;
            vec2 off = vec2(fract(v_t0.x) - 0.25, fract(v_t0.y) - 0.5);
            ctr = P - dpdu * off.x - dpdv * off.y;
        }
        uint wh = sky_hash3(ivec3(floor(ctr * 4.0 + 0.5)));
        float lit = step(sky_u01(wh), 0.5 * pc.params.y) * flicker(wh, t);
        keep = mix(0.2, 1.0, lit);
    }
    out_color = vec4(pc.haze.rgb * haze, (1.0 - haze) * keep);
}
