#version 450
// Procedural sky (--sky-hd, renderer_sky): one full-target triangle; the fragment shader gets the
// pixel's D3D NDC (y up) to rebuild its view direction.
layout(location = 0) out vec2 v_ndc;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(p, 0.0, 1.0);   // Vulkan clip space: y down
    v_ndc = vec2(p.x, -p.y);
    // --taa (renderer_taa): the frame's jitter packed in firstInstance as in geom.vert (the whole triangle moves: each pixel shows the sky the jittered scene has there); 0 = none.
    if (gl_InstanceIndex != 0) {
        ivec2 j = ivec2(gl_InstanceIndex, gl_InstanceIndex << 16) >> 16;
        gl_Position.xy += vec2(j) * (1.0 / 1048576.0) * gl_Position.w;
    }
}
