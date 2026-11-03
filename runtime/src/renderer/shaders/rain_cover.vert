#version 450
// Rain cover map (--rain-hd-cover, renderer_rain_cover): the frame's opaque world drawn again, depth
// only, seen straight down: each vertex's clip x, y, w (the frame's camera) taken to the map's NDC
// x, y and depth by three affine rows (push constants). No fragment shader.
layout(location = 0) in vec4 pos;
layout(push_constant) uniform PC {
    vec4 row[3];
} pc;
void main() {
    vec4 q = vec4(pos.xy, pos.w, 1.0);
    gl_Position = vec4(dot(pc.row[0], q), dot(pc.row[1], q), dot(pc.row[2], q), 1.0);
}
