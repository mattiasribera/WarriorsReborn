/* --cinema-bars: the title's cutscene letterbox bars over the port's post effects (cinema_bars.h). */
#include "cinema_bars.h"

#include <math.h>
#include <string.h>

int cinema_bars_on = 1;

int cinema_bars_arg(int *on, const char *opt) {
    if (!strcmp(opt, "--cinema-bars")) *on = 1;
    else if (!strcmp(opt, "--no-cinema-bars")) *on = 0;
    else return 0;
    return 1;
}

#define EDGE 0.98f       /* clip coordinate counted as the frame's edge (half-pixel offsets) */
#define SNAP 0.002f      /* tolerance of the rectangle's corners and of z 0 */
#define MAX_HEIGHT 0.5f  /* clip units: a quarter of the frame (each title bar is 0.24) */

int cinema_bar_draw(const RVertex *v, uint32_t nv, int textured, int vertex_program, int blend) {
    if (nv != 4 || textured || vertex_program || blend) return 0;
    float x[4], y[4], x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < 4; i++) {
        if (v[i].color & 0x00FFFFFFu || !(v[i].w > 0)) return 0;
        x[i] = v[i].x / v[i].w, y[i] = v[i].y / v[i].w;
        if (fabsf(v[i].z / v[i].w) > SNAP) return 0;
        x0 = fminf(x0, x[i]), x1 = fmaxf(x1, x[i]), y0 = fminf(y0, y[i]), y1 = fmaxf(y1, y[i]);
    }
    for (int i = 0; i < 4; i++)   /* every corner on the bounding rectangle's sides */
        if ((fabsf(x[i] - x0) > SNAP && fabsf(x[i] - x1) > SNAP) || (fabsf(y[i] - y0) > SNAP && fabsf(y[i] - y1) > SNAP)) return 0;
    if (x0 > -EDGE || x1 < EDGE) return 0;
    if (y0 > -EDGE && y1 < EDGE) return 0;
    return y1 - y0 <= MAX_HEIGHT;
}

uint32_t cinema_bars_reorder(uint32_t *ord, uint32_t n, const uint8_t *bar, const uint8_t *scene, uint32_t *tmp, uint32_t *pos) {
    uint32_t last = n;   /* replay position of the last scene draw */
    for (uint32_t k = 0; k < n; k++)
        if (scene[ord[k]]) last = k;
    uint32_t moved = 0;
    for (uint32_t k = 0; k < last && last < n; k++) moved += bar[ord[k]];
    if (!moved) return 0;
    uint32_t m = 0, at = n;
    for (uint32_t k = 0; k <= last; k++)
        if (!bar[ord[k]]) {
            if (k == *pos) at = m;
            tmp[m++] = ord[k];
        }
    for (uint32_t k = 0; k < last; k++)
        if (bar[ord[k]]) tmp[m++] = ord[k];
    memcpy(ord, tmp, (size_t)m * sizeof *ord);   /* positions after the last scene draw are unchanged */
    if (at < n) *pos = at;
    return moved;
}
