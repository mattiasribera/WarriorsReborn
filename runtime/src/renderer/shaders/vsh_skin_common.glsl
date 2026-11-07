// --gpu-skinning: the job records of the compute pre-pass (std430; renderer.h RSkinJob and
// vk_renderer.cpp SkinNormJob). Set 0 bindings: 0 jobs, 1 normal jobs, 2 program words, 3
// constants, 4 stream bytes, 5 weld tables, 6 face normals, 7 volume partial sums, 8 the frame's
// vertices (RVertex, 23 words), 9 its indices, 10 its normals (3 floats per vertex).
struct Job {
    uint prog, nins, nv, out_vertex, consts, fed, nel, pad;
    uint sbase[4];
    uint stride[4];
    uint el[16];
    uint fog_mode;
    float fog_start, fog_end, fog_density;
    float tscale[8];
};
// One --char-lighting draw: vertices vfirst.., triangles from index ifirst (draw-relative), the
// weld tables at group / start / corner (words of binding 5), its faces at face.., its volume
// partial sums at part.. (nparts), the camera scale px, py.
struct NormJob {
    uint vfirst, nv, ifirst, ntri, group, start, corner, face, part, nparts;
    float px, py;
    uint pad[4];
};
