/* Photo mode: the pure part (photo.h). Camera decomposition, free camera,
 * clip matrix, capture plan, accumulation, the look, PNG, file names and the knob snapshot state. */
#include "photo.h"
#include "../third_party/stb/stb_image_write.h"   /* implemented in texpack_stb.c */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PH_PI 3.14159265358979323846

/* ---- 4x4 matrices ---- */
void ph_mat_identity(PhMat *a) {
    memset(a, 0, sizeof *a);
    for (int i = 0; i < 4; i++) a->m[i][i] = 1;
}

void ph_mat_mul(const PhMat *a, const PhMat *b, PhMat *out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += (double)a->m[r][k] * b->m[k][c];
            out->m[r][c] = (float)s;
        }
}

/* Gauss-Jordan with partial pivoting, in double. */
int ph_mat_invert(const PhMat *a, PhMat *out) {
    double w[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) w[r][c] = a->m[r][c], w[r][c + 4] = r == c;
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(w[r][c]) > fabs(w[piv][c])) piv = r;
        if (!(fabs(w[piv][c]) > 1e-12)) {
            ph_mat_identity(out);
            return -1;
        }
        if (piv != c)
            for (int k = 0; k < 8; k++) {
                double t = w[c][k];
                w[c][k] = w[piv][k], w[piv][k] = t;
            }
        double inv = 1.0 / w[c][c];
        for (int k = 0; k < 8; k++) w[c][k] *= inv;
        for (int r = 0; r < 4; r++)
            if (r != c && w[r][c] != 0) {
                double f = w[r][c];
                for (int k = 0; k < 8; k++) w[r][k] -= f * w[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out->m[r][c] = (float)w[r][c + 4];
    return 0;
}

void ph_mat_rotation_only(const PhMat *a, PhMat *out) {
    *out = *a;
    for (int r = 0; r < 3; r++) out->m[r][3] = 0;
}

/* ---- small vector helpers ---- */
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float *a) { return sqrtf(dot3(a, a)); }
static void cross3(const float *a, const float *b, float *o) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- the title's camera ---- */
int ph_camera_from_wvp(const float wvp[4][4], PhCamera *c) {
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            if (!isfinite(wvp[r][k])) return -1;
    /* row3 = k * forward_row */
    float k = len3(wvp[3]);
    if (!(k > 1e-6f)) return -1;
    float fwd[4], right[4], up[4];
    for (int i = 0; i < 4; i++) fwd[i] = wvp[3][i] / k;
    /* row0 = px * right_row + cx * forward_row, row1 likewise */
    float cx = dot3(wvp[0], fwd), cy = dot3(wvp[1], fwd);
    for (int i = 0; i < 4; i++) right[i] = wvp[0][i] - cx * fwd[i], up[i] = wvp[1][i] - cy * fwd[i];
    float px = len3(right), py = len3(up);
    if (!(px > 1e-6f) || !(py > 1e-6f)) return -1;
    for (int i = 0; i < 4; i++) right[i] /= px, up[i] /= py;
    /* row2 = a * forward_row + (0 0 0 b): no other direction */
    float a = dot3(wvp[2], fwd);
    float res[3] = {wvp[2][0] - a * fwd[0], wvp[2][1] - a * fwd[1], wvp[2][2] - a * fwd[2]};
    if (len3(res) > 1e-3f * fmaxf(fabsf(a), 1)) return -1;
    if (fabsf(dot3(right, up)) > 1e-3f || fabsf(dot3(right, fwd)) > 1e-3f || fabsf(dot3(up, fwd)) > 1e-3f) return -1;
    c->px = px, c->py = py, c->cx = cx, c->cy = cy;
    c->a = a, c->b = wvp[2][3] - a * fwd[3], c->k = k;
    for (int i = 0; i < 4; i++) c->view[0][i] = right[i], c->view[1][i] = up[i], c->view[2][i] = fwd[i], c->view[3][i] = i == 3;
    /* eye = -R^T t */
    for (int i = 0; i < 3; i++) c->eye[i] = -(right[i] * right[3] + up[i] * up[3] + fwd[i] * fwd[3]);
    return 0;
}

void ph_camera_projection(const PhCamera *c, PhMat *p) {
    memset(p, 0, sizeof *p);
    p->m[0][0] = c->px, p->m[0][2] = c->cx;
    p->m[1][1] = c->py, p->m[1][2] = c->cy;
    p->m[2][2] = c->a, p->m[2][3] = c->b;
    p->m[3][2] = c->k;
}

/* ---- the free camera ----
 * Yaw is a heading on the ground plane (perpendicular to up) measured in a reference frame built
 * from up alone: gz = (0 0 1) projected onto the ground (fallback (0 1 0)), gx = up x gz. The
 * photo camera's orientation is Q = Rot(up, yaw - yaw0) * Rot(h0, -(pitch - pitch0)) applied to the
 * entry camera's axes (h0 = the horizontal right at the entry heading), then the roll about its own
 * forward: at reset every angle delta is 0 and Q is the identity exactly. */
static void ground_frame(const float up[3], float gz[3], float gx[3]) {
    static const float z[3] = {0, 0, 1}, y[3] = {0, 1, 0}, x[3] = {1, 0, 0};
    const float *cand[3] = {z, y, x};
    for (int i = 0; i < 3; i++) {
        float d = dot3(cand[i], up);
        float g[3] = {cand[i][0] - d * up[0], cand[i][1] - d * up[1], cand[i][2] - d * up[2]};
        float l = len3(g);
        if (l > 1e-3f) {
            for (int k = 0; k < 3; k++) gz[k] = g[k] / l;
            break;
        }
    }
    cross3(up, gz, gx);
}

static void heading(const PhFreeCam *c, float yaw, float h[3], float r[3]) {
    float gz[3], gx[3];
    ground_frame(c->up, gz, gx);
    float cs = cosf(yaw), sn = sinf(yaw);
    for (int i = 0; i < 3; i++) h[i] = cs * gz[i] + sn * gx[i], r[i] = cs * gx[i] - sn * gz[i];
}

/* Rodrigues: the 3x3 rotation about unit axis k by angle t (exactly the identity for t == 0). */
static void axis_rot(const float k[3], float t, float m[3][3]) {
    float cs = cosf(t), sn = sinf(t), oc = 1 - cs;
    m[0][0] = cs + k[0] * k[0] * oc, m[0][1] = k[0] * k[1] * oc - k[2] * sn, m[0][2] = k[0] * k[2] * oc + k[1] * sn;
    m[1][0] = k[1] * k[0] * oc + k[2] * sn, m[1][1] = cs + k[1] * k[1] * oc, m[1][2] = k[1] * k[2] * oc - k[0] * sn;
    m[2][0] = k[2] * k[0] * oc - k[1] * sn, m[2][1] = k[2] * k[1] * oc + k[0] * sn, m[2][2] = cs + k[2] * k[2] * oc;
}

static void mul3(float a[3][3], float b[3][3], float o[3][3]) {
    float t[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
    memcpy(o, t, sizeof t);
}

void ph_freecam_reset(PhFreeCam *c, const float up_view[3], float max_dist) {
    memset(c, 0, sizeof *c);
    float l = up_view ? len3(up_view) : 0;
    if (l > 1e-6f && isfinite(l))
        for (int i = 0; i < 3; i++) c->up[i] = up_view[i] / l;
    else
        c->up[1] = 1;
    static const float f[3] = {0, 0, 1};
    float gz[3], gx[3];
    ground_frame(c->up, gz, gx);
    c->pitch0 = asinf(clampf(dot3(f, c->up), -1, 1));
    c->yaw0 = atan2f(dot3(f, gx), dot3(f, gz));
    c->yaw = c->yaw0, c->pitch = c->pitch0;
    c->zoom = 1;
    c->max_dist = max_dist;
}

void ph_freecam_step(PhFreeCam *c, const PhInput *in, float dt) {
    if (!(dt > 0)) return;
    c->yaw += clampf(in->yaw, -1, 1) * PH_TURN_SPEED * dt;
    c->yaw = remainderf(c->yaw, (float)(2 * PH_PI));
    c->pitch = clampf(c->pitch + clampf(in->pitch, -1, 1) * PH_TURN_SPEED * dt, -PH_PITCH_MAX, PH_PITCH_MAX);
    c->roll = clampf(c->roll + clampf(in->roll, -1, 1) * PH_ROLL_SPEED * dt, -PH_ROLL_MAX, PH_ROLL_MAX);
    c->zoom = clampf(c->zoom * exp2f(clampf(in->zoom, -1, 1) * dt / PH_ZOOM_SPEED), PH_ZOOM_MIN, PH_ZOOM_MAX);
    float h[3], r[3], speed = in->speed > 0 ? in->speed : 1, s = PH_MOVE_SPEED * speed * dt;
    heading(c, c->yaw, h, r);
    for (int i = 0; i < 3; i++)
        c->pos[i] += s * (clampf(in->move[0], -1, 1) * r[i] + clampf(in->move[1], -1, 1) * c->up[i] + clampf(in->move[2], -1, 1) * h[i]);
    float d = len3(c->pos);
    if (c->max_dist > 0 && d > c->max_dist)
        for (int i = 0; i < 3; i++) c->pos[i] *= c->max_dist / d;
}

void ph_freecam_view_delta(const PhFreeCam *c, float lens_x, float lens_y, float focus, PhMat *d) {
    float h0[3], r0[3], q[3][3], pitch_m[3][3], loc[3][3];
    heading(c, c->yaw0, h0, r0);
    axis_rot(c->up, c->yaw - c->yaw0, q);
    axis_rot(r0, -(c->pitch - c->pitch0), pitch_m);
    mul3(q, pitch_m, q);
    /* roll + clockwise: the camera's up tilts toward its right */
    float cs = cosf(c->roll), sn = sinf(c->roll);
    float rl[3][3] = {{cs, sn, 0}, {-sn, cs, 0}, {0, 0, 1}};
    mul3(q, rl, loc);   /* columns: the photo camera's right / up / forward in the entry view */
    ph_mat_identity(d);
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) d->m[r][k] = loc[k][r];   /* R^T */
        d->m[r][3] = -(loc[0][r] * c->pos[0] + loc[1][r] * c->pos[1] + loc[2][r] * c->pos[2]);
    }
    if (focus > 0 && (lens_x != 0 || lens_y != 0)) {
        /* x' = x - lx + lx z / focus (y likewise): row0 += lx / focus * row2 - lx * row3 */
        float l[2] = {lens_x, lens_y};
        for (int r = 0; r < 2; r++)
            for (int k = 0; k < 4; k++) d->m[r][k] += l[r] / focus * d->m[2][k] - l[r] * d->m[3][k];
    }
}

/* ---- clip space ---- */
void ph_clip_identity(PhClipXform *t) { t->sx = t->sy = 1, t->ox = t->oy = 0; }

void ph_clip_matrix(const PhCamera *cam, const PhMat *d, float zoom, const PhClipXform *t, PhMat *m) {
    PhMat p, pinv, pd, pdp;
    ph_camera_projection(cam, &p);
    ph_mat_invert(&p, &pinv);
    ph_mat_mul(&p, d, &pd);
    ph_mat_mul(&pd, &pinv, &pdp);
    for (int k = 0; k < 4; k++) {
        float x = pdp.m[0][k] * zoom, y = pdp.m[1][k] * zoom;
        m->m[0][k] = t->sx * x + t->ox * pdp.m[3][k];
        m->m[1][k] = t->sy * y + t->oy * pdp.m[3][k];
        m->m[2][k] = pdp.m[2][k];
        m->m[3][k] = pdp.m[3][k];
    }
}

/* ---- the capture plan ----
 * Per axis, the smallest tile count t whose crop (grid / t, an integer) fits in the frame: with
 * t == 1 any crop <= out; with t > 1 at least guard * crop of overscan per side. The overscan
 * out - crop must be even so the crop is centred on whole pixels. When no trim works for t, the
 * saved size is trimmed by up to PLAN_TRIM pixels on that axis (a stretch of a few pixels at most,
 * < 0.3 % of a 1280 image) before trying the next t. */
#define PLAN_TRIM 3
static int plan_axis(int final, int ss, int out, float guard, int *final_o, int *tiles, int *crop) {
    for (int t = 1; t <= PH_TILES_MAX; t++)
        for (int trim = 0; trim <= PLAN_TRIM && final - trim >= 16; trim++) {
            int g = (final - trim) * ss;
            if (g % t) continue;
            int cw = g / t;
            if (cw > out || (out - cw) % 2) continue;
            if (t > 1 && (float)(out - cw) * 0.5f < guard * (float)cw) continue;
            *final_o = final - trim, *tiles = t, *crop = cw;
            return 0;
        }
    return -1;
}

int ph_plan(const PhPlanIn *in, PhPlan *p) {
    memset(p, 0, sizeof *p);
    if (in->out_w < 16 || in->out_h < 16) return -1;
    int ss = in->ss == 2 || in->ss == 4 ? in->ss : 1;
    int fw = in->target_w > 0 ? in->target_w : in->out_w;
    int fh = (int)floor((double)fw * in->out_h / in->out_w + 0.5);
    if (fw < 16 || fh < 16) return -1;
    float guard = in->guard < 0 ? PH_GUARD_DEFAULT : in->guard;
    p->out_w = in->out_w, p->out_h = in->out_h, p->ss = ss;
    if (plan_axis(fw, ss, in->out_w, guard, &p->final_w, &p->tiles_x, &p->crop_w) ||
        plan_axis(fh, ss, in->out_h, guard, &p->final_h, &p->tiles_y, &p->crop_h)) {
        memset(p, 0, sizeof *p);
        return -1;
    }
    p->grid_w = p->final_w * ss, p->grid_h = p->final_h * ss;
    p->crop_x = (p->out_w - p->crop_w) / 2, p->crop_y = (p->out_h - p->crop_h) / 2;
    p->kx = (float)p->tiles_x * p->crop_w / p->out_w;
    p->ky = (float)p->tiles_y * p->crop_h / p->out_h;
    int tiles = p->tiles_x * p->tiles_y;
    p->per_tile = in->frames / tiles > 1 ? in->frames / tiles : 1;
    p->frames = tiles * p->per_tile;
    return 0;
}

static float halton(int i, int base) {
    float f = 1, r = 0;
    while (i > 0) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}

/* Shirley-Chiu concentric map of the unit square onto the unit disc. */
static void concentric(float u, float v, float *x, float *y) {
    float a = 2 * u - 1, b = 2 * v - 1, r, phi;
    if (a == 0 && b == 0) {
        *x = *y = 0;
        return;
    }
    if (fabsf(a) > fabsf(b)) r = a, phi = (float)(PH_PI / 4) * (b / a);
    else r = b, phi = (float)(PH_PI / 2) - (float)(PH_PI / 4) * (a / b);
    *x = r * cosf(phi), *y = r * sinf(phi);
}

void ph_plan_shot(const PhPlan *p, int f, PhShot *s) {
    memset(s, 0, sizeof *s);
    int per = p->per_tile > 0 ? p->per_tile : 1, tx = p->tiles_x > 0 ? p->tiles_x : 1;
    int tile = f / per;
    s->sample = f % per;
    s->tile_x = tile % tx, s->tile_y = tile / tx;
    /* Sample 0 is the pixel centre; later samples Halton from index 2 (index 1 is that centre). */
    if (s->sample > 0) {
        s->jx = halton(s->sample + 1, 2) - 0.5f, s->jy = halton(s->sample + 1, 3) - 0.5f;
        concentric(halton(s->sample + 1, 5), halton(s->sample + 1, 7), &s->lens_u, &s->lens_v);
    }
    float c_x = -1 + (float)(2 * s->tile_x + 1) / (float)tx;
    float c_y = 1 - (float)(2 * s->tile_y + 1) / (float)(p->tiles_y > 0 ? p->tiles_y : 1);
    s->t.sx = p->kx, s->t.ox = -p->kx * c_x + 2 * s->jx / (float)p->out_w;
    s->t.sy = p->ky, s->t.oy = -p->ky * c_y - 2 * s->jy / (float)p->out_h;
}

/* ---- accumulation ---- */
static float srgb_lin[256];
static int srgb_lin_ready;

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float linear_to_srgb(float c) {
    if (!(c > 0)) return 0;
    if (c >= 1) return 1;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f;
}
static const float *lin_table(void) {
    if (!srgb_lin_ready) {
        for (int i = 0; i < 256; i++) srgb_lin[i] = srgb_to_linear(i / 255.0f);
        srgb_lin_ready = 1;
    }
    return srgb_lin;
}

int ph_accum_init(PhAccum *a, const PhPlan *p) {
    memset(a, 0, sizeof *a);
    a->plan = *p;
    size_t n = (size_t)p->final_w * (size_t)p->final_h;
    if (!n) return -1;
    a->sum = calloc(n * 3, sizeof *a->sum);
    a->count = calloc(n, sizeof *a->count);
    if (!a->sum || !a->count) {
        ph_accum_free(a);
        return -1;
    }
    return 0;
}

void ph_accum_free(PhAccum *a) {
    free(a->sum);
    free(a->count);
    a->sum = NULL, a->count = NULL;
    a->added = 0;
}

void ph_accum_add(PhAccum *a, const PhShot *s, const uint32_t *px, int pitch) {
    const PhPlan *p = &a->plan;
    if (!a->sum || s->tile_x < 0 || s->tile_x >= p->tiles_x || s->tile_y < 0 || s->tile_y >= p->tiles_y) return;
    const float *lin = lin_table();
    for (int v = 0; v < p->crop_h; v++) {
        const uint32_t *row = px + (size_t)(p->crop_y + v) * (size_t)pitch + p->crop_x;
        int fy = (s->tile_y * p->crop_h + v) / p->ss;
        float *dst = a->sum + (size_t)fy * p->final_w * 3;
        uint32_t *cnt = a->count + (size_t)fy * p->final_w;
        for (int u = 0; u < p->crop_w; u++) {
            int fx = (s->tile_x * p->crop_w + u) / p->ss;
            uint32_t c = row[u];
            dst[3 * fx] += lin[(c >> 16) & 255];
            dst[3 * fx + 1] += lin[(c >> 8) & 255];
            dst[3 * fx + 2] += lin[c & 255];
            cnt[fx]++;
        }
    }
    a->added++;
}

void ph_accum_resolve(const PhAccum *a, float *rgb) {
    size_t n = (size_t)a->plan.final_w * (size_t)a->plan.final_h;
    for (size_t i = 0; i < n; i++) {
        float inv = a->count && a->count[i] ? 1.0f / (float)a->count[i] : 0;
        for (int k = 0; k < 3; k++) rgb[3 * i + k] = a->sum ? a->sum[3 * i + k] * inv : 0;
    }
}

/* ---- the look ----
 * Exposure scales linear light; everything after it works in display space (sRGB-encoded 0..1):
 * filter, saturation (around Rec.709 luma), vignette, grain, bars. */
void ph_look_default(PhLook *l) {
    memset(l, 0, sizeof *l);
    l->saturation = 1;
}

const char *ph_filter_key(int filter) {
    static const char *k[PH_FILTER_COUNT] = {"none", "warm", "cool", "sepia", "bw", "noir", "faded"};
    return filter >= 0 && filter < PH_FILTER_COUNT ? k[filter] : NULL;
}

const char *ph_bars_key(int bars) {
    static const char *k[PH_BARS_COUNT] = {"none", "1.85", "2.39"};
    return bars >= 0 && bars < PH_BARS_COUNT ? k[bars] : NULL;
}

float ph_toast_alpha(double elapsed_ms) {
    if (elapsed_ms < 0 || elapsed_ms >= PH_TOAST_MS) return 0;
    const double fade0 = PH_TOAST_MS - PH_TOAST_FADE_MS;
    return elapsed_ms <= fade0 ? 1.0f : (float)((PH_TOAST_MS - elapsed_ms) / PH_TOAST_FADE_MS);
}
int ph_bars_rows(int bars, int w, int h) {
    double ratio = bars == PH_BARS_185 ? 1.85 : bars == PH_BARS_239 ? 2.39 : 0;
    if (!(ratio > 0) || w <= 0 || h <= 0 || (double)w / h >= ratio) return 0;
    int top = (int)floor((h - w / ratio) * 0.5 + 0.5);
    return top < h / 2 ? top : h / 2;
}

static float luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

/* The filter on one display-space colour. */
static void filter_px(int filter, float c[3]) {
    float y = luma(c);
    switch (filter) {
    case PH_FILTER_WARM: c[0] *= 1.08f, c[1] *= 1.02f, c[2] *= 0.90f; break;
    case PH_FILTER_COOL: c[0] *= 0.92f, c[2] *= 1.08f; break;
    case PH_FILTER_SEPIA: c[0] = y * 1.10f, c[1] = y * 0.95f, c[2] = y * 0.75f; break;
    case PH_FILTER_BW: c[0] = c[1] = c[2] = y; break;
    case PH_FILTER_NOIR: c[0] = c[1] = c[2] = clampf((y - 0.5f) * 1.5f + 0.5f, 0, 1); break;
    case PH_FILTER_FADED:
        for (int k = 0; k < 3; k++) c[k] = 0.08f + 0.88f * (y + (c[k] - y) * 0.8f);
        break;
    default: break;
    }
    for (int k = 0; k < 3; k++) c[k] = clampf(c[k], 0, 1);
}

static float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

/* Deterministic hash noise in [-1, 1) from the seed and the pixel. */
static float grain_noise(uint32_t seed, int x, int y) {
    uint32_t h = seed * 0x9E3779B9u ^ (uint32_t)x * 0x85EBCA6Bu ^ (uint32_t)y * 0xC2B2AE35u;
    h ^= h >> 16, h *= 0x7FEB352Du, h ^= h >> 15, h *= 0x846CA68Bu, h ^= h >> 16;
    return (float)(h >> 8) / 8388608.0f - 1;
}

static uint32_t to8(float v) { return (uint32_t)(clampf(v, 0, 1) * 255 + 0.5f); }

void ph_finish(const PhLook *l, float *rgb, int w, int h, uint32_t *out) {
    float gain = exp2f(clampf(l->exposure, -3, 3)), sat = clampf(l->saturation, 0, 2);
    float vig = clampf(l->vignette, 0, 1), grain = clampf(l->grain, 0, 1) * 0.08f;
    int top = ph_bars_rows(l->bars, w, h);
    float aspect = (float)w / (float)h, norm = 1 / sqrtf(aspect * aspect + 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float *c = rgb + ((size_t)y * w + x) * 3;
            for (int k = 0; k < 3; k++) c[k] = linear_to_srgb(c[k] * gain);
            filter_px(l->filter, c);
            if (sat != 1) {
                float yl = luma(c);
                for (int k = 0; k < 3; k++) c[k] = clampf(yl + (c[k] - yl) * sat, 0, 1);
            }
            if (vig > 0) {
                float nx = ((x + 0.5f) / w * 2 - 1) * aspect, ny = (y + 0.5f) / h * 2 - 1;
                float f = 1 - vig * smoothstep(0.35f, 1.0f, sqrtf(nx * nx + ny * ny) * norm);
                for (int k = 0; k < 3; k++) c[k] *= f;
            }
            if (grain > 0) {
                float g = grain * grain_noise(l->seed, x, y);
                for (int k = 0; k < 3; k++) c[k] = clampf(c[k] + g, 0, 1);
            }
            if (y < top || y >= h - top) c[0] = c[1] = c[2] = 0;
            out[(size_t)y * w + x] = to8(c[0]) << 16 | to8(c[1]) << 8 | to8(c[2]);
        }
}

/* Per channel: exposure and the filter's per-channel part (luma filters can only act per channel
 * here: bw / noir keep the channel, noir with its contrast, sepia its tint, faded its lift). */
void ph_look_lut(const PhLook *l, uint32_t lut[256]) {
    const float *lin = lin_table();
    float gain = exp2f(clampf(l->exposure, -3, 3));
    static const float tint[PH_FILTER_COUNT][3] = {
        {1, 1, 1}, {1.08f, 1.02f, 0.90f}, {0.92f, 1, 1.08f}, {1.10f, 0.95f, 0.75f}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    int f = l->filter >= 0 && l->filter < PH_FILTER_COUNT ? l->filter : PH_FILTER_NONE;
    for (int i = 0; i < 256; i++) {
        float v = linear_to_srgb(lin[i] * gain);
        uint32_t o = 0;
        for (int k = 0; k < 3; k++) {
            float c = v * tint[f][k];
            if (f == PH_FILTER_NOIR) c = (c - 0.5f) * 1.5f + 0.5f;
            if (f == PH_FILTER_FADED) c = 0.08f + 0.88f * c;
            o |= to8(c) << (8 * k);
        }
        lut[i] = o;
    }
}

/* ---- files ---- */
int ph_png_write(const char *path, const uint32_t *px, int w, int h, int pitch, char *err, size_t errn) {
    if (!path || !px || w <= 0 || h <= 0 || pitch < w) {
        if (err && errn) snprintf(err, errn, "bad image (%dx%d, pitch %d)", w, h, pitch);
        return -1;
    }
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (!rgb) {
        if (err && errn) snprintf(err, errn, "out of memory (%dx%d)", w, h);
        return -1;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * pitch + x];
            unsigned char *o = rgb + ((size_t)y * w + x) * 3;
            o[0] = (unsigned char)(c >> 16), o[1] = (unsigned char)(c >> 8), o[2] = (unsigned char)c;
        }
    int ok = stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    if (!ok) {
        if (err && errn) snprintf(err, errn, "cannot write %s", path);
        return -1;
    }
    return 0;
}

int ph_photo_path(char *buf, size_t n, const char *dir, int year, int month, int day, int hour, int min, int sec,
                  int (*exists)(const char *path)) {
    const char *sep = dir && *dir ? "/" : "";
    if (!dir) dir = "";
    for (int i = 1; i < 10000; i++) {
        char suffix[16] = "";
        if (i > 1) snprintf(suffix, sizeof suffix, "_%d", i);
        int len = snprintf(buf, n, "%s%sWarriorsReborn_%04d%02d%02d_%02d%02d%02d%s.png", dir, sep, year, month, day, hour, min,
                           sec, suffix);
        if (len < 0 || (size_t)len >= n) {
            if (n) buf[0] = 0;
            return -1;
        }
        if (!exists || !exists(buf)) return len;
    }
    if (n) buf[0] = 0;
    return -1;
}

/* ---- the state machine ---- */
void ph_init(PhState *s, const PhKnob *knobs, int n) {
    memset(s, 0, sizeof *s);
    s->stage = PH_OFF;
    s->knobs = knobs;
    s->nknobs = n < 0 ? 0 : n > PH_KNOBS_MAX ? PH_KNOBS_MAX : n;
}

int ph_enter(PhState *s) {
    if (s->stage != PH_OFF) return -1;
    int changed = 0;
    for (int i = 0; i < s->nknobs; i++) {
        const PhKnob *k = &s->knobs[i];
        s->changed[i] = 0;
        if (!k->get || !k->set) continue;
        s->saved[i] = k->get();
        if ((k->can && !k->can()) || s->saved[i] == k->photo) continue;
        k->set(k->photo);
        s->changed[i] = 1;
        changed++;
    }
    s->stage = PH_ACTIVE;
    s->frame = 0;
    return changed;
}

int ph_exit(PhState *s) {
    if (s->stage == PH_OFF) return -1;
    int restored = 0;
    for (int i = 0; i < s->nknobs; i++)
        if (s->changed[i]) {
            s->knobs[i].set(s->saved[i]);
            s->changed[i] = 0;
            restored++;
        }
    s->stage = PH_OFF;
    s->frame = 0;
    return restored;
}

int ph_capture_begin(PhState *s, const PhPlanIn *in) {
    if (s->stage != PH_ACTIVE) return -1;
    PhPlan p;
    if (ph_plan(in, &p)) return -1;
    s->plan = p;
    s->frame = 0;
    s->stage = PH_CAPTURING;
    return 0;
}

int ph_capture_next(PhState *s, PhShot *shot) {
    if (s->stage != PH_CAPTURING || s->frame >= s->plan.frames) return 0;
    ph_plan_shot(&s->plan, s->frame++, shot);
    return 1;
}

float ph_capture_progress(const PhState *s) {
    if (s->stage != PH_CAPTURING || s->plan.frames <= 0) return 0;
    return (float)s->frame / (float)s->plan.frames;
}

void ph_capture_end(PhState *s) {
    if (s->stage == PH_CAPTURING) s->stage = PH_ACTIVE;
}
