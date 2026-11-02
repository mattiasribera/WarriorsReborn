#version 450
// Rain slice (--rain-hd, renderer_rain): procedural streaks, no texture. The pixel's position in
// screen heights is turned into the tilted frame (y = the fall direction) and scrolled; a grid of
// cells (columns staggered so rows do not line up) holds at most one streak per cell, at a random
// place across it and along it. Each streak has its own distance, slice distance x spread^h, and is
// drawn at that depth (gl_FragDepth, tested against the scene) and sized and dimmed by 1 / spread^h,
// so the scene hides streaks one by one and sizes vary smoothly with distance. A streak is `width`
// wide (coverage of the pixel: thinner than a pixel fades instead of breaking up) and `length` of
// the cell long, brightest at its head (the bottom) and fading towards its tail: one exposure of a
// falling drop, motion blurred. The grid repeats every 4096 cells (RENDERER_RAIN_PERIOD) so the
// scroll can wrap. Two passes per slice: pass 0 (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) adds a little flat
// colour along the streaks over a uniform veil (haze) of the same colour; pass 1 (DST_COLOR / ONE)
// brightens what lies behind a streak, dst x (1 + lit x colour x streak): the scene's light.
layout(push_constant) uniform PC {
    vec4 frame;    // screen heights per pixel, target aspect, cos(tilt), sin(tilt)
    vec4 cell;     // cell width, cell height (screen heights), streak length (fraction of the cell), width
    vec4 scroll;   // scroll x, y (screen heights, tilted frame), alpha, fill
    vec4 color;    // rgb, slice depth
    vec4 haze;     // veil opacity, pass (0 colour and veil, 1 lit), lit, spread
    vec4 depth;    // depth projection a, b (a = 0: slice depth only), slice distance, log2(spread)
} pc;
layout(location = 0) out vec4 o;
// Streaks are never nearer than the slice's triangle (spread >= 1): the early depth test stays valid.
layout(depth_greater) out float gl_FragDepth;

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
    uint hc = uint(col) * 747796405u + 2891336453u;   // the column's stagger (one-word PCG hash)
    hc = ((hc >> ((hc >> 28u) + 4u)) ^ hc) * 277803737u;
    float stagger = float((hc >> 22u) ^ hc) * (1.0 / 4294967296.0);
    float qy = q.y / pc.cell.y + stagger;
    float row = mod(floor(qy), period);
    vec3 h = hash3(uvec3(uint(col), uint(row), 0xA7u));
    float hd = fract(h.x * 61.7 + h.z * 17.3);   // distance within the slice
    vec2 f = vec2(q.x / pc.cell.x - cx, fract(qy));
    // The streak's own distance: 1 to spread times the slice's; farther streaks are smaller and dimmer.
    float s = pc.depth.x > 0.0 ? exp2(pc.depth.w * hd) : 1.0;
    float k = 1.0 / s;
    // Across: the streak [-w/2, w/2] around its centre, overlapped with the pixel (1 pixel wide).
    float centre = 0.15 + 0.7 * h.y;
    float dx = (f.x - centre) * pc.cell.x / px, wp = pc.cell.w * k / px;
    float cov = max(0.0, min(0.5 * wp, dx + 0.5) - max(-0.5 * wp, dx - 0.5));
    // Along: t = 0 at the tail (top), 1 at the head; the tail fades (motion blur), the head ends soft.
    float len = pc.cell.z * k, start = h.z * (1.0 - len);
    float t = (f.y - start) / len;
    float along = t <= 0.0 || t >= 1.0 ? 0.0 : t * t * (3.0 - 2.0 * t) * (1.0 - smoothstep(0.88, 1.0, t));
    float shade = (0.65 + 0.35 * fract(h.y * 7.31 + h.z * 3.17)) * mix(1.0, k, 0.5);
    float streak = h.x < pc.scroll.w ? cov * along * shade : 0.0;
    gl_FragDepth = streak > 0.0 && pc.depth.x > 0.0 ? clamp(pc.depth.x - pc.depth.y / (pc.depth.z * s), 0.0, 1.0) : gl_FragCoord.z;
    if (pc.haze.y > 0.5) {
        if (streak <= 0.0 || pc.haze.z <= 0.0) discard;
        o = vec4(pc.color.rgb * (pc.haze.z * streak), 0.0);
    } else {
        float a = pc.scroll.z * streak + pc.haze.x * (1.0 - pc.scroll.z * streak);
        if (a <= 0.0) discard;
        o = vec4(pc.color.rgb, a);
    }
}
