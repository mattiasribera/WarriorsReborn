/* --gpu-skinning: game-independent pieces of running character vertex programs on the GPU
 *. The renderer's compute shader (shaders/vsh_skin.comp) interprets
 * the same decoded VshProgram as the CPU interpreter (src/vsh.c), from instruction words packed
 * here; a draw is eligible when its program is on the allow-list (FNV-1a 64 of the microcode, the
 * programs whose GPU output was checked against the CPU) and every instruction and vertex element
 * type it uses is one the shader implements.
 *
 * The --char-lighting normals of a GPU-skinned draw are built on the GPU with char_light_normals'
 * rule; the vertices that share their sums (char_light's weld) are taken from the draw's inputs:
 * equal position, bone index and weight bytes give equal skinned positions in every pose, so the
 * groups and each group's triangle corners (gsk_adjacency_build) depend only on the mesh. */
#ifndef GPU_SKIN_H
#define GPU_SKIN_H
#include <stddef.h>
#include <stdint.h>
#include "geom.h"
#include "renderer.h"
#include "vsh.h"

typedef struct {
    int on;      /* --gpu-skinning (default) / --no-gpu-skinning */
    int world;   /* with it, --gpu-vertex-programs (default) / --no-gpu-vertex-programs: the world and
                    object programs of gsk_world_program_allowed too */
} GpuSkinConfig;
#define GPU_SKIN_DEFAULTS {1, 1}
/* Consumes --gpu-skinning / --no-gpu-skinning and --gpu-vertex-programs / --no-gpu-vertex-programs
 * (returns 1), else 0. */
int gpu_skin_arg(GpuSkinConfig *c, const char *opt);

/* FNV-1a 64 over the program's bytes (header and instruction dwords). */
uint64_t gsk_program_hash(const uint32_t *function, uint32_t ndwords);
/* 1 for a character program whose GPU output was checked against the CPU interpreter. */
int gsk_program_allowed(uint64_t hash);
/* 1 for a world or object program (no bone palette) whose GPU output was checked against the CPU
 * interpreter (--gpu-vertex-programs). */
int gsk_world_program_allowed(uint64_t hash);
/* 1 when every instruction's MAC and ILU operation is one the shader implements. */
int gsk_program_supported(const VshProgram *p);
/* 1 for the vertex element types the shader fetches (every GEOM_* type except FLOAT2H). */
int gsk_type_supported(uint32_t type);
/* 1 when the layout's input elements (registers 0-15) are all of supported types in streams 0-3,
 * at most GSK_MAX_ELEMENTS of them, and position (v0) is one of them. */
int gsk_layout_supported(const GeomLayout *l);

/* Instruction words (4 per instruction):
 *   w0: mac 0-3, ilu 4-6, a0x 7, mac_mask 8-11, ilu_mask 12-15, out_r 16-19, o_mask 20-23,
 *       o_reg 24, o_ilu 25, vreg 26-29
 *   w1: o_addr 0-7, creg 8-15
 *   w2: operand a bits 0-14, operand b bits 15-29;  w3: operand c bits 0-14
 *   operand: mux 0-1, reg 2-5, neg 6, swizzle x 7-8, y 9-10, z 11-12, w 13-14 */
void gsk_pack_program(const VshProgram *p, uint32_t (*words)[4]);
void gsk_unpack_ins(const uint32_t w[4], VshIns *x);

/* The constants a draw's program reads: the 192 registers as recorded, with the D3D library's
 * screen-space constants neutralised (vsh_set_screen_constants), as the CPU path runs them. */
void gsk_pack_constants(const float (*vsc)[4], float out[VSH_NCONST][4]);

/* One draw for the compute shader: renderer.h RSkinJob (std430 layout of vsh_skin.comp's Job).
 * Element k: reg | stream << 4 | type << 8 | offset << 16. Fog: fog_mode GSK_FOG_OFF or a
 * GEOM_FOG_* mode (RVertex.fog = 1 - geom_fog_factor of oFog.x). */
#define GSK_MAX_ELEMENTS 16
#define GSK_FOG_OFF 0xFFFFFFFFu
typedef RSkinJob GskJob;
/* Fills j's layout part from l (gsk_layout_supported must hold). */
void gsk_job_layout(GskJob *j, const GeomLayout *l);

/* Weld groups and their triangle corners for the GPU normals. keys: key_bytes per vertex (the
 * bytes of position, bone index and weight inputs); group[v] in 0..ng-1; group g's corners are
 * corner[start[g] .. start[g + 1] - 1], the triangle number of every triangle corner (index
 * idx[3t + k]) of a vertex in g, in index order. Triangles with an index >= nv are left out.
 * Returns 0, or -1 when out of memory (a is then empty). */
typedef struct {
    uint32_t *group, *start, *corner;
    uint32_t nv, ng, ncorner;
} GskAdjacency;
int gsk_adjacency_build(const uint32_t *idx, uint32_t ni, uint32_t nv, const uint8_t *keys, uint32_t key_bytes, GskAdjacency *a);
void gsk_adjacency_free(GskAdjacency *a);
/* Word-at-a-time 64-bit hash of n bytes (the adjacency cache key; not FNV: per frame data). */
uint64_t gsk_hash_bytes(uint64_t h, const void *p, size_t n);
#endif
