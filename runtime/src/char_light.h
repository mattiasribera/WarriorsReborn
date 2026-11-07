/* --char-lighting: the lamp-lights light list (lamp_lights.h) lights
 * the characters per pixel.
 *
 * --lamp-lights lights characters in its half-size screen pass, with a normal rebuilt from the
 * depth: noisy and flat across a body's curves. With --char-lighting the character draws (the
 * RENDERER_CHARACTER set) get smooth view-space vertex normals from their own triangles
 * (char_light_normals) and are drawn with the renderer's lit geometry variant
 * (renderer_draw_lit): per fragment, the interpolated normal, the lamp falloff, a wrapped Lambert
 * term and the soft knee of the lamp pass. The lamp pass then leaves character pixels alone. */
#ifndef CHAR_LIGHT_H
#define CHAR_LIGHT_H
#include <stdint.h>
#include "renderer.h" /* RVertex */

typedef struct {
    int on;   /* --char-lighting / --no-char-lighting */
} CharLightConfig;
#define CHAR_LIGHT_DEFAULTS {0}

/* Parses one option. Returns 1 when consumed, 0 when opt is not a char-lighting option. */
int char_light_arg(CharLightConfig *c, const char *opt);

/* Smooth unit normals (3 floats per vertex, right-handed view space: x right, y up, the camera
 * looks down -z) of an indexed triangle list in D3D clip space: each vertex's view position is
 * (x / px, y / py, -w) (px, py: the camera scale, clip x = px * view x; w: the view depth), each
 * triangle adds its area-weighted face normal to its three vertices, vertices at exactly the same
 * clip position share the sum (UV and material seams do not crease), and the whole mesh is turned
 * outward by the sign of its volume (the sum over triangles of a . (b - a) x (c - a)), whatever
 * the title's winding. A vertex that no usable triangle touches gets (0, 0, 1) (facing the camera).
 * Indices out of range and triangles behind the camera (w <= 0) are skipped. Returns 0, or -1 when
 * no triangle was usable (every normal is (0, 0, 1) then) or the scratch memory failed. */
int char_light_normals(const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, float px, float py, float *n);

#endif
