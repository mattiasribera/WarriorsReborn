/* Photo mode test (src/photo.c): matrices, the title camera's
 * decomposition, the free camera, the clip matrix, the capture plan and its shots, accumulation,
 * the look, the PNG writer, the file names and the enter / capture / exit state machine. */
#include "../src/photo.h"
#include "../third_party/stb/stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define PI 3.14159265358979f

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static int mat_near(const PhMat *a, const PhMat *b, float eps) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!near(a->m[r][c], b->m[r][c], eps)) return 0;
    return 1;
}

static int mat_is_identity_exact(const PhMat *a) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (a->m[r][c] != (r == c ? 1.0f : 0.0f)) return 0;
    return 1;
}

static void xform(const PhMat *m, const float p[4], float out[4]) {
    for (int r = 0; r < 4; r++) out[r] = m->m[r][0] * p[0] + m->m[r][1] * p[1] + m->m[r][2] * p[2] + m->m[r][3] * p[3];
}

static void test_matrices(void) {
    PhMat a = {{{2, 0, 1, 3}, {0, 1, 0, -2}, {1, 0, 3, 0}, {0, 0, 0, 1}}}, inv, prod, id;
    ph_mat_identity(&id);
    CHECK(ph_mat_invert(&a, &inv) == 0, "invertible matrix inverts");
    ph_mat_mul(&a, &inv, &prod);
    CHECK(mat_near(&prod, &id, 1e-5f), "a * a^-1 = identity");
    ph_mat_mul(&inv, &a, &prod);
    CHECK(mat_near(&prod, &id, 1e-5f), "a^-1 * a = identity");
    PhMat b = {{{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}}};
    ph_mat_mul(&a, &b, &prod);
    CHECK(prod.m[0][0] == 2 * 1 + 1 * 9 + 3 * 13 && prod.m[2][3] == 1 * 4 + 3 * 12, "mul is row x column");
    PhMat sing = {{{1, 2, 3, 4}, {2, 4, 6, 8}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    CHECK(ph_mat_invert(&sing, &inv) == -1 && mat_is_identity_exact(&inv), "singular: -1 and identity");
    PhMat rot;
    ph_mat_rotation_only(&a, &rot);
    CHECK(rot.m[0][3] == 0 && rot.m[1][3] == 0 && rot.m[2][3] == 0 && rot.m[0][0] == 2 && rot.m[3][3] == 1,
          "rotation_only zeroes the translation column");
}

/* A rigid world -> view matrix: rotation (yaw about y, pitch about x, roll about z) and eye. */
static void rigid_view(float yaw, float pitch, float roll, const float eye[3], PhMat *v) {
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    PhMat ry = {{{cy, 0, -sy, 0}, {0, 1, 0, 0}, {sy, 0, cy, 0}, {0, 0, 0, 1}}};
    PhMat rx = {{{1, 0, 0, 0}, {0, cp, sp, 0}, {0, -sp, cp, 0}, {0, 0, 0, 1}}};
    PhMat rz = {{{cr, sr, 0, 0}, {-sr, cr, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    PhMat t, r;
    ph_mat_mul(&rz, &rx, &t);
    ph_mat_mul(&t, &ry, &r);
    for (int i = 0; i < 3; i++) r.m[i][3] = -(r.m[i][0] * eye[0] + r.m[i][1] * eye[1] + r.m[i][2] * eye[2]);
    *v = r;
}

static void test_camera(void) {
    PhCamera want = {.px = 1.2f, .py = 2.1f, .cx = 0.01f, .cy = -0.02f, .a = 1.001f, .b = -0.1001f, .k = 1.5f};
    PhMat p, v, wvp;
    ph_camera_projection(&want, &p);
    const float eye[3] = {10, 2, -5};
    rigid_view(0.7f, -0.3f, 0.1f, eye, &v);
    ph_mat_mul(&p, &v, &wvp);
    PhCamera c;
    CHECK(ph_camera_from_wvp((const float(*)[4])wvp.m, &c) == 0, "perspective camera with a rigid view decomposes");
    CHECK(near(c.px, want.px, 1e-4f) && near(c.py, want.py, 1e-4f) && near(c.cx, want.cx, 1e-4f) && near(c.cy, want.cy, 1e-4f),
          "px py cx cy recovered (%g %g %g %g)", c.px, c.py, c.cx, c.cy);
    CHECK(near(c.a, want.a, 1e-4f) && near(c.b, want.b, 1e-4f) && near(c.k, want.k, 1e-4f), "a b k recovered (%g %g %g)", c.a, c.b, c.k);
    int view_ok = 1;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            if (!near(c.view[r][k], v.m[r][k], 1e-4f)) view_ok = 0;
    CHECK(view_ok, "view recovered");
    CHECK(near(c.eye[0], 10, 1e-3f) && near(c.eye[1], 2, 1e-3f) && near(c.eye[2], -5, 1e-3f), "eye recovered (%g %g %g)",
          c.eye[0], c.eye[1], c.eye[2]);
    PhMat p2;
    ph_camera_projection(&c, &p2);
    CHECK(mat_near(&p2, &p, 1e-4f), "projection rebuilt");

    PhMat ortho = wvp;
    for (int k = 0; k < 3; k++) ortho.m[3][k] = 0;
    ortho.m[3][3] = 1;
    CHECK(ph_camera_from_wvp((const float(*)[4])ortho.m, &c) == -1, "orthographic (w row without a direction) rejected");
    PhMat shear = {{{1, 0.5f, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}, sv;
    ph_mat_mul(&v, &shear, &sv);
    ph_mat_mul(&p, &sv, &wvp);
    CHECK(ph_camera_from_wvp((const float(*)[4])wvp.m, &c) == -1, "non-rigid (sheared) view rejected");
    PhMat nan = wvp;
    nan.m[1][2] = NAN;
    CHECK(ph_camera_from_wvp((const float(*)[4])nan.m, &c) == -1, "non-finite rejected");
}

static void test_freecam(void) {
    PhFreeCam c;
    PhMat d;
    const float up_tilted[3] = {0.1f, 0.95f, 0.3f};
    ph_freecam_reset(&c, up_tilted, 0);
    ph_freecam_view_delta(&c, 0, 0, 0, &d);
    CHECK(mat_is_identity_exact(&d), "reset with a pitched entry camera: D == identity exactly");
    CHECK(c.zoom == 1 && c.roll == 0 && c.pos[0] == 0 && c.yaw == c.yaw0 && c.pitch == c.pitch0, "reset state");
    CHECK(near(c.pitch0, asinf(0.3f / sqrtf(0.01f + 0.9025f + 0.09f)), 1e-5f), "pitch0 against up");
    const float zero[3] = {0, 0, 0};
    ph_freecam_reset(&c, zero, 0);
    CHECK(c.up[0] == 0 && c.up[1] == 1 && c.up[2] == 0, "zero up means (0, 1, 0)");

    c.pos[0] = 1, c.pos[1] = 2, c.pos[2] = 3;
    ph_freecam_view_delta(&c, 0, 0, 0, &d);
    float p[4] = {4, 5, 6, 1}, q[4];
    xform(&d, p, q);
    CHECK(near(q[0], 3, 1e-6f) && near(q[1], 3, 1e-6f) && near(q[2], 3, 1e-6f) && q[3] == 1, "pure translation: p - pos");

    ph_freecam_reset(&c, NULL, 0);
    PhInput in = {.move = {0, 0, 1}};
    ph_freecam_step(&c, &in, 1);
    CHECK(near(c.pos[2], PH_MOVE_SPEED, 1e-5f) && near(c.pos[0], 0, 1e-6f) && near(c.pos[1], 0, 1e-6f), "forward moves along +z");
    in.speed = 2;
    in.move[2] = 0, in.move[1] = 1;
    ph_freecam_step(&c, &in, 1);
    CHECK(near(c.pos[1], 2 * PH_MOVE_SPEED, 1e-5f), "up moves along up, speed multiplier");

    ph_freecam_reset(&c, NULL, 0);
    PhInput turn = {.yaw = 1};
    ph_freecam_step(&c, &turn, (PI / 2) / PH_TURN_SPEED);
    ph_freecam_view_delta(&c, 0, 0, 0, &d);
    float right[4] = {1, 0, 0, 1}, fw[4];
    xform(&d, right, fw);
    CHECK(near(fw[0], 0, 1e-5f) && near(fw[1], 0, 1e-5f) && near(fw[2], 1, 1e-5f), "yaw +90 degrees: forward is the entry right");
    PhInput fwd = {.move = {0, 0, 1}};
    ph_freecam_step(&c, &fwd, 1);
    CHECK(near(c.pos[0], PH_MOVE_SPEED, 1e-4f) && near(c.pos[2], 0, 1e-4f), "forward after the turn moves along entry +x");

    ph_freecam_reset(&c, NULL, 0);
    PhInput look_up = {.pitch = 1};
    ph_freecam_step(&c, &look_up, 0.3f);
    ph_freecam_view_delta(&c, 0, 0, 0, &d);
    float upish[4] = {0, sinf(0.3f * PH_TURN_SPEED), cosf(0.3f * PH_TURN_SPEED), 1};
    xform(&d, upish, q);
    CHECK(near(q[2], 1, 1e-5f), "+ pitch looks up");
    ph_freecam_step(&c, &look_up, 10);
    CHECK(c.pitch == PH_PITCH_MAX, "pitch clamped to +PH_PITCH_MAX");
    look_up.pitch = -1;
    ph_freecam_step(&c, &look_up, 20);
    CHECK(c.pitch == -PH_PITCH_MAX, "pitch clamped to -PH_PITCH_MAX");
    PhInput roll = {.roll = 1};
    ph_freecam_step(&c, &roll, 10);
    CHECK(c.roll == PH_ROLL_MAX, "roll clamped");

    ph_freecam_reset(&c, NULL, 0);
    PhInput zin = {.zoom = 1};
    ph_freecam_step(&c, &zin, PH_ZOOM_SPEED);
    CHECK(near(c.zoom, 2, 1e-5f), "zoom doubles in PH_ZOOM_SPEED seconds");
    ph_freecam_step(&c, &zin, 100);
    CHECK(c.zoom == PH_ZOOM_MAX, "zoom clamped to max");
    zin.zoom = -1;
    ph_freecam_step(&c, &zin, 100);
    CHECK(c.zoom == PH_ZOOM_MIN, "zoom clamped to min");

    ph_freecam_reset(&c, NULL, 5);
    ph_freecam_step(&c, &fwd, 10);
    float len = sqrtf(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
    CHECK(near(len, 5, 1e-4f), "max_dist clamps the position (%g)", len);

    ph_freecam_reset(&c, NULL, 0);
    ph_freecam_view_delta(&c, 0.1f, 0.05f, 4, &d);
    float onfocus[4] = {0.3f, -0.2f, 4, 1}, near_pt[4] = {0.3f, -0.2f, 2, 1};
    xform(&d, onfocus, q);
    CHECK(near(q[0], 0.3f, 1e-6f) && near(q[1], -0.2f, 1e-6f) && q[2] == 4, "lens: the focus plane stays");
    xform(&d, near_pt, q);
    CHECK(near(q[0], 0.3f - 0.05f, 1e-6f) && near(q[1], -0.2f - 0.025f, 1e-6f), "lens: a nearer point moves");
}

static void test_clip_matrix(void) {
    PhCamera cam = {.px = 1.2f, .py = 2.1f, .cx = 0.01f, .cy = -0.02f, .a = 1.001f, .b = -0.1001f, .k = 1};
    PhMat id, m, p;
    PhClipXform t;
    ph_mat_identity(&id);
    ph_clip_identity(&t);
    ph_clip_matrix(&cam, &id, 1, &t, &m);
    CHECK(mat_near(&m, &id, 1e-5f), "identity D, zoom 1, identity T: M = identity");

    PhFreeCam fc;
    ph_freecam_reset(&fc, NULL, 0);
    fc.pos[0] = 0.5f, fc.pos[2] = -1;
    fc.yaw = 0.3f;
    fc.pitch = -0.2f;
    PhMat d;
    ph_freecam_view_delta(&fc, 0, 0, 0, &d);
    t.sx = 2, t.sy = 2, t.ox = 0.3f, t.oy = -0.1f;
    ph_clip_matrix(&cam, &d, 1.5f, &t, &m);
    ph_camera_projection(&cam, &p);
    float v[4] = {1, 0.5f, 6, 1}, clip[4], dv[4], want[4], got[4];
    xform(&p, v, clip);
    xform(&d, v, dv);
    xform(&p, dv, want);
    want[0] *= 1.5f, want[1] *= 1.5f;
    want[0] = 2 * want[0] + 0.3f * want[3];
    want[1] = 2 * want[1] - 0.1f * want[3];
    xform(&m, clip, got);
    int ok = 1;
    for (int i = 0; i < 4; i++) ok &= near(got[i], want[i], 1e-3f);
    CHECK(ok, "M clip = T Z P D v (%g %g %g %g vs %g %g %g %g)", got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
}

static void test_plan(void) {
    PhPlan p;
    PhPlanIn in = {.out_w = 1280, .out_h = 720, .ss = 1, .frames = 64, .guard = -1};
    CHECK(ph_plan(&in, &p) == 0 && p.tiles_x == 1 && p.tiles_y == 1 && p.crop_w == 1280 && p.crop_h == 720 && p.crop_x == 0 &&
              p.crop_y == 0 && p.kx == 1 && p.ky == 1 && p.per_tile == 64 && p.frames == 64 && p.final_w == 1280 && p.final_h == 720,
          "1x native: one tile, crop = out, the frame budget");
    in = (PhPlanIn){.out_w = 1920, .out_h = 1080, .ss = 2, .frames = 90, .guard = -1};
    CHECK(ph_plan(&in, &p) == 0 && p.final_w == 1920 && p.final_h == 1080 && p.grid_w == 3840 && p.grid_h == 2160 &&
              p.tiles_x == 3 && p.tiles_y == 3 && p.crop_w == 1280 && p.crop_h == 720 && p.crop_x == 320 && p.crop_y == 180 &&
              near(p.kx, 2, 1e-6f) && near(p.ky, 2, 1e-6f) && p.per_tile == 10 && p.frames == 90,
          "ss 2 at 1920x1080: 3x3 tiles of 1280x720, k 2 (%dx%d %dx%d k %g)", p.tiles_x, p.tiles_y, p.crop_w, p.crop_h, p.kx);
    in = (PhPlanIn){.out_w = 1280, .out_h = 720, .ss = 1, .target_w = 3840, .frames = 0, .guard = -1};
    CHECK(ph_plan(&in, &p) == 0 && p.final_w == 3840 && p.final_h == 2160 && p.tiles_x == 4 && p.tiles_y == 4 && p.crop_w == 960 &&
              p.crop_h == 540 && p.per_tile == 1 && p.frames == 16 && near(p.kx, 3, 1e-6f),
          "4K from 1280x720: 4x4 tiles, at least one frame per tile");
    in = (PhPlanIn){.out_w = 1280, .out_h = 720, .ss = 4, .target_w = 3840, .frames = 16, .guard = -1};
    CHECK(ph_plan(&in, &p) == -1, "more than PH_TILES_MAX tiles: rejected");
    in = (PhPlanIn){.out_w = 8, .out_h = 720, .ss = 1};
    CHECK(ph_plan(&in, &p) == -1, "tiny output rejected");
    in = (PhPlanIn){.out_w = 64, .out_h = 36, .ss = 2, .frames = 9, .guard = -1};
    CHECK(ph_plan(&in, &p) == 0 && p.crop_w * p.tiles_x == p.grid_w && p.crop_h * p.tiles_y == p.grid_h && p.final_w >= 61 &&
              (p.out_w - p.crop_w) % 2 == 0 && (p.out_h - p.crop_h) % 2 == 0 && p.crop_w * 1.2f <= 64 && p.crop_h * 1.2f <= 36,
          "odd sizes: trimmed so the tiles divide the grid (final %dx%d, tiles %dx%d)", p.final_w, p.final_h, p.tiles_x, p.tiles_y);
    in = (PhPlanIn){.out_w = 1920, .out_h = 1080, .ss = 3, .frames = 4};
    CHECK(ph_plan(&in, &p) == 0 && p.ss == 1 && p.tiles_x == 1, "ss 3 counts as 1");
}

static void test_shots(void) {
    PhPlan p;
    PhPlanIn in = {.out_w = 1920, .out_h = 1080, .ss = 2, .frames = 45, .guard = -1};
    ph_plan(&in, &p);
    int count[PH_TILES_MAX][PH_TILES_MAX] = {{0}}, zero_ok = 1, range_ok = 1;
    for (int f = 0; f < p.frames; f++) {
        PhShot s;
        ph_plan_shot(&p, f, &s);
        count[s.tile_y][s.tile_x]++;
        if (s.sample == 0 && (s.jx != 0 || s.jy != 0 || s.lens_u != 0 || s.lens_v != 0)) zero_ok = 0;
        if (fabsf(s.jx) > 0.5f || fabsf(s.jy) > 0.5f || s.lens_u * s.lens_u + s.lens_v * s.lens_v > 1.0001f) range_ok = 0;
    }
    int cover = 1;
    for (int j = 0; j < p.tiles_y; j++)
        for (int i = 0; i < p.tiles_x; i++)
            if (count[j][i] != p.per_tile) cover = 0;
    CHECK(cover, "every tile exactly per_tile (%d) times", p.per_tile);
    CHECK(zero_ok, "sample 0: no jitter, no lens offset");
    CHECK(range_ok, "jitter within half a pixel, lens in the unit disc");
    PhShot s1;
    ph_plan_shot(&p, 1, &s1);
    CHECK(s1.sample == 1 && (s1.jx != 0 || s1.jy != 0), "sample 1 is jittered");

    /* Tile (1, 2) sample 0: its top-left NDC corner lands on the crop rectangle's top-left. */
    PhShot s;
    ph_plan_shot(&p, (2 * p.tiles_x + 1) * p.per_tile, &s);
    float x = -1 + 2.0f * 1 / p.tiles_x, y = 1 - 2.0f * 2 / p.tiles_y;
    float xn = s.t.sx * x + s.t.ox, yn = s.t.sy * y + s.t.oy;
    float px = (xn + 1) * 0.5f * p.out_w, py = (1 - yn) * 0.5f * p.out_h;
    CHECK(s.tile_x == 1 && s.tile_y == 2 && near(px, p.crop_x, 1e-2f) && near(py, p.crop_y, 1e-2f),
          "tile corner onto the crop corner (%g %g)", px, py);
    ph_plan_shot(&p, (2 * p.tiles_x + 1) * p.per_tile + 1, &s);
    xn = s.t.sx * x + s.t.ox, yn = s.t.sy * y + s.t.oy;
    px = (xn + 1) * 0.5f * p.out_w, py = (1 - yn) * 0.5f * p.out_h;
    CHECK(near(px, p.crop_x + s.jx, 1e-2f) && near(py, p.crop_y + s.jy, 1e-2f), "jitter moves the image by (jx, jy) pixels");
}

static float srgb_to_lin(int v) {
    float c = v / 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void test_accum(void) {
    PhPlan p;
    PhPlanIn in = {.out_w = 64, .out_h = 36, .ss = 2, .frames = 18, .guard = -1};
    CHECK(ph_plan(&in, &p) == 0 && p.tiles_x * p.tiles_y > 1, "small tiled plan (%dx%d)", p.tiles_x, p.tiles_y);
    uint32_t frame[64 * 36];
    const uint32_t col = 0x00406080u;
    for (int i = 0; i < 64 * 36; i++) frame[i] = col;
    PhAccum a;
    CHECK(ph_accum_init(&a, &p) == 0, "accum init");
    int n = p.final_w * p.final_h;
    float *rgb = malloc(sizeof(float) * 3 * n);
    uint32_t *out = malloc(sizeof(uint32_t) * n);
    /* Only the first tile. */
    for (int f = 0; f < p.per_tile; f++) {
        PhShot s;
        ph_plan_shot(&p, f, &s);
        ph_accum_add(&a, &s, frame, 64);
    }
    ph_accum_resolve(&a, rgb);
    int last = 3 * (n - 1);
    CHECK(near(rgb[0], srgb_to_lin(0x40), 1e-4f) && rgb[last] == 0 && rgb[last + 1] == 0 && rgb[last + 2] == 0,
          "half-done capture: tile 0 resolved, untouched tiles 0");
    for (int f = p.per_tile; f < p.frames; f++) {
        PhShot s;
        ph_plan_shot(&p, f, &s);
        ph_accum_add(&a, &s, frame, 64);
    }
    CHECK(a.added == p.frames, "added == frames (%d)", a.added);
    ph_accum_resolve(&a, rgb);
    int ok = 1;
    for (int i = 0; i < n; i++)
        if (!near(rgb[3 * i], srgb_to_lin(0x40), 1e-4f) || !near(rgb[3 * i + 1], srgb_to_lin(0x60), 1e-4f) ||
            !near(rgb[3 * i + 2], srgb_to_lin(0x80), 1e-4f))
            ok = 0;
    CHECK(ok, "a constant frame resolves to its colour everywhere");
    PhLook l;
    ph_look_default(&l);
    ph_finish(&l, rgb, p.final_w, p.final_h, out);
    ok = 1;
    for (int i = 0; i < n; i++)
        if ((out[i] & 0xFFFFFFu) != col) ok = 0;
    CHECK(ok, "accumulated and finished: the input colour back");
    ph_accum_free(&a);
    CHECK(a.sum == NULL, "accum free");
    free(rgb);
    free(out);
}

static void unpack(uint32_t p, int c[3]) { c[0] = (p >> 16) & 255, c[1] = (p >> 8) & 255, c[2] = p & 255; }

static void test_look(void) {
    enum { W = 32, H = 18 };
    float rgb[W * H * 3], base[W * H * 3];
    uint32_t in[W * H], out[W * H];
    for (int i = 0; i < W * H; i++) {
        in[i] = (uint32_t)((i * 7) & 255) << 16 | (uint32_t)((i * 13 + 5) & 255) << 8 | (uint32_t)((i * 29 + 9) & 255);
        int c[3];
        unpack(in[i], c);
        for (int k = 0; k < 3; k++) base[3 * i + k] = srgb_to_lin(c[k]);
    }
    PhLook l;
    ph_look_default(&l);
    CHECK(l.exposure == 0 && l.filter == PH_FILTER_NONE && l.saturation == 1 && l.vignette == 0 && l.grain == 0 && l.bars == PH_BARS_NONE,
          "default look");
    memcpy(rgb, base, sizeof rgb);
    ph_finish(&l, rgb, W, H, out);
    int ok = 1;
    for (int i = 0; i < W * H; i++) {
        int a[3], b[3];
        unpack(in[i], a), unpack(out[i], b);
        for (int k = 0; k < 3; k++)
            if (abs(a[k] - b[k]) > 1) ok = 0;
    }
    CHECK(ok, "identity look reproduces the sRGB input within 1 LSB");

    l.exposure = 1;
    memcpy(rgb, base, sizeof rgb);
    ph_finish(&l, rgb, W, H, out);
    int brighter = 1;
    for (int i = 1; i < W * H; i++) {
        int a[3], b[3];
        unpack(in[i], a), unpack(out[i], b);
        for (int k = 0; k < 3; k++)
            if (b[k] < a[k] || (a[k] > 0 && a[k] < 200 && b[k] <= a[k])) brighter = 0;
    }
    CHECK(brighter, "exposure +1 brightens");

    ph_look_default(&l);
    l.filter = PH_FILTER_BW;
    memcpy(rgb, base, sizeof rgb);
    ph_finish(&l, rgb, W, H, out);
    ok = 1;
    for (int i = 0; i < W * H; i++) {
        int c[3];
        unpack(out[i], c);
        if (c[0] != c[1] || c[1] != c[2]) ok = 0;
    }
    CHECK(ok, "bw: r == g == b");

    ph_look_default(&l);
    l.vignette = 1;
    memcpy(rgb, base, sizeof rgb);
    for (int i = 0; i < W * H * 3; i++) rgb[i] = 0.5f;
    ph_finish(&l, rgb, W, H, out);
    int cc[3], ce[3];
    unpack(out[0], cc), unpack(out[(H / 2) * W + W / 2], ce);
    CHECK(cc[0] < ce[0] / 4 && ce[0] > 180, "vignette darkens the corners (%d vs %d)", cc[0], ce[0]);

    ph_look_default(&l);
    l.grain = 1, l.seed = 7;
    for (int i = 0; i < W * H * 3; i++) rgb[i] = 0.2f;
    ph_finish(&l, rgb, W, H, out);
    uint32_t out2[W * H];
    for (int i = 0; i < W * H * 3; i++) rgb[i] = 0.2f;
    ph_finish(&l, rgb, W, H, out2);
    int differ = 0;
    for (int i = 1; i < W * H; i++) differ |= out[i] != out[0];
    CHECK(differ && !memcmp(out, out2, sizeof out), "grain: noisy and deterministic");

    CHECK(ph_bars_rows(PH_BARS_239, 1920, 1080) == 138, "2.39 on 1920x1080: 138 rows (%d)", ph_bars_rows(PH_BARS_239, 1920, 1080));
    CHECK(ph_bars_rows(PH_BARS_185, 1920, 1080) == 21, "1.85 on 1920x1080: 21 rows (%d)", ph_bars_rows(PH_BARS_185, 1920, 1080));
    CHECK(ph_bars_rows(PH_BARS_185, 2560, 1080) == 0 && ph_bars_rows(PH_BARS_NONE, 1920, 1080) == 0, "already wider / none: 0");
    ph_look_default(&l);
    l.bars = PH_BARS_239;
    for (int i = 0; i < W * H * 3; i++) rgb[i] = 1;
    ph_finish(&l, rgb, W, H, out);
    int top = ph_bars_rows(PH_BARS_239, W, H);
    CHECK(top > 0 && (out[0] & 0xFFFFFF) == 0 && (out[(H - 1) * W] & 0xFFFFFF) == 0 && (out[top * W] & 0xFFFFFF) == 0xFFFFFF,
          "bars rows black, the picture kept");

    CHECK(!strcmp(ph_filter_key(PH_FILTER_NOIR), "noir") && ph_filter_key(PH_FILTER_COUNT) == NULL && ph_filter_key(-1) == NULL,
          "filter keys");
    CHECK(!strcmp(ph_bars_key(PH_BARS_239), "2.39") && ph_bars_key(PH_BARS_COUNT) == NULL, "bars keys");

    uint32_t lut[256];
    ph_look_default(&l);
    ph_look_lut(&l, lut);
    ok = 1;
    for (int i = 0; i < 256; i++)
        if (lut[i] != ((uint32_t)i | (uint32_t)i << 8 | (uint32_t)i << 16)) ok = 0;
    CHECK(ok, "default look: identity LUT");
    l.filter = PH_FILTER_WARM;
    ph_look_lut(&l, lut);
    CHECK((lut[128] & 255) > ((lut[128] >> 16) & 255), "warm LUT: red over blue");
}

static void test_png(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/photo_test_%d.png", tmp, (int)getpid());
    enum { W = 23, H = 17, PITCH = 29 };
    uint32_t px[PITCH * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < PITCH; x++) px[y * PITCH + x] = 0xFF000000u | (uint32_t)(x * 11) << 16 | (uint32_t)(y * 15) << 8 | (uint32_t)((x ^ y) * 9);
    char err[128] = "";
    CHECK(ph_png_write(path, px, W, H, PITCH, err, sizeof err) == 0, "png written (%s)", err);
    int w = 0, h = 0, n = 0;
    unsigned char *img = stbi_load(path, &w, &h, &n, 3);
    int ok = img && w == W && h == H;
    for (int y = 0; ok && y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t p = px[y * PITCH + x];
            const unsigned char *q = img + 3 * (y * W + x);
            if (q[0] != ((p >> 16) & 255) || q[1] != ((p >> 8) & 255) || q[2] != (p & 255)) ok = 0;
        }
    CHECK(ok, "png round trip: size and pixels");
    stbi_image_free(img);
    remove(path);
    err[0] = 0;
    CHECK(ph_png_write("/nonexistent-dir-for-photo-test/x.png", px, W, H, PITCH, err, sizeof err) == -1 && err[0],
          "unwritable path: -1 with a reason (%s)", err);
}

static int taken_until;   /* names with N < taken_until (1 = the plain name) are taken */
static int fake_exists(const char *path) {
    const char *u = strrchr(path, '_');
    int n = 1;
    if (u && strlen(u) < 8) n = atoi(u + 1);   /* "_N.png" (the time part "_HHMMSS.png" is longer) */
    return n < taken_until;
}

static void test_path(void) {
    char buf[256];
    int len = ph_photo_path(buf, sizeof buf, "Photos", 2026, 11, 21, 9, 5, 3, NULL);
    CHECK(len == (int)strlen(buf) && !strcmp(buf, "Photos/WarriorsReborn_20261121_090503.png"), "photo path format (%s)", buf);
    taken_until = 2;
    ph_photo_path(buf, sizeof buf, "Photos", 2026, 11, 21, 9, 5, 3, fake_exists);
    CHECK(!strcmp(buf, "Photos/WarriorsReborn_20261121_090503_2.png"), "taken name: _2 (%s)", buf);
    taken_until = 4;
    ph_photo_path(buf, sizeof buf, "Photos", 2026, 11, 21, 9, 5, 3, fake_exists);
    CHECK(!strcmp(buf, "Photos/WarriorsReborn_20261121_090503_4.png"), "_2 and _3 taken: _4 (%s)", buf);
    CHECK(ph_photo_path(buf, 20, "Photos", 2026, 11, 21, 9, 5, 3, NULL) == -1, "too small a buffer: -1");
}

static float kv[3];
static int kcan[3] = {1, 0, 1};
static float get0(void) { return kv[0]; }
static float get1(void) { return kv[1]; }
static float get2(void) { return kv[2]; }
static void set0(float v) { kv[0] = v; }
static void set1(float v) { kv[1] = v; }
static void set2(float v) { kv[2] = v; }
static int can0(void) { return kcan[0]; }
static int can1(void) { return kcan[1]; }

static void test_state(void) {
    const PhKnob knobs[] = {
        {"ssgi", get0, set0, 1, can0},
        {"unsupported", get1, set1, 1, can1},
        {"already", get2, set2, 2, NULL},
    };
    kv[0] = 0.1f, kv[1] = 0.3f, kv[2] = 2;
    PhState s;
    ph_init(&s, knobs, 3);
    CHECK(s.stage == PH_OFF, "init: off");
    CHECK(ph_exit(&s) == -1, "exit while off: -1");
    PhPlanIn in = {.out_w = 1920, .out_h = 1080, .ss = 2, .frames = 18, .guard = -1};
    CHECK(ph_capture_begin(&s, &in) == -1, "capture_begin while off: -1");
    CHECK(ph_enter(&s) == 1 && s.stage == PH_ACTIVE, "enter: one knob changed");
    CHECK(kv[0] == 1 && kv[1] == 0.3f && kv[2] == 2, "photo value only where can() and it differs");
    CHECK(ph_enter(&s) == -1, "double enter: -1");
    kv[0] = 0.7f;   /* changed while active */
    kv[1] = 0.9f;   /* not overridden by photo mode: left alone on exit */
    CHECK(ph_capture_progress(&s) == 0, "progress 0 when not capturing");
    PhShot shot;
    CHECK(ph_capture_next(&s, &shot) == 0, "capture_next when not capturing: 0");
    PhPlanIn bad = {.out_w = 4, .out_h = 4};
    CHECK(ph_capture_begin(&s, &bad) == -1 && s.stage == PH_ACTIVE, "bad plan: -1, still active");
    CHECK(ph_capture_begin(&s, &in) == 0 && s.stage == PH_CAPTURING, "capture_begin");
    int n = 0, prog_ok = 1;
    float last = 0;
    while (ph_capture_next(&s, &shot)) {
        n++;
        float pr = ph_capture_progress(&s);
        if (pr < last || pr > 1) prog_ok = 0;
        last = pr;
    }
    CHECK(n == s.plan.frames && n == 18, "capture_next hands out plan.frames shots (%d)", n);
    CHECK(prog_ok && last == 1, "progress 0..1");
    CHECK(ph_capture_next(&s, &shot) == 0, "then 0");
    ph_capture_end(&s);
    CHECK(s.stage == PH_ACTIVE, "capture_end: active");
    CHECK(ph_exit(&s) == 1 && s.stage == PH_OFF, "exit restores one knob");
    CHECK(kv[0] == 0.1f && kv[1] == 0.9f && kv[2] == 2, "exact saved value back, other knobs untouched");
    CHECK(ph_exit(&s) == -1, "double exit: -1");

    kv[0] = 0.25f;
    CHECK(ph_enter(&s) == 1 && ph_capture_begin(&s, &in) == 0, "enter and capture again");
    ph_capture_next(&s, &shot);
    CHECK(ph_exit(&s) == 1 && s.stage == PH_OFF && kv[0] == 0.25f, "exit from capturing restores");
}

/* The "Y: show panel" hint after hiding the panel: full for most of PH_TOAST_MS, then fades out. */
static void test_toast(void) {
    CHECK(ph_toast_alpha(-1) == 0, "toast: nothing before it was shown");
    CHECK(ph_toast_alpha(0) == 1 && ph_toast_alpha(1000) == 1 && ph_toast_alpha(PH_TOAST_MS - PH_TOAST_FADE_MS) == 1, "toast: full at first");
    const float mid = ph_toast_alpha(PH_TOAST_MS - PH_TOAST_FADE_MS / 2);
    CHECK(mid > 0.49f && mid < 0.51f, "toast: half way through its fade (%g)", mid);
    CHECK(ph_toast_alpha(PH_TOAST_MS) == 0 && ph_toast_alpha(1e9) == 0, "toast: gone after %g ms", PH_TOAST_MS);
    CHECK(PH_TOAST_MS == 2000.0, "toast: shown for 2 s");
}

int main(void) {
    test_toast();
    test_matrices();
    test_camera();
    test_freecam();
    test_clip_matrix();
    test_plan();
    test_shots();
    test_accum();
    test_look();
    test_png();
    test_path();
    test_state();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
