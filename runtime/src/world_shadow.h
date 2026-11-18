/* --world-shadows: buildings, props, train tracks and cars cast
 * shadows along the level's own shadow light.
 *
 * The title draws one shadow, of the characters only (its shadow map, sampled by every world
 * draw). Its light is c90 c_vSunDir, the per-level `xshadowdir` of global.lua
 * (the direction the light travels, world z up). Here the frame's opaque world is drawn a second
 * time, depth only, into an orthographic map along that direction (renderer.h renderer_world_shadow),
 * fitted to the nearest part of the view, and every world draw that samples the title's map also
 * tests this one: where the world blocks the light and the title's own test says lit, the vertex
 * colour darkens as the title's shadow does, so both shadows agree and never stack. This module
 * holds the options and the light fit. */
#ifndef WORLD_SHADOW_H
#define WORLD_SHADOW_H
#include <stdint.h>
#include "renderer.h"

#define WORLD_SHADOW_STRENGTH_DEFAULT 1.0f
#define WORLD_SHADOW_STRENGTH_MIN 0.1f
#define WORLD_SHADOW_STRENGTH_MAX 1.0f
#define WORLD_SHADOW_SIZE_DEFAULT 2048
#define WORLD_SHADOW_RANGE_DEFAULT 45.0f
#define WORLD_SHADOW_RANGE_MIN 15.0f
#define WORLD_SHADOW_RANGE_MAX 120.0f
typedef struct {
    int on;           /* --world-shadows / --no-world-shadows */
    float strength;   /* --world-shadows-strength 0.1-1: share of the title's own darkness (1 = the same) */
    int size;         /* --world-shadows-size 512|1024|2048|4096: map texels per side */
    float range;      /* --world-shadows-range 15-120: world units (metres) of the view the map covers */
} WorldShadowConfig;
#define WORLD_SHADOW_DEFAULTS {0, WORLD_SHADOW_STRENGTH_DEFAULT, WORLD_SHADOW_SIZE_DEFAULT, WORLD_SHADOW_RANGE_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a world-shadow option, -1 for a missing or out-of-range value (c unchanged). */
int world_shadow_arg(WorldShadowConfig *c, const char *opt, const char *val);

/* The title's shadow darkness: its receivers' pixel shader multiplies a shadowed texel by c1 = 0.7
 * against c0 = 1.0. Full strength darkens a world shadow the same. */
#define WORLD_SHADOW_DARK 0.7f
/* The map's depth runs from WORLD_SHADOW_UP world units towards the light beyond the covered part
 * of the view (taller casters outside it still cast into it) to just past it. */
#define WORLD_SHADOW_UP 100.0f
/* Fades: the shadow fades out from WORLD_SHADOW_FADE x range to range away from the camera; at a
 * tracked lamp it keeps 1 - WORLD_SHADOW_LAMP_KEEP of itself (the lamp's own light is baked in);
 * where the baked vertex colour's luminance is below WORLD_SHADOW_BAKED_LO there is none, full from
 * WORLD_SHADOW_BAKED_HI (already dark places do not darken twice). */
#define WORLD_SHADOW_FADE 0.75f
#define WORLD_SHADOW_LAMP_KEEP 0.85f
#define WORLD_SHADOW_BAKED_LO 0.05f
#define WORLD_SHADOW_BAKED_HI 0.20f
/* The renderer's indoor gate (renderer_world_shadow_gate): a surface this far nearer the light than
 * a test point counts as cover over it. */
#define WORLD_SHADOW_ROOF 0.5f

/* The renderer's input for a frame (*out) from the frame's world-to-clip matrix wvp (rows x, y, z,
 * w over the homogeneous world position: a world program's c69-c72, clip = wvp p) and the light's
 * travel direction sun (c90; any length above 0.1). The map is square, size texels a side, around
 * the bounding sphere of the view from the camera to c->range units ahead (a fixed size for a fixed
 * field of view), its centre snapped to whole texels so still geometry does not shimmer as the
 * camera moves or turns. lamps (n, world positions and radii; may be NULL) weaken the shadow near
 * them (the renderer's indoor gate, renderer_world_shadow_gate, scales the darkening on the device;
 * dt is left 0 for the caller). Returns 1 and *out, or 0 (out
 * unchanged) when off, without a usable camera (wvp's x, y and w rows not independent) or light. */
int world_shadow_build(const WorldShadowConfig *c, const float wvp[4][4], const float sun[3], const float (*lamps)[4], uint32_t n,
                       RWorldShadow *out);

#endif
