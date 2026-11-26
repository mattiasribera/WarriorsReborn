#version 450
// The final grade of the finished frame (src/hdr.h), one full-frame
// pass at the output size. The source is the display-encoded 8-bit frame (gamma 2.2), read exactly.
// SDR (--saturation != 1, the swapchain or the read-back frame): saturation in linear light around the
// BT.709 luminance, back to gamma 2.2 (hdr_sdr_grade is the CPU reference).
// HDR (the HDR10 swapchain blit, -DHDR): the same saturation, then nits: paper white x the linear
// value for the HUD, the scene the same with its highlights expanded towards the peak (hdr_expand:
// the light mask, bloom's alpha at the scene's end, fully, the rest of the scene partly); the scene
// is what did not change between the scene's end (a) and the HUD's end (b), compared over the target
// texel under the pixel and, when the post chain resamples or sharpens, the 3 x 3 around it (the
// frame generation's HUD rule, fg_hud.comp). BT.709 -> BT.2020, SMPTE ST 2084 (PQ) into the
// A2B10G10R10 image the swapchain blit copies (hdr_pixel is the CPU reference).
layout(set = 0, binding = 0) uniform sampler2D src;
#ifdef HDR
layout(set = 0, binding = 1) uniform sampler2D a_tex;
layout(set = 0, binding = 2) uniform sampler2D b_tex;
#endif
layout(push_constant) uniform PC {
    vec4 p0;   // saturation, paper white (nits), peak (nits), flags (1: scene anchor this frame, 2: light mask)
    vec4 p1;   // target texels per output pixel (x, y), reach (0 or 1), 0
    vec4 p2;   // target width, height, 0, 0
} pc;
layout(location = 0) out vec4 o;

const float KNEE = 0.6, SCENE_WEIGHT = 0.4;   // hdr.h HDR_KNEE, HDR_SCENE_WEIGHT

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 saturate_lin(vec3 c, float s) {
    if (s == 1.0) return c;
    float y = luma(c);
    return max(vec3(y) + s * (c - vec3(y)), vec3(0.0));
}

#ifdef HDR
float pq(float nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
#endif

void main() {
    vec4 s = texelFetch(src, ivec2(gl_FragCoord.xy), 0);
    vec3 lin = saturate_lin(pow(clamp(s.rgb, 0.0, 1.0), vec3(2.2)), pc.p0.x);
#ifndef HDR
    o = vec4(pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.2)), s.a);
#else
    float scene = 0.0, light = 0.0;
    uint flags = uint(pc.p0.w);
    if ((flags & 1u) != 0u) {
        ivec2 q = ivec2(gl_FragCoord.xy * pc.p1.xy), last = ivec2(pc.p2.xy) - 1;
        q = clamp(q, ivec2(0), last);
        bool hud = false;
        int r = int(pc.p1.z);
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++) {
                ivec2 t = clamp(q + ivec2(x, y), ivec2(0), last);
                hud = hud || any(notEqual(texelFetch(a_tex, t, 0).rgb, texelFetch(b_tex, t, 0).rgb));
            }
        scene = hud ? 0.0 : 1.0;
        if ((flags & 2u) != 0u) light = clamp(texelFetch(a_tex, q, 0).a, 0.0, 1.0);
    }
    float paper = pc.p0.y, peak = pc.p0.z, ratio = max(peak / paper, 1.0);
    float y = luma(lin), w = scene * (SCENE_WEIGHT + (1.0 - SCENE_WEIGHT) * light);
    float ye = y;
    if (y > KNEE && w > 0.0) {
        float t = min((y - KNEE) / (1.0 - KNEE), 1.0);
        ye = y + w * (ratio - 1.0) * t * t * t;
    }
    vec3 nits = lin * (y > 1e-6 ? ye / y : 1.0) * paper;
    float top = max(nits.r, max(nits.g, nits.b));
    if (top > peak) nits *= peak / top;   // a saturated colour's brightest channel: down to the peak
    // BT.709 -> BT.2020 (ITU-R BT.2087); GLSL matrices are column-major.
    const mat3 to2020 = mat3(0.6274040, 0.0690970, 0.0163916, 0.3292820, 0.9195400, 0.0880132, 0.0433136, 0.0113612, 0.8955950);
    nits = max(to2020 * nits, vec3(0.0));
    o = vec4(pq(nits.r), pq(nits.g), pq(nits.b), 1.0);
#endif
}
