#version 450
// Contrast-adaptive sharpening of the finished frame (--sharpen), after the algorithm of AMD
// FidelityFX CAS (the "no scaling" filter), written for this renderer. For each pixel e and its
// 3x3 neighbourhood
//     a b c
//     d e f
//     g h i
// the soft minimum and maximum are min(b, d, e, f, h) + min(a..i) and the same with max (each in
// 0..2), the amount amp = sqrt(clamp(min(mn, 2 - mx) / mx, 0, 1)) (less where the neighbourhood
// is already near black or white, so edges do not clip), the weight w = amp * peak with
// peak = -1 / mix(8, 5, sharpness), and the result (e + w * (b + d + f + h)) / (1 + 4w): a
// negative-lobe cross filter, normalized, so a flat area stays exactly as it was. Texels outside
// the frame repeat the edge.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { float peak; } pc;
layout(location = 0) out vec4 o;

ivec2 lim;
vec3 at(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), lim), 0).rgb; }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    lim = textureSize(src, 0) - 1;
    vec3 a = at(p + ivec2(-1, -1)), b = at(p + ivec2(0, -1)), c = at(p + ivec2(1, -1));
    vec3 d = at(p + ivec2(-1, 0)), f = at(p + ivec2(1, 0));
    vec3 g = at(p + ivec2(-1, 1)), h = at(p + ivec2(0, 1)), i = at(p + ivec2(1, 1));
    vec4 e4 = texelFetch(src, p, 0);
    vec3 e = e4.rgb;
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, i)));
    mx += max(mx, max(max(a, c), max(g, i)));
    vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1.0 / 1024.0)), 0.0, 1.0));
    vec3 w = amp * pc.peak;
    o = vec4(clamp((e + w * (b + d + f + h)) / (1.0 + 4.0 * w), 0.0, 1.0), e4.a);
}
