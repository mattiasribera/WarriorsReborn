/* --cinema-bars: the title's in-engine cutscene letterbox bars stay solid black over the port's
 * post effects.
 *
 * The title letterboxes its in-engine cinematics with two black bars, each 12% of the frame high
 * (a 1.75:1 picture on a 4:3 display; with --widescreen the same bars on the anamorphic 16:9
 * frame give a 2.34:1 scope picture, so they are kept as they are). They are drawn near the end
 * of the frame, after the world, the effects and the title's rain layers, by a generic 2D
 * rectangle routine (XBE 0xbc630, one D3DDevice_DrawVerticesUP per bar at 0xbc92c: a D3DPT_QUADLIST
 * of 4 pre-transformed vertices, stride 0x1c, z 0, diffuse 0xff000000, no texture, no blending)
 * that leaves D3DRS_ZENABLE on (the depth writes off). Being depth-tested, the bars count as
 * draws of the 3D scene for the port's "the scene ends at its last depth-tested draw" rule, so
 * the bloom composite, the lamp halos moved after it, the filmic look (vignette, grade,
 * halation) and the rain fade-out ran after the bars and lit them up.
 *
 * With --cinema-bars (default on) the bars are recognised each frame (cinema_bar_draw), left out
 * of the scene for those rules, replayed right after the scene's last draw (cinema_bars_reorder:
 * after every post pass anchored there, before the HUD and subtitles drawn after them) and drawn
 * without the depth test, so nothing of the scene can hide or light them. The title's own draws
 * after the bars (its subtitles, a fade it draws over the whole frame) stay over them as on the
 * Xbox. --no-cinema-bars replays them as recorded. */
#ifndef CINEMA_BARS_H
#define CINEMA_BARS_H
#include <stdint.h>

#include "renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --cinema-bars (1, the default) / --no-cinema-bars (0). Set by main.c, read by render_frame
 * (src/hle_d3d.c). */
extern int cinema_bars_on;

/* Consumes --cinema-bars / --no-cinema-bars into *on; returns 1 when opt is one of them. */
int cinema_bars_arg(int *on, const char *opt);

/* 1 when a back-buffer draw is one of the letterbox bars: 4 vertices (a quad), untextured, no
 * vertex program, no blending, every vertex black (any alpha) at z 0, an axis-aligned rectangle
 * across the whole width (clip x -1..1) touching the top or the bottom edge and at most a
 * quarter of the frame high (not a full-frame fade to black). v are the draw's clip-space
 * vertices (RVertex). */
int cinema_bar_draw(const RVertex *v, uint32_t nv, int textured, int vertex_program, int blend);

/* Replay order ord[0..n) (op indices). bar[i] marks the bars, scene[i] the scene's draws (the
 * depth-tested back-buffer draws and the title's rain layers, bars excluded), by op index. The
 * bars replayed before the last scene draw are moved to right after it, both groups keeping
 * their order; tmp holds n entries. *pos, a replay position of a non-bar op (or n: none), is
 * remapped to where that op is replayed now. Returns the number of bars moved. */
uint32_t cinema_bars_reorder(uint32_t *ord, uint32_t n, const uint8_t *bar, const uint8_t *scene, uint32_t *tmp, uint32_t *pos);

#ifdef __cplusplus
}
#endif
#endif
