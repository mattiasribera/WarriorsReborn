#version 450
// --reflections sssr's surface pass (vk_renderer.cpp surf_record): the
// frame's material draws again with the main pass's transform (geom.vert: D3D clip space, y negated),
// their view position for the face normal and the texture coordinates of the stage their specular
// map follows.
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_t0;
layout(location = 2) in vec4 in_t1;
layout(location = 3) in vec4 in_t2;
layout(location = 4) in vec4 in_t3;
layout(push_constant) uniform P { vec4 k; } pc;   // 1 / px, 1 / py, the map's stage, 0
layout(location = 0) out vec3 v_view;
layout(location = 1) out vec4 v_t;
void main() {
    gl_Position = vec4(in_pos.x, -in_pos.y, in_pos.z, in_pos.w);
    // --taa (renderer_taa): the frame's jitter packed in firstInstance as in geom.vert; 0 = none.
    if (gl_InstanceIndex != 0) {
        ivec2 j = ivec2(gl_InstanceIndex, gl_InstanceIndex << 16) >> 16;
        gl_Position.xy += vec2(j) * (1.0 / 1048576.0) * in_pos.w;
    }
    v_view = vec3(in_pos.x * pc.k.x, in_pos.y * pc.k.y, -in_pos.w);
    int st = int(pc.k.z);
    v_t = st == 0 ? in_t0 : st == 1 ? in_t1 : st == 2 ? in_t2 : in_t3;
}
