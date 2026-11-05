/* --sky-hd: see sky.h. */
#include "sky.h"
#include <math.h>
#include <string.h>

int sky_arg(SkyConfig *c, const char *opt, const char *val) {
    (void)val;
    if (!strcmp(opt, "--sky-hd")) c->on = 1;
    else if (!strcmp(opt, "--no-sky-hd")) c->on = 0;
    else return 0;
    return 1;
}

/* ---- stage classifier ---- */

int sky_is_dome(const SkyOp *o) {
    return o->kind == SKY_OP_DRAW && o->fvf == SKY_DOME_FVF && !o->zwrite && o->view_origin && o->radius <= SKY_DOME_RADIUS &&
           o->near > 0 && o->near <= SKY_DOME_NEAR && o->tex_fmt == SKY_TEX_DXT1 && o->tex_w >= SKY_DOME_MIN_W && o->tex_h == 2 * o->tex_w;
}
static int is_cap(const SkyOp *o) {
    return o->kind == SKY_OP_DRAW && o->fvf && o->blend && !o->zwrite && o->view_origin && o->radius <= 2 * SKY_DOME_RADIUS;
}
static int is_skyline(const SkyOp *o) {
    return o->kind == SKY_OP_DRAW && (o->fvf == 0x152u || o->fvf == 0x252u) && o->zwrite && o->near >= SKY_SKYLINE_NEAR_MIN &&
           o->near <= SKY_SKYLINE_NEAR_MAX && o->tex_fmt && o->tex_w <= SKY_SKYLINE_ATLAS_MAX && o->tex_h <= SKY_SKYLINE_ATLAS_MAX;
}
static int is_glow(const SkyOp *o) {
    return o->kind == SKY_OP_DRAW && o->fvf && o->blend && !o->zwrite && o->emissive >= SKY_GLOW_EMISSIVE;
}

int sky_classify(const SkyOp *ops, uint32_t n, SkyStage *out) {
    *out = (SkyStage){-1, -1, -1, -1, 0};
    /* The stage: after the last colour clear that precedes the first depth-only clear. */
    uint32_t start = n, end = n;
    for (uint32_t i = 0; i < n; i++) {
        if (ops[i].kind == SKY_OP_CLEAR_COLOR) start = i + 1;
        else if (ops[i].kind == SKY_OP_CLEAR_DEPTH && start < n) {
            end = i;
            break;
        }
    }
    if (start >= n) return 0;
    uint32_t i = start;
    while (i < end && ops[i].kind == SKY_OP_OTHER) i++;
    if (i >= end || !sky_is_dome(&ops[i])) return 0;
    SkyStage s = {(int32_t)i, -1, -1, -1, 0};
    for (i++; i < end && ops[i].kind == SKY_OP_OTHER; i++) {}
    if (i < end && is_cap(&ops[i])) s.cap = (int32_t)i++;
    for (; i < end && s.skyline < 0; i++)
        if (is_skyline(&ops[i])) s.skyline = (int32_t)i;
    if (end < n) {
        for (i = end + 1; i < n && ops[i].kind == SKY_OP_OTHER; i++) {}
        if (i < n && is_glow(&ops[i])) {
            s.glow_first = (int32_t)i;
            for (; i < n && (ops[i].kind == SKY_OP_OTHER || is_glow(&ops[i])); i++)
                if (ops[i].kind == SKY_OP_DRAW) s.glow_count++;
        }
    }
    *out = s;
    return 1;
}

/* ---- palette ---- */

static int lum(uint32_t c) { return (int)((299u * (c >> 16 & 255) + 587u * (c >> 8 & 255) + 114u * (c & 255)) / 1000u); }

/* Luminance at the given share (0-1) of the texels in rows y0..y1-1 (histogram). */
static int percentile(const uint32_t *t, uint32_t w, uint32_t y0, uint32_t y1, float share) {
    uint32_t hist[256] = {0}, total = (y1 - y0) * w;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = 0; x < w; x++) hist[lum(t[y * w + x])]++;
    uint32_t want = (uint32_t)(share * (float)total), sum = 0;
    for (int l = 0; l < 256; l++) {
        sum += hist[l];
        if (sum > want) return l;
    }
    return 255;
}
/* Mean colour of the texels of rows y0..y1-1 with luminance lo..hi (all of them when none). */
static uint32_t mean_between(const uint32_t *t, uint32_t w, uint32_t y0, uint32_t y1, int lo, int hi) {
    uint64_t s[3] = {0}, k = 0, a[3] = {0}, ka = 0;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t c = t[y * w + x];
            const int l = lum(c);
            for (int j = 0; j < 3; j++) a[j] += c >> (16 - 8 * j) & 255;
            ka++;
            if (l < lo || l > hi) continue;
            for (int j = 0; j < 3; j++) s[j] += c >> (16 - 8 * j) & 255;
            k++;
        }
    if (!k) {
        memcpy(s, a, sizeof s);
        k = ka ? ka : 1;
    }
    return 0xFF000000u | (uint32_t)((s[0] + k / 2) / k) << 16 | (uint32_t)((s[1] + k / 2) / k) << 8 | (uint32_t)((s[2] + k / 2) / k);
}
/* Trimmed mean (40th-80th luminance percentile) of rows y0..y1-1. */
static uint32_t trimmed(const uint32_t *t, uint32_t w, uint32_t y0, uint32_t y1) {
    return mean_between(t, w, y0, y1, percentile(t, w, y0, y1, 0.4f), percentile(t, w, y0, y1, 0.8f));
}

float sky_dome_row(float e) {
    static const float pt[][2] = {{0, SKY_HORIZON_ROW}, {32, 0.375f}, {52, 0.06f}, {85, 0}, {90, 0}};
    if (e <= 0) return SKY_HORIZON_ROW;
    for (int i = 1; i < 5; i++)
        if (e <= pt[i][0]) {
            const float f = (e - pt[i - 1][0]) / (pt[i][0] - pt[i - 1][0]);
            return pt[i - 1][1] + f * (pt[i][1] - pt[i - 1][1]);
        }
    return 0;
}

int sky_palette(const uint32_t *t, uint32_t w, uint32_t h, SkyPalette *out) {
    if (!t || w < 8 || h < 8) return -1;
    SkyPalette p;
    const uint32_t band = h / 100 ? h / 100 : 1;   /* rows averaged on each side of a ramp row */
    for (int k = 0; k < SKY_RAMP; k++) {
        const uint32_t y = (uint32_t)(sky_dome_row(90.0f * (float)k / (SKY_RAMP - 1)) * (float)(h - 1) + 0.5f);
        const uint32_t y0 = y > band ? y - band : 0, y1 = y + band + 1 < h ? y + band + 1 : h;
        p.ramp[k] = trimmed(t, w, y0, y1);
    }
    /* Silhouette: the darkest third of the bottom rows. */
    const uint32_t sy = h - (uint32_t)((float)h * SKY_SILHOUETTE_ROWS + 0.5f);
    p.silhouette = mean_between(t, w, sy, h, 0, percentile(t, w, sy, h, 0.33f));
    /* Stars: small points in the upper rows, texels brighter by SKY_STAR_DELTA than each of the
     * texels two away (left, right, up, down; wrapping across, mirrored at the top and bottom), so painted
     * moons and lit cloud edges, wider than a few texels, do not count. */
    const uint32_t rows = (uint32_t)((float)h * SKY_STAR_ROWS);
    uint32_t bright = 0;
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < w; x++) {
            const float l = (float)lum(t[y * w + x]) - SKY_STAR_DELTA;
            const uint32_t up = y >= 2 ? y - 2 : y + 2, dn = y + 2 < h ? y + 2 : y - 2;   /* mirrored at the edges */
            bright += l > (float)lum(t[y * w + (x + 2) % w]) && l > (float)lum(t[y * w + (x + w - 2) % w]) && l > (float)lum(t[up * w + x]) &&
                      l > (float)lum(t[dn * w + x]);
        }
    const float share = rows ? (float)bright / (float)(rows * w) : 0;
    p.stars = share / SKY_STAR_FULL > 1 ? 1 : share / SKY_STAR_FULL;
    if (lum(p.ramp[SKY_RAMP - 1]) < SKY_STAR_DARK_LUM && p.stars < SKY_STAR_DARK_MIN) p.stars = SKY_STAR_DARK_MIN;
    *out = p;
    return 0;
}

/* ---- sky and skyline parameters ---- */

static uint32_t shade_color(uint32_t c, uint32_t shade) {
    uint32_t r = 0xFF000000u;
    for (int j = 0; j < 3; j++) {
        const uint32_t a = c >> (16 - 8 * j) & 255, b = shade >> (16 - 8 * j) & 255;
        r |= ((a * b + 127) / 255) << (16 - 8 * j);
    }
    return r;
}

int sky_build(const SkyPalette *pal, const float view[16], const float proj[16], uint32_t shade, int cap, float rain, const float *sun,
              float seconds, RSky *out) {
    /* A perspective projection without skew: w = z_view x P11. */
    if (!(proj[0] > 0) || !(proj[5] > 0) || proj[11] == 0 || proj[15] != 0) return 0;
    rain = rain < 0 ? 0 : rain > 1 ? 1 : rain;
    RSky s;
    memset(&s, 0, sizeof s);
    /* View axis j in world space: column j of the view rotation (row vectors). A pixel at NDC (x, y)
     * looks along x_v X + y_v Y + z_v Z with z_v = sign(P11), x_v = (x |P11| - z_v P8) / P0. */
    const float zv = proj[11] > 0 ? 1.0f : -1.0f, w = fabsf(proj[11]);
    for (int i = 0; i < 3; i++) {
        const float X = view[4 * i], Y = view[4 * i + 1], Z = view[4 * i + 2];
        s.right[i] = X * w / proj[0];
        s.up[i] = Y * w / proj[5];
        s.fwd[i] = zv * Z - X * zv * proj[8] / proj[0] - Y * zv * proj[9] / proj[5];
    }
    for (int k = 0; k < RENDERER_SKY_RAMP; k++) s.ramp[k] = shade_color(pal->ramp[k], shade);
    s.silhouette = shade_color(pal->silhouette, shade);
    s.stars = (pal->stars > 1 ? 1 : pal->stars) * SKY_STAR_CHANCE * (1 - rain);
    const float base = cap ? SKY_COVER_CAP : SKY_COVER_CLEAR;
    s.cover = base + (SKY_COVER_RAIN - base) * rain;
    if (sun && pal->stars > 0) {
        const float l = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        if (l > 0 && -sun[2] / l >= SKY_MOON_MIN_ELEVATION) {
            for (int i = 0; i < 3; i++) s.moon[i] = -sun[i] / l;
            s.moon_size = SKY_MOON_RADIUS;
            s.moon_light = 1 - rain;
        }
    }
    s.seconds = seconds;
    *out = s;
    return 1;
}

void sky_skyline_build(const SkyPalette *pal, uint32_t shade, float rain, float seconds, RSkyline *out) {
    rain = rain < 0 ? 0 : rain > 1 ? 1 : rain;
    out->haze = shade_color(pal->ramp[0], shade);
    out->haze_max = SKY_HAZE_MAX;
    out->haze_dist = SKY_HAZE_DIST;
    out->windows = 1 - 0.4f * rain;
    out->seconds = seconds;
}
