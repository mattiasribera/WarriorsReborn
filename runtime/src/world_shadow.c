/* --world-shadows options and the light fit (world_shadow.h). */
#include "world_shadow.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int world_shadow_arg(WorldShadowConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--world-shadows") || !strcmp(opt, "--no-world-shadows")) {
        c->on = opt[2] == 'w';
        return 1;
    }
    const int strength = !strcmp(opt, "--world-shadows-strength"), size = !strcmp(opt, "--world-shadows-size"),
              range = !strcmp(opt, "--world-shadows-range");
    if (!strength && !size && !range) return 0;
    char *end;
    if (!val || !*val) return -1;
    const double f = strtod(val, &end);
    if (*end) return -1;
    if (strength) {
        if (!(f >= WORLD_SHADOW_STRENGTH_MIN && f <= WORLD_SHADOW_STRENGTH_MAX)) return -1;   /* NaN fails too */
        c->strength = (float)f;
    } else if (size) {
        if (f != 512 && f != 1024 && f != 2048 && f != 4096) return -1;
        c->size = (int)f;
    } else {
        if (!(f >= WORLD_SHADOW_RANGE_MIN && f <= WORLD_SHADOW_RANGE_MAX)) return -1;
        c->range = (float)f;
    }
    return 2;
}

static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static int normalize3(double v[3]) {
    const double l = sqrt(dot3(v, v));
    if (!(l > 1e-12) || !isfinite(l)) return -1;
    for (int i = 0; i < 3; i++) v[i] /= l;
    return 0;
}
/* World position of the clip (x, y, w): f = [A^-1 | -A^-1 b] applied to (x, y, w, 1). */
static void at_clip(const double f[3][4], double x, double y, double w, double o[3]) {
    for (int i = 0; i < 3; i++) o[i] = f[i][0] * x + f[i][1] * y + f[i][2] * w + f[i][3];
}

int world_shadow_build(const WorldShadowConfig *c, const float wvp[4][4], const float sun[3], const float (*lamps)[4], uint32_t n,
                       RWorldShadow *out) {
    if (!c->on || c->size < 1) return 0;
    double d[3] = {sun[0], sun[1], sun[2]};
    if (!(sqrt(dot3(d, d)) > 0.1) || normalize3(d)) return 0;
    /* clip (x, y, w) = A p + b: the x, y and w rows; p = A^-1 ((x, y, w) - b). */
    const int rows[3] = {0, 1, 3};
    double a[3][3], b[3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) a[i][j] = wvp[rows[i]][j];
        b[i] = wvp[rows[i]][3];
    }
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    double scale = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) scale = fmax(scale, fabs(a[i][j]));
    if (!(scale > 0) || !isfinite(det) || !(fabs(det) > 1e-9 * scale * scale * scale)) return 0;
    double inv[3][3];
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    double f[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) f[i][j] = inv[i][j];
        f[i][3] = -(inv[i][0] * b[0] + inv[i][1] * b[1] + inv[i][2] * b[2]);
    }
    double eye[3], ahead[3];
    at_clip(f, 0, 0, 0, eye);
    /* w grows along the view axis: |grad w| clip w per world unit ahead. */
    double fwd[3] = {a[2][0], a[2][1], a[2][2]};
    const double wpu = sqrt(dot3(fwd, fwd));
    if (normalize3(fwd)) return 0;
    /* The view from near to range ahead: its corners' distance along the axis and off it (the
     * largest of the four corners at each end), then the smallest sphere centred on the axis. */
    const double zn = 0.1, zf = c->range;
    double ln = 0, lf = 0;
    for (int k = 0; k < 8; k++) {
        const double z = k < 4 ? zn : zf, w = z * wpu;
        double p[3], v[3];
        at_clip(f, (k & 1) ? w : -w, (k & 2) ? w : -w, w, p);
        for (int i = 0; i < 3; i++) v[i] = p[i] - eye[i];
        const double along = dot3(v, fwd), off2 = dot3(v, v) - along * along, off = off2 > 0 ? sqrt(off2) : 0;
        if (k < 4) ln = fmax(ln, off);
        else lf = fmax(lf, off);
    }
    double zc = (zf * zf + lf * lf - zn * zn - ln * ln) / (2 * (zf - zn));
    zc = zc < zn ? zn : zc > zf ? zf : zc;
    double r = sqrt(fmax((zf - zc) * (zf - zc) + lf * lf, (zc - zn) * (zc - zn) + ln * ln));
    r = ceil(r * 4) / 4;   /* steady against rounding: whole quarter units */
    if (!(r > 0) || !isfinite(r)) return 0;
    for (int i = 0; i < 3; i++) ahead[i] = eye[i] + fwd[i] * zc;
    /* The light's frame: x and y across the light, z along it (away from the light). */
    const double up[3] = {0, 0, 1}, side[3] = {1, 0, 0};
    double ax[3], ay[3];
    cross3(fabs(d[2]) < 0.99 ? up : side, d, ax);
    if (normalize3(ax)) return 0;
    cross3(d, ax, ay);
    const double texel = 2 * r / c->size;
    const double cx = floor(dot3(ahead, ax) / texel) * texel, cy = floor(dot3(ahead, ay) / texel) * texel;
    const double zmid = dot3(ahead, d), zmin = zmid - r - WORLD_SHADOW_UP, zmax = zmid + r + 1, depth = zmax - zmin;
    /* light: (world, 1) -> map NDC x, y, depth. */
    double light[3][4];
    for (int j = 0; j < 3; j++) {
        light[0][j] = ax[j] / r;
        light[1][j] = ay[j] / r;
        light[2][j] = d[j] / depth;
    }
    light[0][3] = -cx / r;
    light[1][3] = -cy / r;
    light[2][3] = -zmin / depth;
    RWorldShadow o;
    memset(&o, 0, sizeof o);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            double v = j == 3 ? light[i][3] : 0;
            for (int k = 0; k < 3; k++) v += light[i][k] * f[k][j];
            o.clip_to_map[i][j] = (float)v;
            o.clip_to_world[i][j] = (float)f[i][j];
        }
    for (int i = 0; i < 3; i++) {
        o.eye[i] = (float)eye[i];
        for (int j = 0; j < 4; j++) o.world_to_map[i][j] = (float)light[i][j];
    }
    o.clearance = (float)(WORLD_SHADOW_ROOF / depth);
    const double gl = hypot(fwd[0], fwd[1]);
    o.ahead[0] = gl > 1e-6 ? (float)(fwd[0] / gl) : 1.0f;
    o.ahead[1] = gl > 1e-6 ? (float)(fwd[1] / gl) : 0.0f;
    o.fade_far = c->range;
    o.fade_near = WORLD_SHADOW_FADE * c->range;
    /* Two texels (the bilinear taps reach a texel out, a sloped receiver changes depth by about a
     * texel per texel at the moon's height) and at least 4 cm. */
    o.bias = (float)(fmax(2 * texel, 0.04) / depth);
    const float s = c->strength < 0 ? 0 : c->strength > 1 ? 1 : c->strength;
    o.darken = (1 - WORLD_SHADOW_DARK) * s;
    o.baked_lo = WORLD_SHADOW_BAKED_LO;
    o.baked_hi = WORLD_SHADOW_BAKED_HI;
    o.lamp_keep = WORLD_SHADOW_LAMP_KEEP;
    for (uint32_t i = 0; lamps && i < n && o.lamps < RENDERER_WORLD_SHADOW_LAMPS; i++) {
        if (!(lamps[i][3] > 1e-3f)) continue;
        memcpy(o.lamp[o.lamps++], lamps[i], sizeof o.lamp[0]);
    }
    *out = o;
    return 1;
}
