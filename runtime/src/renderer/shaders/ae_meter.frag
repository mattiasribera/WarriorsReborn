#version 450
// --auto-exposure meter (src/auto_exposure.h): one texel per cell of a small grid over the scene
// copy at the filmic anchor; per cell 8x8 bilinear taps, each the log2 linear luma (clamped to
// -13..0) weighted by (1 - light mask) x a centre weight (1 in the middle, 0.25 at the corners), so
// lamps, neon and fire do not meter. Out: the weighted sum and the weight sum, per tap.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(push_constant) uniform PC {
    vec4 a;   // grid width, height, 0, 0
} pc;
layout(location = 0) out vec4 o;

void main() {
    vec2 cell = floor(gl_FragCoord.xy);
    float swl = 0.0, sw = 0.0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            vec2 uv = (cell + (vec2(i, j) + 0.5) / 8.0) / pc.a.xy;
            vec4 s = textureLod(scene, uv, 0.0);
            vec3 lin = pow(clamp(s.rgb, 0.0, 1.0), vec3(2.2));
            float l = clamp(log2(dot(lin, vec3(0.2126, 0.7152, 0.0722)) + 1e-5), -13.0, 0.0);
            float w = (1.0 - clamp(s.a, 0.0, 1.0)) * mix(1.0, 0.25, smoothstep(0.25, 1.1, length((uv - 0.5) * 2.0)));
            swl += w * l;
            sw += w;
        }
    o = vec4(swl / 64.0, sw / 64.0, 0.0, 1.0);
}
