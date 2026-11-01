#version 450
// Glass panel over the frame: the blurred copy inside a rounded rect (box: x0, y0, x1, y1 in target
// pixels, corner radius), mixed with the tint (rgb, strength), a light rim of rim_w pixels along
// the edge (rgb, strength), both slightly stronger at the top; antialiased edge as coverage alpha.
layout(set = 0, binding = 0) uniform sampler2D blurred;
layout(push_constant) uniform Glass { vec4 box; vec4 tint; vec4 rim; float corner; float rim_w; } g;
layout(location = 0) out vec4 o;

void main() {
    vec2 f = gl_FragCoord.xy;
    vec3 c = texelFetch(blurred, ivec2(f), 0).rgb;
    vec2 ctr = 0.5 * (g.box.xy + g.box.zw), hs = 0.5 * (g.box.zw - g.box.xy);
    float r = min(g.corner, min(hs.x, hs.y));
    vec2 q = abs(f - ctr) - (hs - r);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;   // signed distance, < 0 inside
    float cover = clamp(0.5 - d, 0.0, 1.0);
    if (cover <= 0.0) discard;
    float t = clamp((f.y - g.box.y) / max(g.box.w - g.box.y, 1.0), 0.0, 1.0);
    c = mix(c, g.tint.rgb, clamp(g.tint.a * (1.2 - 0.4 * t), 0.0, 1.0));
    float rim = clamp(1.0 + (d + 0.5) / g.rim_w, 0.0, 1.0);
    c = mix(c, g.rim.rgb, g.rim.a * rim * (1.0 - 0.5 * t));
    o = vec4(c, cover);
}
