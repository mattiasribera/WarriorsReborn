/* HUD bar recognizer and glass rect math (see hud_glass.h). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hud_glass.h"

#define UV_TOL 0.002f     /* atlas UVs are rounded part rects; neighbouring parts differ by >= 0.005 */
#define AXIS_TOL 0.05f    /* pixels: a screen quad's corners share x and y within this */

typedef struct {
    float x0, y0, x1, y1;   /* viewport pixels */
    float s0, t0, s1, t1;   /* stage-0 UV rect */
    uint32_t v[4];          /* its vertices */
} SQuad;

static void to_screen(const RVertex *p, const RViewport *vp, float *x, float *y) {
    float w = p->w != 0 ? p->w : 1;
    *x = vp->x + (p->x / w * 0.5f + 0.5f) * vp->w;
    *y = vp->y + (0.5f - p->y / w * 0.5f) * vp->h;
}

/* The 6 indices of two triangles -> an axis-aligned screen quad over 4 distinct vertices. */
static int quad_of(const RVertex *v, uint32_t nv, const uint32_t *ix, const RViewport *vp, SQuad *q) {
    int n = 0;
    for (int k = 0; k < 6; k++) {
        if (ix[k] >= nv) return 0;
        int seen = 0;
        for (int j = 0; j < n; j++) seen |= q->v[j] == ix[k];
        if (!seen) {
            if (n == 4) return 0;
            q->v[n++] = ix[k];
        }
    }
    if (n != 4) return 0;
    float xs[4], ys[4];
    q->x0 = q->y0 = q->s0 = q->t0 = INFINITY;
    q->x1 = q->y1 = q->s1 = q->t1 = -INFINITY;
    for (int k = 0; k < 4; k++) {
        const RVertex *p = &v[q->v[k]];
        to_screen(p, vp, &xs[k], &ys[k]);
        q->x0 = fminf(q->x0, xs[k]); q->x1 = fmaxf(q->x1, xs[k]);
        q->y0 = fminf(q->y0, ys[k]); q->y1 = fmaxf(q->y1, ys[k]);
        q->s0 = fminf(q->s0, p->tex[0][0]); q->s1 = fmaxf(q->s1, p->tex[0][0]);
        q->t0 = fminf(q->t0, p->tex[0][1]); q->t1 = fmaxf(q->t1, p->tex[0][1]);
    }
    for (int k = 0; k < 4; k++) {   /* every corner on the bounding rect's corners */
        int ex = fabsf(xs[k] - q->x0) < AXIS_TOL || fabsf(xs[k] - q->x1) < AXIS_TOL;
        int ey = fabsf(ys[k] - q->y0) < AXIS_TOL || fabsf(ys[k] - q->y1) < AXIS_TOL;
        if (!ex || !ey) return 0;
    }
    return 1;
}

/* The track part, drawn as a thin horizontal strip (the HUD's 640x480 layout: 3.5 px high, about
 * 110 px wide; limits relative to the viewport so scaled viewports behave the same). */
static int is_track(const SQuad *q, const RViewport *vp) {
    if (fabsf(q->s0 - HUD_GLASS_TRACK_S0) > UV_TOL || fabsf(q->s1 - HUD_GLASS_TRACK_S1) > UV_TOL ||
        fabsf(q->t0 - HUD_GLASS_TRACK_T0) > UV_TOL || fabsf(q->t1 - HUD_GLASS_TRACK_T1) > UV_TOL)
        return 0;
    float w = q->x1 - q->x0, h = q->y1 - q->y0;
    return h > 0 && h <= vp->h / 40 && w >= vp->w / 64 && w >= 4 * h;
}

/* A quad lying on a track's strip: within its rows (1 px slack at 480 lines) and touching its
 * columns (end caps sit just outside). */
static int on_strip(const SQuad *q, const SQuad *t, const RViewport *vp) {
    float slack = vp->h / 480, h = t->y1 - t->y0, pad = h > 2 * slack ? h : 2 * slack;
    return q->y0 >= t->y0 - slack && q->y1 <= t->y1 + slack && q->x1 >= t->x0 - pad && q->x0 <= t->x1 + pad;
}

static int is_target(const SQuad *t, const RViewport *vp) { return (t->x0 + t->x1) / 2 > vp->x + vp->w / 2; }

static int find(const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const RViewport *vp, HudGlassRect *bars, int max,
                uint8_t *member, int targets_only) {
    if (member) memset(member, 0, nv);
    if (!v || !idx || !vp || !ni || ni % 6 || vp->w <= 0 || vp->h <= 0) return 0;
    uint32_t nq = ni / 6;
    /* Two passes over the quads (no allocation): validate every quad first, then per track. */
    for (uint32_t i = 0; i < nq; i++) {
        SQuad q;
        if (!quad_of(v, nv, idx + i * 6, vp, &q)) return 0;
    }
    int found = 0;
    for (uint32_t i = 0; i < nq && found < max; i++) {
        SQuad t;
        quad_of(v, nv, idx + i * 6, vp, &t);
        if (!is_track(&t, vp) || (targets_only && !is_target(&t, vp))) continue;
        HudGlassRect r = {t.x0, t.y0, t.x1, t.y1};
        for (uint32_t j = 0; j < nq; j++) {
            SQuad q;
            quad_of(v, nv, idx + j * 6, vp, &q);
            if (!on_strip(&q, &t, vp)) continue;
            r.x0 = fminf(r.x0, q.x0); r.y0 = fminf(r.y0, q.y0);
            r.x1 = fmaxf(r.x1, q.x1); r.y1 = fmaxf(r.y1, q.y1);
            if (member)
                for (int k = 0; k < 4; k++)
                    if (member[q.v[k]] != HUD_GLASS_TRACK) member[q.v[k]] = j == i ? HUD_GLASS_TRACK : HUD_GLASS_PART;
        }
        bars[found++] = r;
    }
    return found;
}

int hud_glass_find(const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const RViewport *vp,
                   HudGlassRect *bars, int max, uint8_t *member) {
    return find(v, nv, idx, ni, vp, bars, max, member, 0);
}
int hud_glass_find_targets(const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const RViewport *vp,
                           HudGlassRect *bars, int max, uint8_t *member) {
    return find(v, nv, idx, ni, vp, bars, max, member, 1);
}

static uint32_t scale_rgb(uint32_t c, float toward_white, float mul) {
    uint32_t out = c & 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        float ch = (float)(c >> s & 0xFF);
        ch = (ch + (255 - ch) * toward_white) * mul;
        out |= (uint32_t)(ch > 255 ? 255 : ch + 0.5f) << s;
    }
    return out;
}
static uint32_t scale_alpha(uint32_t c, float f) {
    if (!(f > 0)) f = 0;
    return (c & 0x00FFFFFFu) | (uint32_t)((c >> 24) * f + 0.5f) << 24;
}
static void set_screen_y(RVertex *p, const RViewport *vp, float y) { p->y = (1 - 2 * (y - vp->y) / vp->h) * p->w; }

static void set_screen_x(RVertex *p, const RViewport *vp, float x) { p->x = ((x - vp->x) / vp->w * 2 - 1) * p->w; }
static float clamp01(float f) { return f > 1 ? 1 : f > 0 ? f : 0; }   /* NaN -> 0 */

uint32_t hud_hd_vivid(uint32_t c, float gain) {
    if (!(gain > 0)) return c;
    float ch[3], mx = 0, mn = 1;
    for (int i = 0; i < 3; i++) {
        ch[i] = (c >> (16 - 8 * i) & 0xFF) / 255.0f;
        mx = fmaxf(mx, ch[i]);
        mn = fminf(mn, ch[i]);
    }
    /* HSV: value = max, saturation = (max - min) / max; the hue is the position of each channel
     * between min and max, kept by mapping min..max linearly onto the new min..max. */
    const float sat = mx > 0 ? (mx - mn) / mx : 0;
    const float mx2 = fminf(1, mx * (1 + gain)), sat2 = fminf(1, sat * (1 + gain)), mn2 = mx2 * (1 - sat2);
    uint32_t out = c & 0xFF000000u;
    for (int i = 0; i < 3; i++) {
        const float f = mx > mn ? mn2 + (ch[i] - mn) / (mx - mn) * (mx2 - mn2) : mx2;
        out |= (uint32_t)(clamp01(f) * 255 + 0.5f) << (16 - 8 * i);
    }
    return out;
}

HudHdStyle hud_hd_style(const HudGlassConfig *c) {
    HudHdStyle s;
    s.height = c->hd_height;
    s.opacity = c->hd_opacity;
    s.track_opacity = c->hd_track_opacity >= 0 ? c->hd_track_opacity : c->hd_opacity;
    s.fill_opacity = c->hd_fill_opacity >= 0 ? c->hd_fill_opacity : c->hd_opacity;
    s.vivid = c->hd_vivid;
    s.frame = c->hd_frame;
    return s;
}

int hud_hd_restyle(RVertex *v, uint32_t *nv, uint32_t *idx, uint32_t *ni, const RViewport *vp, float scale, float opacity,
                   HudGlassRect *outlines) {
    HudHdStyle s = {scale, opacity, opacity, opacity, 0, 0};
    return hud_hd_restyle_style(v, nv, idx, ni, vp, &s, outlines);
}

int hud_hd_restyle_style(RVertex *v, uint32_t *nv, uint32_t *idx, uint32_t *ni, const RViewport *vp, const HudHdStyle *s,
                         HudGlassRect *outlines) {
    if (!v || !nv || !idx || !ni || !vp || !s || !*ni || *ni % 6 || vp->w <= 0 || vp->h <= 0) return 0;
    const float scale = s->height, opacity = s->opacity;
    const int frame = s->frame;
    const float track_a = clamp01(s->track_opacity), fill_a = clamp01(s->fill_opacity);
    /* The saturation and brightness a fill at alpha fill_a loses over a neutral background is a
     * factor fill_a; vivid 1 gives all of it back (as far as the color has room). */
    const float gain = frame && fill_a > 0 && fill_a < 1 ? clamp01(s->vivid) * (1 / fill_a - 1) : 0;
    const uint32_t nq = *ni / 6;
    for (uint32_t i = 0; i < nq; i++) {
        SQuad q;
        if (!quad_of(v, *nv, idx + i * 6, vp, &q)) return 0;
    }
    SQuad tracks[HUD_GLASS_MAX_BARS];
    int nb = 0;
    for (uint32_t i = 0; i < nq && nb < HUD_GLASS_MAX_BARS; i++) {
        SQuad t;
        quad_of(v, *nv, idx + i * 6, vp, &t);
        if (is_track(&t, vp) && is_target(&t, vp)) tracks[nb++] = t;
    }
    if (!nb) return 0;
    const float px = vp->h / 480;   /* one pixel of the title's 640x480 layout */
    RVertex outline[HUD_GLASS_MAX_BARS][16];
    const int oq = frame ? 4 : 1;   /* outline quads per bar */
    for (int b = 0; b < nb; b++) {
        const SQuad *t = &tracks[b];
        const float y0 = t->y0, y1 = t->y1, h_old = y1 - y0;
        float h = HUD_HD_ASPECT * (t->x1 - t->x0) * (scale > 0 ? scale : 1);
        if (h < h_old) h = h_old;
        /* The fill: the widest opaque quad on the strip that starts at the track's left edge (the
         * rage meter's) or up to 2 pixels left of it (the mission bar's starts at its left cap). */
        int fill = -1;
        float fill_w = 0, fill_x1 = 0;
        HudGlassRect r = {t->x0, t->y0, t->x1, t->y1};
        for (uint32_t j = 0; j < nq; j++) {
            SQuad q;
            quad_of(v, *nv, idx + j * 6, vp, &q);
            if (!on_strip(&q, t, vp)) continue;
            r.x0 = fminf(r.x0, q.x0);
            r.x1 = fmaxf(r.x1, q.x1);
            const int is_t = fabsf(q.x0 - t->x0) < AXIS_TOL && fabsf(q.x1 - t->x1) < AXIS_TOL && fabsf(q.s0 - t->s0) < UV_TOL;
            if (!is_t && q.x0 > t->x0 - 2 * px && q.x0 < t->x0 + 0.5f * px && v[q.v[0]].color >> 24 >= 0x80 && q.x1 - q.x0 > fill_w) {
                fill = (int)j;
                fill_w = q.x1 - q.x0;
                fill_x1 = q.x1;
            }
        }
        /* Grow every strip quad downward, recolor the track and the fill. Each vertex once: a
         * vertex shared by two quads is moved by the first. */
        for (uint32_t j = 0; j < nq; j++) {
            SQuad q;
            quad_of(v, *nv, idx + j * 6, vp, &q);
            if (!on_strip(&q, t, vp)) continue;
            const int is_t = fabsf(q.x0 - t->x0) < AXIS_TOL && fabsf(q.x1 - t->x1) < AXIS_TOL && fabsf(q.s0 - t->s0) < UV_TOL;
            for (int k = 0; k < 4; k++) {
                RVertex *p = &v[q.v[k]];
                float x, y;
                to_screen(p, vp, &x, &y);
                const int bottom = y > (y0 + y1) / 2;
                set_screen_y(p, vp, bottom ? y0 + h : y0);
                if (frame) {
                    if (is_t) {
                        /* The empty part only: the track's left edge moves to the fill's right one. */
                        if (fill >= 0 && x < (t->x0 + t->x1) / 2) set_screen_x(p, vp, fminf(t->x1, fmaxf(t->x0, fill_x1)));
                        p->color = scale_alpha(HUD_HD_GLASS_TRACK, track_a);
                    } else if ((int)j == fill) {
                        const uint32_t c = hud_hd_vivid(p->color, gain);
                        p->color = scale_alpha(bottom ? scale_rgb(c, 0, 0.8f) : c, fill_a);
                    } else {
                        p->color = scale_alpha(p->color, track_a);   /* end caps */
                    }
                    continue;
                }
                if (is_t) p->color = HUD_HD_TRACK;
                else if ((int)j == fill) p->color = bottom ? scale_rgb(p->color, 0, 0.85f) : scale_rgb(p->color, 0.35f, 1);
                if (opacity < 1) p->color = scale_alpha(p->color, opacity);
            }
        }
        /* Outline quad: the strip grown by one pixel, the track part's UVs, same depth. */
        const float ox[4] = {r.x0 - px, r.x1 + px, r.x1 + px, r.x0 - px}, oy[4] = {y0 - px, y0 - px, y0 + h + px, y0 + h + px};
        if (outlines) outlines[b] = (HudGlassRect){ox[0], oy[0], ox[1], oy[2]};
        const float os[4] = {t->s0, t->s1, t->s1, t->s0}, ot[4] = {t->t0, t->t0, t->t1, t->t1};
        /* Quad rects (x0, y0, x1, y1): the whole outline rect, or its frame (top, bottom, left, right). */
        const float X0 = ox[0], X1 = ox[1], Y0 = oy[0], Y1 = oy[2];
        const float rects[4][4] = {{X0, Y0, X1, frame ? Y0 + px : Y1}, {X0, Y1 - px, X1, Y1}, {X0, Y0 + px, X0 + px, Y1 - px},
                                   {X1 - px, Y0 + px, X1, Y1 - px}};
        const uint32_t ocol = frame ? HUD_HD_GLASS_OUTLINE : opacity < 1 ? scale_alpha(HUD_HD_OUTLINE, sqrtf(opacity)) : HUD_HD_OUTLINE;
        for (int e = 0; e < oq; e++) {
            const float qx[4] = {rects[e][0], rects[e][2], rects[e][2], rects[e][0]}, qy[4] = {rects[e][1], rects[e][1], rects[e][3], rects[e][3]};
            for (int k = 0; k < 4; k++) {
                RVertex *o = &outline[b][4 * e + k];
                *o = v[t->v[0]];
                set_screen_x(o, vp, qx[k]);
                set_screen_y(o, vp, qy[k]);
                o->color = ocol;
                o->specular = 0;
                o->tex[0][0] = os[k];
                o->tex[0][1] = ot[k];
            }
        }
    }
    /* Outline quads first: shift the indices, append the vertices. */
    const uint32_t nov = 4 * oq * nb, noi = 6 * oq * nb;
    memmove(idx + noi, idx, *ni * sizeof *idx);
    static const uint32_t pat[6] = {0, 1, 2, 2, 0, 3};
    for (int b = 0; b < nb; b++)
        for (int e = 0; e < oq; e++) {
            const uint32_t q = (uint32_t)(b * oq + e);
            for (int k = 0; k < 4; k++) v[*nv + 4 * q + k] = outline[b][4 * e + k];
            for (int k = 0; k < 6; k++) idx[6 * q + k] = *nv + 4 * q + pat[k];
        }
    *nv += nov;
    *ni += noi;
    return nb;
}

void hud_glass_fade(RVertex *v, uint32_t nv, const uint8_t *member, float opacity) {
    if (!v || !member) return;
    if (!(opacity > 0)) opacity = 0;   /* NaN too */
    if (opacity > 1) opacity = 1;
    for (uint32_t i = 0; i < nv; i++) {
        if (!member[i]) continue;
        uint32_t a = v[i].color >> 24;
        a = (uint32_t)(a * (member[i] == HUD_GLASS_TRACK ? opacity * 0.5f : opacity) + 0.5f);
        v[i].color = (v[i].color & 0x00FFFFFFu) | a << 24;
    }
}

HudGlassBox hud_glass_box(HudGlassRect r, float margin, float sx, float sy, int tw, int th) {
    HudGlassBox b = {0, 0, 0, 0};
    float x0 = floorf((r.x0 - margin) * sx), y0 = floorf((r.y0 - margin) * sy);
    float x1 = ceilf((r.x1 + margin) * sx), y1 = ceilf((r.y1 + margin) * sy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = (float)tw;
    if (y1 > th) y1 = (float)th;
    if (!(x1 > x0) || !(y1 > y0)) return b;
    b.x = (int)x0;
    b.y = (int)y0;
    b.w = (int)(x1 - x0);
    b.h = (int)(y1 - y0);
    return b;
}

static int parse_float(const char *v, float lo, float hi, float *out) {
    char *end;
    if (!v || !*v) return -1;
    double d = strtod(v, &end);
    if (*end || !(d >= lo && d <= hi)) return -1;   /* NaN fails the range check */
    *out = (float)d;
    return 0;
}

int hud_glass_arg(HudGlassConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--hud-glass")) {
        c->on = 1;
        return 1;
    }
    float f;
    if (!strcmp(opt, "--hud-hd")) {
        c->hd = 1;
        return 1;
    }
    if (!strcmp(opt, "--hud-hd-opacity") || !strcmp(opt, "--hud-hd-frost")) {
        const int op = opt[9] == 'o';
        if (parse_float(val, 0, op ? 1 : 32, &f)) return -1;
        *(op ? &c->hd_opacity : &c->hd_frost) = f;
        c->hd = 1;
        return 2;
    }
    if (!strcmp(opt, "--hud-hd-glass")) {   /* preset; options after it override its values */
        c->hd = c->hd_frame = 1;
        c->hd_track_opacity = HUD_HD_GLASS_TRACK_OPACITY;
        c->hd_fill_opacity = HUD_HD_GLASS_FILL_OPACITY;
        c->hd_vivid = HUD_HD_GLASS_VIVID;
        c->hd_frost = HUD_HD_GLASS_FROST;
        return 1;
    }
    if (!strcmp(opt, "--hud-hd-track-opacity") || !strcmp(opt, "--hud-hd-fill-opacity") || !strcmp(opt, "--hud-hd-vivid")) {
        if (parse_float(val, 0, 1, &f)) return -1;
        *(opt[9] == 't' ? &c->hd_track_opacity : opt[9] == 'f' ? &c->hd_fill_opacity : &c->hd_vivid) = f;
        c->hd = c->hd_frame = 1;
        return 2;
    }
    if (!strcmp(opt, "--hud-hd-height")) {
        if (parse_float(val, 0.5f, 4, &f)) return -1;
        c->hd_height = f;
        c->hd = 1;
        return 2;
    }
    if (!strcmp(opt, "--hud-glass-blur")) {
        if (parse_float(val, 1, 32, &f)) return -1;
        c->blur = f;
    } else if (!strcmp(opt, "--hud-glass-opacity")) {
        if (parse_float(val, 0, 1, &f)) return -1;
        c->opacity = f;
    } else if (!strcmp(opt, "--hud-glass-tint")) {
        if (!val) return -1;
        const char *h = val[0] == '0' && (val[1] == 'x' || val[1] == 'X') ? val + 2 : val;
        if (strlen(h) != 8 || strspn(h, "0123456789abcdefABCDEF") != 8) return -1;
        c->tint = (uint32_t)strtoul(h, NULL, 16);
    } else {
        return 0;
    }
    c->on = 1;
    return 2;
}

RGlass hud_glass_panel(const HudGlassConfig *c, HudGlassRect bar) {
    RGlass g;
    g.x0 = bar.x0 - c->margin;
    g.y0 = bar.y0 - c->margin;
    g.x1 = bar.x1 + c->margin;
    g.y1 = bar.y1 + c->margin;
    g.corner = (g.y1 - g.y0) / 2;
    g.blur = c->blur;
    g.tint = c->tint;
    g.rim = c->rim;
    return g;
}
RGlass hud_hd_frost(HudGlassRect outline, float frost) {
    RGlass g = {outline.x0, outline.y0, outline.x1, outline.y1, frost, 0, 0, 0};
    return g;
}
