#version 450
// Bloom downsample (--bloom): one level of the chain at half the size of its source, the 13-tap
// filter of Jimenez ("Next generation post processing in Call of Duty: Advanced Warfare", 2014):
// four overlapping 2x2 boxes around the center (weight 0.125 each) and the inner box (0.5), all
// read with bilinear taps, so each output texel averages a 4x4 source area without the blockiness
// of a plain 2x2 box.
// The first level (prefilter = 1) reads the scene and applies the bright pass to every tap: a soft
// knee around the threshold on the brightest channel (the curve of Unity's bloom: quadratic from
// threshold - knee to threshold + knee, linear above, divided by 1 - threshold so the glow of white
// does not depend on the threshold), with the light mask in the scene's alpha
// (additive draws, see renderer.h renderer_bloom) lowering the threshold and knee by up to a fifth
// and raising the result up to 1 + boost, clamped to 1 per channel so a light never feeds more than
// white into the chain; its boxes are weighted by 1 / (1 + luma) (Karis average) so a single bright
// pixel does not flicker as the camera moves.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec2 texel;      // source texel size (uv)
    vec2 inv_size;   // 1 / destination size
    float threshold, knee, boost, prefilter;
} pc;
layout(location = 0) out vec4 o;

vec2 uv;
vec4 tap(float x, float y) { return texture(src, uv + vec2(x, y) * pc.texel); }

vec3 bright(vec4 c) {
    float m = clamp(c.a, 0.0, 1.0);
    float t = pc.threshold * (1.0 - 0.2 * m), k = max(pc.knee * (1.0 - 0.2 * m), 1e-4);
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - t + k, 0.0, 2.0 * k);
    rq = rq * rq / (4.0 * k);
    // Rescaled by 1 / (1 - threshold): white gives 1 at any threshold (the scene has no values above 1).
    return min(c.rgb * (max(rq, br - t) / (max(br, 1e-4) * max(1.0 - t, 0.1))) * (1.0 + pc.boost * m), vec3(1.0));
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    uv = gl_FragCoord.xy * pc.inv_size;
    vec4 a = tap(-2, -2), b = tap(0, -2), c = tap(2, -2);
    vec4 d = tap(-1, -1), e = tap(1, -1);
    vec4 f = tap(-2, 0), g = tap(0, 0), h = tap(2, 0);
    vec4 i = tap(-1, 1), j = tap(1, 1);
    vec4 k = tap(-2, 2), l = tap(0, 2), m = tap(2, 2);
    if (pc.prefilter > 0.5) {
        vec3 A = bright(a), B = bright(b), C = bright(c), D = bright(d), E = bright(e), F = bright(f), G = bright(g);
        vec3 H = bright(h), I = bright(i), J = bright(j), K = bright(k), L = bright(l), M = bright(m);
        vec3 box[5] = vec3[5]((D + E + I + J) * 0.25, (A + B + F + G) * 0.25, (B + C + G + H) * 0.25, (F + G + K + L) * 0.25,
                              (G + H + L + M) * 0.25);
        const float wt[5] = float[5](0.5, 0.125, 0.125, 0.125, 0.125);
        vec3 sum = vec3(0.0);
        float ws = 0.0;
        for (int n = 0; n < 5; n++) {
            float w = wt[n] / (1.0 + luma(box[n]));
            sum += box[n] * w;
            ws += w;
        }
        o = vec4(sum / ws, 1.0);
        return;
    }
    vec3 r = (d + e + i + j).rgb * 0.125 + ((a + b + f + g) + (b + c + g + h) + (f + g + k + l) + (g + h + l + m)).rgb * 0.03125;
    o = vec4(r, 1.0);
}
