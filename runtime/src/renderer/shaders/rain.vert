#version 450
// Rain slice (--rain-hd): full-target triangle at the slice's NDC depth, so the depth test against
// the scene hides it where the scene is nearer (shaders/rain.frag draws the streaks).
layout(push_constant) uniform PC {
    vec4 frame;    // screen heights per pixel, target aspect, cos(tilt), sin(tilt)
    vec4 cell;     // cell width, cell height (screen heights), streak length (fraction of the cell), width
    vec4 scroll;   // scroll x, y (screen heights, tilted frame), alpha, fill
    vec4 color;    // rgb, slice depth
    vec4 haze;     // veil opacity, pass (0 colour and veil, 1 lit), lit, spread
    vec4 depth;    // depth projection a, b (a = 0: slice depth only), slice distance, log2(spread)
} pc;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, pc.color.w, 1.0);
}
