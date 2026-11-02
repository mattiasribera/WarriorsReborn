#version 450
// Bloom upsample (--bloom): the smaller level read with a 3x3 tent (1 2 1 / 2 4 2 / 1 2 1, / 16) of
// bilinear taps one source texel apart, times scale, added (blend ONE / ONE) onto the next larger
// level (bloom_comp.frag then adds level 0 onto the scene).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec2 texel;      // source texel size (uv)
    vec2 inv_size;   // 1 / destination size
    float threshold, knee, boost, scale;
} pc;
layout(location = 0) out vec4 o;

vec2 uv;
vec3 tap(float x, float y) { return texture(src, uv + vec2(x, y) * pc.texel).rgb; }

void main() {
    uv = gl_FragCoord.xy * pc.inv_size;
    vec3 c = tap(0, 0) * 4.0 + (tap(-1, 0) + tap(1, 0) + tap(0, -1) + tap(0, 1)) * 2.0 +
             (tap(-1, -1) + tap(1, -1) + tap(-1, 1) + tap(1, 1));
    o = vec4(c * (pc.scale / 16.0), 0.0);
}
