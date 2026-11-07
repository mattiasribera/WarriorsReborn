/* --world-lighting: the lamp-lights light list (lamp_lights.h) lights
 * the world geometry (walls, floors, buildings) and the title's prop objects per pixel.
 *
 * The world's light is baked: its programs (the title's vsWorldTransform + vsWorldLightN +
 * vsWorldFinal) write oD0 = r7 x c93 + c94 + v2, the baked vertex colour v2 plus material-scaled
 * vertex lights and a flat ambient; the lamp pools are baked into v2. With --world-lighting the
 * draws of those programs (and of the object programs, vsObjectShadow*) get view-space normals
 * from their raw v1 through the draw's own transform (world_light_record) and are drawn with the
 * renderer's lit geometry variant in its world mode (renderer_draw_world_lit): per fragment the
 * lamp falloff and N.L are added to the vertex colour before the pixel shader's combiners, so the
 * final combiner's fog mix still covers the lit pixels. Each draw is lit only by the lights that
 * reach its view-space bounding box (world_light_mask). Lights are not occluded: a lamp behind a
 * wall lights the wall's far side too (as --char-lighting). */
#ifndef WORLD_LIGHT_H
#define WORLD_LIGHT_H
#include <stdint.h>
#include "renderer.h" /* RLampLights */
#include "vsh.h"      /* VshProgram */

#define WORLD_LIGHT_STRENGTH_DEFAULT 1.0f
#define WORLD_LIGHT_STRENGTH_MIN 0.1f
#define WORLD_LIGHT_STRENGTH_MAX 3.0f
typedef struct {
    int on;           /* --world-lighting / --no-world-lighting */
    float strength;   /* --world-lighting-strength 0.1-3 */
} WorldLightConfig;
#define WORLD_LIGHT_DEFAULTS {0, WORLD_LIGHT_STRENGTH_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a world-lighting option, -1 for a missing or out-of-range value (c unchanged). */
int world_light_arg(WorldLightConfig *c, const char *opt, const char *val);

/* Classes of decoded vertex programs (world_light_program). */
enum { WORLD_PROG_NONE = 0, WORLD_PROG_WORLD = 1, WORLD_PROG_OBJECT = 2 };
/* The class of a program: WORLD when oPos.x-w are dp4 of the raw input v0 with c69-c72 (D3D
 * numbering; the world-view-projection rows), oD0.rgb is written by an add or mad that reads the
 * input v2 (the baked colour), no constant is read relative to a0 (no arl: not a bone palette) and
 * the object world rows c62-c64 are not read; OBJECT when all of that holds but c62-c64 are read
 * (the object programs bring their normal into the world for the title's lights); NONE otherwise
 * (characters, shadow casters, fixed transforms, the sky). */
int world_light_program(const VshProgram *p);

/* A draw's recorded lighting inputs: its view normals at first - 1 in the store (0 = not lit) and
 * its view-space bounding box (min x, y, z, max x, y, z; right-handed: the camera looks down -z). */
typedef struct {
    uint32_t first;
    float box[6];
} WorldLitOp;
/* The view normals of a frame's lit draws (3 floats per vertex), grown on demand. */
typedef struct {
    float *n;
    uint32_t nn, cap;   /* normals in use, allocated */
} WorldLitStore;
void world_lit_store_free(WorldLitStore *s);

/* Records a draw of nv vertices: positions of type pos_type (geom.h GEOM_*) at pos, pos_stride bytes
 * apart, normals of type nrm_type at nrm, nrm_stride apart (both at the draw's first vertex), and
 * wvp = its c69-c72 rows. The camera scale px, py is ssao_proj_scale of the rows (clip x = px x
 * view x); the view rows are (c69 / px, c70 / py, -c72). Each vertex's view normal is its own normal
 * through them (not welded: corners stay sharp; not renormalised: unit for the title's rigid
 * transforms, and the fragment shader renormalises the interpolated one), and op->box the view-space
 * box of the 8 corners of the positions' box (it contains every view position). Appends the normals
 * to s and fills op. Returns 0, or -1 (op->first = 0, s unchanged) for an unusable projection, no
 * vertex or no memory. */
int world_light_record(WorldLitStore *s, WorldLitOp *op, const float (*wvp)[4], uint32_t pos_type, const uint8_t *pos, uint32_t pos_stride,
                       uint32_t nrm_type, const uint8_t *nrm, uint32_t nrm_stride, uint32_t nv);

/* The lights of p (at most 32) that reach box: bit i set when light i has a positive colour x
 * intensity and its sphere (pos, radius) meets the box. 0 = no light reaches the draw. */
uint32_t world_light_mask(const float box[6], const RLampLights *p);

#endif
