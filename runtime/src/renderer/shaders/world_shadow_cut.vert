#version 450
// World shadow map (--world-shadows, renderer_world_shadow), cut-out casters: a draw whose texels
// are see-through where its stage-0 alpha fails its test (fences, wire, grates). As
// rain_cover.vert (the clip x, y, w to the map by three affine rows), plus the stage-0 texture
// coordinates for world_shadow_cut.frag.
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 t0;
layout(location = 0) out vec4 v_t0;
layout(push_constant) uniform PC {
    vec4 row[3];
} pc;
void main() {
    vec4 q = vec4(pos.xy, pos.w, 1.0);
    gl_Position = vec4(dot(pc.row[0], q), dot(pc.row[1], q), dot(pc.row[2], q), 1.0);
    v_t0 = t0;
}
