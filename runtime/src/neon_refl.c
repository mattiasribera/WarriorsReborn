/* --neon-reflections options, the signs' colour and position, and their lights in the wet ground
 * (neon_refl.h). */
#include "neon_refl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wet_streaks.h"

int neon_refl_arg(NeonReflConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--neon-reflections") || !strcmp(opt, "--no-neon-reflections")) {
        c->on = opt[2] == 'n' && opt[3] == 'e';
        return 1;
    }
    if (!strcmp(opt, "--neon-reflections-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= NEON_REFL_STRENGTH_MIN && f <= NEON_REFL_STRENGTH_MAX)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    return 0;
}

int neon_refl_grid(const uint8_t *chain, uint32_t w, uint32_t h, uint32_t levels, uint8_t grid[NEON_REFL_GRID * NEON_REFL_GRID]) {
    if (!chain || !w || !h) return -1;
    const uint8_t *p = chain;
    uint32_t lw = w, lh = h;
    for (uint32_t l = 1; l < levels; l++) {   /* the next level is still at least the grid's size: step down */
        const uint32_t nw = w >> l ? w >> l : 1, nh = h >> l ? h >> l : 1;
        if (nw < NEON_REFL_GRID || nh < NEON_REFL_GRID) break;
        p += (size_t)lw * lh;
        lw = nw;
        lh = nh;
    }
    for (int y = 0; y < NEON_REFL_GRID; y++)
        for (int x = 0; x < NEON_REFL_GRID; x++) {
            const uint32_t tx = (uint32_t)((x + 0.5f) * lw / NEON_REFL_GRID), ty = (uint32_t)((y + 0.5f) * lh / NEON_REFL_GRID);
            grid[y * NEON_REFL_GRID + x] = p[(size_t)(ty < lh ? ty : lh - 1) * lw + (tx < lw ? tx : lw - 1)];
        }
    return 0;
}

int neon_refl_colour(const uint8_t grid[NEON_REFL_GRID * NEON_REFL_GRID], const uint32_t *argb, uint32_t tw, uint32_t th, float rgb[3],
                     float *cover) {
    if (!argb || !tw || !th) return -1;
    double s[3] = {0, 0, 0}, m = 0;
    for (int y = 0; y < NEON_REFL_GRID; y++)
        for (int x = 0; x < NEON_REFL_GRID; x++) {
            const uint8_t k = grid[y * NEON_REFL_GRID + x];
            if (!k) continue;
            uint32_t tx = (uint32_t)((x + 0.5f) * tw / NEON_REFL_GRID), ty = (uint32_t)((y + 0.5f) * th / NEON_REFL_GRID);
            const uint32_t c = argb[(size_t)(ty < th ? ty : th - 1) * tw + (tx < tw ? tx : tw - 1)];
            for (int i = 0; i < 3; i++) s[i] += k * (double)(c >> (16 - 8 * i) & 0xFFu);
            m += k;
        }
    if (m <= 0) return -1;
    for (int i = 0; i < 3; i++) rgb[i] = (float)(s[i] / (m * 255.0));
    *cover = (float)(m / (255.0 * NEON_REFL_GRID * NEON_REFL_GRID));
    return 0;
}

int neon_refl_sprite(const float *xyzw, uint32_t stride, uint32_t n, const float view[16], const float proj[16], const float rgb[3],
                     LampSprite *out) {
    if (!(proj[15] == 0 && fabsf(proj[11]) == 1) || !(proj[0] != 0) || !(proj[5] != 0)) return -1;
    float to_world[3][4];
    if (lamp_view_to_world(view, to_world)) return -1;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; i++) {
        const float *c = xyzw + (size_t)i * stride;
        if (!(c[3] > 0)) continue;   /* behind the camera (or not a number) */
        /* Clip back to the title's view space: w = proj[11] z, x = proj[0] x + proj[8] z (row vectors). */
        const float z = c[3] / proj[11], v[3] = {(c[0] - proj[8] * z) / proj[0], (c[1] - proj[9] * z) / proj[5], z};
        for (int k = 0; k < 3; k++) {
            const float w = to_world[k][0] * v[0] + to_world[k][1] * v[1] + to_world[k][2] * v[2] + to_world[k][3];
            lo[k] = w < lo[k] ? w : lo[k];
            hi[k] = w > hi[k] ? w : hi[k];
        }
        used++;
    }
    if (!used) return -1;
    float size = 0;
    for (int k = 0; k < 3; k++) {
        out->pos[k] = 0.5f * (lo[k] + hi[k]);
        size = hi[k] - lo[k] > size ? hi[k] - lo[k] : size;
        out->rgb[k] = rgb[k];
    }
    out->size = size > NEON_REFL_MIN_SIZE ? size : NEON_REFL_MIN_SIZE;
    out->weight = 1;
    return 0;
}

uint32_t neon_refl_apply(const NeonReflConfig *c, const LampLight *l, uint32_t n, const float view[16], const float proj[16], RRainWet *w) {
    if (!c->on || !(w->streaks > 0) || !(proj[15] == 0 && fabsf(proj[11]) == 1)) return 0;
    w->neon = c->strength;
    uint32_t added = 0;
    for (uint32_t i = 0; i < n && added < NEON_REFL_CAP && w->nlights < RENDERER_WET_LIGHTS; i++) {
        const uint32_t k = w->nlights;
        LampLight li = l[i];
        const float size = li.radius / LAMP_RADIUS_SCALE;
        if (li.radius > NEON_REFL_RADIUS_MAX) li.radius = NEON_REFL_RADIUS_MAX;   /* a sign lights the ground near it only */
        if (!wet_streaks_keep(&li, NEON_REFL_GAIN * c->strength, view, proj, w, k)) continue;
        w->light[k].size = size;
        w->nlights++;
        added++;
    }
    return added;
}
