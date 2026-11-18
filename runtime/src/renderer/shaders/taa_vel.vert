#version 450
// --taa velocity pass (taa_vel_record): a skinned character draw with its current clip position
// (D3D, the frame's skinning output) and the one its previous pose and camera give (the job run
// again with last frame's constants), jittered like the main pass so its pixels match the frame's.
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_prev;
layout(push_constant) uniform Push {
    vec4 jit;   // xy: the jitter in Vulkan NDC units
} pc;
layout(location = 0) out vec4 v_prev;
layout(location = 1) out float v_w;
void main() {
    gl_Position = vec4(in_pos.x, -in_pos.y, in_pos.z, in_pos.w);
    gl_Position.xy += pc.jit.xy * in_pos.w;
    v_prev = in_prev;
    v_w = in_pos.w;
}
