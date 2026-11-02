#version 450
// Rain slice (--rain-hd, renderer_rain): procedural streaks, no texture. The pixel's position in
// screen heights is turned into the tilted frame (y = the fall direction) and scrolled; a grid of
// cells (columns staggered so rows do not line up) holds at most one streak per cell, at a random
// place across it and along it. A streak is `width` wide (coverage of the pixel: thinner than a
// pixel fades instead of breaking up) and `length` of the cell long, brightest at its head (the
// bottom) and fading towards its tail: one exposure of a falling drop. Under the streaks, a uniform
// veil (haze) of the same colour. The grid repeats every 4096 cells (RENDERER_RAIN_PERIOD) so the
// scroll can wrap. Blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
layout(push_constant) uniform PC {
    vec4 frame;    // screen heights per pixel, target aspect, cos(tilt), sin(tilt)
    vec4 cell;     // cell width, cell height (screen heights), streak length (fraction of the cell), width
    vec4 scroll;   // scroll x, y (screen heights, tilted frame), alpha, fill
    vec4 color;    // rgb, slice depth
    vec4 haze;     // veil opacity
} pc;
layout(location = 0) out vec4 o;

// Integer hash of a cell (PCG-style 3D mix): three values in [0, 1).
vec3 hash3(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return vec3(v >> 8u) * (1.0 / 16777216.0);
}

void main() {
    const float period = 4096.0;
    float px = pc.frame.x;
    vec2 p = gl_FragCoord.xy * px - vec2(0.5 * pc.frame.y, 0.5);   // screen heights from the centre, y down
    vec2 r = vec2(pc.frame.z * p.x - pc.frame.w * p.y, pc.frame.w * p.x + pc.frame.z * p.y);
    vec2 q = r - pc.scroll.xy;   // the pattern moves down the fall direction as scroll.y grows
    float cx = floor(q.x / pc.cell.x);
    float col = mod(cx, period);
    float stagger = hash3(uvec3(uint(col), 7u, 0x5Au)).x;
    float qy = q.y / pc.cell.y + stagger;
    float row = mod(floor(qy), period);
    vec3 h = hash3(uvec3(uint(col), uint(row), 0xA7u));
    vec2 f = vec2(q.x / pc.cell.x - cx, fract(qy));
    // Across: the streak [-w/2, w/2] around its centre, overlapped with the pixel (1 pixel wide).
    float centre = 0.15 + 0.7 * h.y;
    float dx = (f.x - centre) * pc.cell.x / px, wp = pc.cell.w / px;
    float cov = max(0.0, min(0.5 * wp, dx + 0.5) - max(-0.5 * wp, dx - 0.5));
    // Along: t = 0 at the tail (top), 1 at the head.
    float len = pc.cell.z, start = h.z * (1.0 - len);
    float t = clamp((f.y - start) / len, 0.0, 1.0);
    float along = smoothstep(0.0, 0.25, t) * (1.0 - smoothstep(0.9, 1.0, t)) * (0.3 + 0.7 * t);
    float shade = 0.65 + 0.35 * fract(h.y * 7.31 + h.z * 3.17);
    float streak = h.x < pc.scroll.w ? pc.scroll.z * cov * along * shade : 0.0;
    float a = streak + pc.haze.x * (1.0 - streak);
    if (a <= 0.0) discard;
    o = vec4(pc.color.rgb, a);
}
