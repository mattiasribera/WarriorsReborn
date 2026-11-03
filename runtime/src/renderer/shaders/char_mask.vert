#version 450
// Character mask (renderer_rain_chars_on): the frame's skinned character draws again, depth only,
// with the main pass's transform (geom.vert: D3D clip space, y negated). No fragment shader.
layout(location = 0) in vec4 in_pos;
void main() {
    gl_Position = vec4(in_pos.x, -in_pos.y, in_pos.z, in_pos.w);
}
