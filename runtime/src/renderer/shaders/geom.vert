#version 450
// Positions arrive in D3D clip space (y up), already transformed on the CPU (src/geom.c,
// src/vsh.c); Vulkan's clip space has y down, so y is negated. Depth range [0, w] matches.
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_d0;
layout(location = 2) in vec4 in_d1;
layout(location = 3) in vec4 in_t0;
layout(location = 4) in vec4 in_t1;
layout(location = 5) in vec4 in_t2;
layout(location = 6) in vec4 in_t3;
layout(location = 7) in float in_fog;
layout(location = 0) out vec4 v_d0;
layout(location = 1) out vec4 v_d1;
layout(location = 2) out vec4 v_t0;
layout(location = 3) out vec4 v_t1;
layout(location = 4) out vec4 v_t2;
layout(location = 5) out vec4 v_t3;
layout(location = 6) out float v_fog;

void main() {
    gl_Position = vec4(in_pos.x, -in_pos.y, in_pos.z, in_pos.w);
    v_d0 = in_d0;
    v_d1 = in_d1;
    v_t0 = in_t0;
    v_t1 = in_t1;
    v_t2 = in_t2;
    v_t3 = in_t3;
    v_fog = in_fog;
}
