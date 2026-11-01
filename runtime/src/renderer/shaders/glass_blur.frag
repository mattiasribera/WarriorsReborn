#version 450
// One direction of the glass blur: a gaussian of radius taps (sigma) along dir, source texels
// clamped to lim (x0, y0, x1, y1 inclusive, the copied region). Source and target have the size
// of the frame target, so gl_FragCoord addresses both.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Blur { ivec4 lim; ivec2 dir; int radius; float sigma; } b;
layout(location = 0) out vec4 o;

void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec3 acc = vec3(0.0);
    float sum = 0.0;
    for (int i = -b.radius; i <= b.radius; i++) {
        ivec2 q = clamp(c + b.dir * i, b.lim.xy, b.lim.zw);
        float w = exp(-0.5 * float(i * i) / (b.sigma * b.sigma));
        acc += w * texelFetch(src, q, 0).rgb;
        sum += w;
    }
    o = vec4(acc / sum, 1.0);
}
