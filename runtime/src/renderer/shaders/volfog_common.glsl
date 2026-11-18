// --volfog (renderer_volfog): the uniform block shared by the passes (volfog_polar.comp,
// volfog_inject.comp, volfog_integrate.comp, volfog_comp.frag; vk_renderer.cpp VolfogUbo, std140)
// and the froxel grid's mapping (volfog.h).
#define VF_MAX 48
#define VF_BINS 128
layout(std140, set = 0, binding = 2) uniform Volfog {
    vec4 proj;      // px, py, 1 / px, 1 / py
    vec4 range;     // near, far, log(far / near), density (scattering per view unit)
    vec4 grid;      // froxels w, h, d, lights
    vec4 up;        // world up in view space, time (s)
    vec4 jit;       // sample offsets in a froxel (0-1), history (1 = blend)
    vec4 fog;       // the title's fog: mode (0 none, 1 exp, 2 exp2, 3 linear), start, end, density
    vec4 prev[3];   // rows: previous view position = prev x (view, 1)
    vec4 prevp;     // previous px, py, -, -
    vec4 tw[3];     // rows: world = tw x (view, 1)
    vec4 wall;      // the wall map: x0, y0, 1 / cell, 1 = walls stop the lights
    vec4 wallz;     // z0, zstep, cell, -
    vec4 depth;     // depth_a, depth_b, output width, height
    vec4 mist;      // the ground's height along up from the camera, the mist's share at the ground (0 none), rain level, -
    vec4 lpos[VF_MAX];   // view position, reach
    vec4 lcol[VF_MAX];   // colour x intensity x gain, kind (0 lamp, 1 fire, 2 neon)
} u;

// Slice coordinate s (0..d) <-> view depth (volfog_slice_depth, volfog_depth_slice).
float vf_slice_depth(float s) { return u.range.x * exp(s / u.grid.z * u.range.z); }
float vf_depth_slice(float z) { return clamp(log(max(z, u.range.x) / u.range.x) / u.range.z * u.grid.z, 0.0, u.grid.z); }
// The view position of grid coordinate g (x, y in froxels, z the slice coordinate).
vec3 vf_view_pos(vec3 g) {
    float z = vf_slice_depth(g.z);
    vec2 ndc = vec2(g.x / u.grid.x * 2.0 - 1.0, 1.0 - g.y / u.grid.y * 2.0);
    return vec3(ndc.x * z * u.proj.z, ndc.y * z * u.proj.w, -z);
}
vec3 vf_world(vec3 v) { return vec3(dot(u.tw[0], vec4(v, 1.0)), dot(u.tw[1], vec4(v, 1.0)), dot(u.tw[2], vec4(v, 1.0))); }
