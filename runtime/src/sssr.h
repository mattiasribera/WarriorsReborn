/* --reflections classic|sssr: the wet ground's screen-space reflections.
 *
 * classic: the wet pass's own short linear march (shaders/wet.frag), exactly as before.
 * sssr: stochastic screen-space reflections ported from AMD FidelityFX SSSR (SDK v1.1.4, MIT;
 * runtime/third_party/FIDELITYFX-NOTICE.md): a min-depth hierarchy of the frame's depth, a
 * hierarchical ray march through it, the reflected direction importance-sampled from a GGX
 * distribution by the surface's roughness (its material: standing water near a mirror, asphalt
 * glossy, dirt rough) and a temporal accumulation with camera reprojection, at the wet pass's half
 * size. --reflections-quality sets the march's budget (and on high two rays per texel).
 * Stage 2: glass, shop windows, car paint and chrome (the relief pack's material
 * shine, --dry-highlights) reflect the street too, dry or wet, with a Fresnel term
 * (--reflections-glass F, 0 off); the ground's roughness also follows its own texture's material
 * gloss; --reflections-res full traces at the internal size instead of half of it. */
#ifndef SSSR_H
#define SSSR_H

#include "renderer.h"   /* RRainWet */

#define SSSR_QUALITY_LOW 1
#define SSSR_QUALITY_MEDIUM 2
#define SSSR_QUALITY_HIGH 3
/* The shader's loop bound for the hierarchical march (shaders/wet.frag SSSR_STEPS_LIMIT). */
#define SSSR_MAX_STEPS_LIMIT 96
/* --reflections-glass's range (its strength; 0 off). */
#define SSSR_GLASS_MAX 2.0f
/* A dry frame's reflected rays reach this far (view units, as the rain's wet ground). */
#define SSSR_GLASS_REACH 40.0f
typedef struct {
    int on;        /* --reflections sssr (1) / classic (0) */
    int quality;   /* --reflections-quality low|medium|high (SSSR_QUALITY_*) */
    float glass;   /* --reflections-glass: glossy materials' reflections, strength 0-SSSR_GLASS_MAX (0 off; default 1) */
    int full;      /* --reflections-res full (1) / half (0, default) */
} SssrConfig;
#define SSSR_DEFAULTS {0, SSSR_QUALITY_MEDIUM, 1.0f, 0}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (2), 0 when opt
 * is not a reflections option, -1 for a missing or unknown value (c unchanged). */
int sssr_arg(SssrConfig *c, const char *opt, const char *val);
/* RendererConfig.sssr: 0 classic, else the quality's march budget (sssr_max_steps). */
int sssr_renderer_steps(const SssrConfig *c);
/* The hierarchical march's most intersections for a quality (medium's for an unknown one). */
int sssr_max_steps(int quality);
/* Rays traced per texel for a quality (high 2, else 1). */
int sssr_rays(int quality);
/* RendererConfig.sssr_glass: 1 when sssr is on with glass reflections (the surface pass), else 0. */
int sssr_renderer_glass(const SssrConfig *c);
/* A dry frame's glass-only reflection pass (RRainWet.dry): out = the camera (view, proj as the wet
 * builds take them: a perspective projection), the depth projection ab (NULL: rain.h's defaults),
 * no wet ground, the glass at c's strength. Returns 1, or 0 (out unchanged) when sssr or glass is
 * off or the projection is not a perspective one. */
int sssr_glass_frame(const SssrConfig *c, const float view[16], const float proj[16], const float ab[2], RRainWet *out);
/* A wet or damp frame's pass gets the glass reflections at c's strength (0 when sssr or glass is off). */
void sssr_glass_apply(const SssrConfig *c, RRainWet *w);

#endif
