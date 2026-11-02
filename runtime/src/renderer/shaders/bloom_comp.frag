#version 450
// Bloom composite (--bloom): level 0 (half the target size, already tent-filtered by the upsample
// chain) read with one bilinear tap per target pixel, times scale = strength / levels, screened
// onto the scene (blend ONE_MINUS_DST_COLOR / ONE: scene + glow x (1 - scene)), so a bright halo
// brightens toward white without clipping into a flat white disk. One tap instead of the 3x3 tent
// keeps the only full-size pass cheap.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec2 texel;      // level 0 texel size (uv), unused
    vec2 inv_size;   // 1 / target size
    float threshold, knee, boost, scale;
} pc;
layout(location = 0) out vec4 o;

void main() { o = vec4(texture(src, gl_FragCoord.xy * pc.inv_size).rgb * pc.scale, 0.0); }
