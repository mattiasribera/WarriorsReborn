/* --wet-light-streaks options and the wet pass's light list (wet_streaks.h). */
#include "wet_streaks.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int wet_streaks_arg(WetStreaksConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--wet-light-streaks") || !strcmp(opt, "--no-wet-light-streaks")) {
        c->on = opt[2] == 'w';
        return 1;
    }
    if (!strcmp(opt, "--wet-light-streaks-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= WET_STREAKS_STRENGTH_MIN && f <= WET_STREAKS_STRENGTH_MAX)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    if (!strcmp(opt, "--wet-light-streaks-cap")) {
        char *end;
        if (!val || !*val) return -1;
        const long v = strtol(val, &end, 10);
        if (*end || v < WET_STREAKS_CAP_MIN || v > WET_STREAKS_CAP_MAX) return -1;
        c->cap = (int)v;
        return 2;
    }
    return 0;
}

int wet_streaks_keep(const LampLight *li, float scale, const float view[16], const float proj[16], RRainWet *w, uint32_t k) {
    const float back = -proj[11];   /* view z towards the viewer (as rain_wet_build) */
    if (!(li->intensity > 0) || !(scale > 0) || !(li->rgb[0] > 0 || li->rgb[1] > 0 || li->rgb[2] > 0)) return 0;
    float v[3];
    for (int c = 0; c < 3; c++) v[c] = li->pos[0] * view[c] + li->pos[1] * view[4 + c] + li->pos[2] * view[8 + c] + view[12 + c];
    v[2] *= back;
    const float r = li->radius > 1e-3f ? li->radius : 1e-3f;
    if (!(v[2] < r) || -v[2] > WET_STREAKS_FAR) return 0;   /* well behind the camera, or too far ahead */
    for (int c = 0; c < 3; c++) {
        w->light[k].pos[c] = v[c];
        w->light[k].rgb[c] = (li->rgb[c] > 0 ? li->rgb[c] : 0) * li->intensity * scale;
    }
    w->light[k].radius = r;
    w->light[k].size = 0;
    return 1;
}

uint32_t wet_streaks_build(const WetStreaksConfig *c, const LampLight *l, uint32_t n, uint32_t first_fire, float scale, const float view[16],
                           const float proj[16], int has_fog, uint32_t fog_argb, RRainWet *w) {
    if (!c->on || !(proj[15] == 0 && fabsf(proj[11]) == 1)) return 0;
    const float back = -proj[11];   /* view z towards the viewer (as rain_wet_build) */
    for (int j = 0; j < 3; j++) {   /* as rain_puddle_build */
        w->right[j] = view[4 * j];
        w->up_v[j] = view[4 * j + 1];
        w->back[j] = back * view[4 * j + 2];
        w->eye[j] = -(view[12] * view[4 * j] + view[13] * view[4 * j + 1] + view[14] * view[4 * j + 2]);
    }
    w->streaks = WET_STREAKS_GAIN * c->strength;
    w->streak_rough = WET_STREAKS_ROUGH;
    w->streak_reach = WET_STREAKS_REACH;
    w->fallback = WET_STREAKS_FALLBACK;
    const uint32_t fog = has_fog ? fog_argb : WET_STREAKS_FOG_DEFAULT;
    for (int k = 0; k < 3; k++) w->fog_rgb[k] = (float)(fog >> (16 - 8 * k) & 0xFFu) / 255.0f;
    uint32_t cap = c->cap < 1 ? 1 : (uint32_t)c->cap, kept = 0;
    if (cap > RENDERER_WET_LIGHTS) cap = RENDERER_WET_LIGHTS;
    if (first_fire > n) first_fire = n;
    for (uint32_t i = first_fire; i < n && kept < cap; i++) kept += (uint32_t)wet_streaks_keep(&l[i], scale, view, proj, w, kept);   /* the fires */
    for (uint32_t i = 0; i < first_fire && kept < cap; i++) kept += (uint32_t)wet_streaks_keep(&l[i], scale, view, proj, w, kept);
    w->nlights = kept;
    return kept;
}
