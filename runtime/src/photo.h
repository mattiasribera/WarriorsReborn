/* Photo mode: the pure parts, game- and renderer-independent and not
 * thread-safe (tests/photo_test.c). The runtime glue is src/hle_photo.c, the frozen-frame replay in
 * src/hle_d3d.c (hle_d3d_photo_*).
 *
 * Conventions. Matrices are column form, m[r][c], acting on column vectors: clip = M * [x y z 1].
 * A world-to-clip matrix as the title hands it to a vertex program (c69-c72, "wvp") is that form:
 * row r of it is clip component r (x y z w). View space is x right, y up, z forward (the view
 * distance), as the title's D3D (left-handed) cameras: a perspective projection P maps it to clip
 *   x = px * x + cx * z,  y = py * y + cy * z,  z = a * z + b,  w = k * z.
 * The free camera lives in the view space of the title's camera at the moment photo mode froze the
 * frame (the "entry view"); D maps entry-view points to photo-view points, and the clip-space
 * matrix of a recorded 3D draw becomes M * clip with M = T * P * D * P^-1 (ph_clip_matrix), T the
 * clip-space zoom / tile / jitter (PhClipXform). */
#ifndef PHOTO_H
#define PHOTO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 4x4 matrices ---- */
typedef struct {
    float m[4][4];
} PhMat;
void ph_mat_identity(PhMat *a);
void ph_mat_mul(const PhMat *a, const PhMat *b, PhMat *out);   /* out = a * b (out may alias neither) */
int ph_mat_invert(const PhMat *a, PhMat *out);                /* 0, or -1 when singular (out = identity) */
/* a with its translation column (m[0..2][3]) zeroed: the rotation part of a rigid D, for draws
 * whose view has no translation (the sky dome). */
void ph_mat_rotation_only(const PhMat *a, PhMat *out);

/* ---- the title's camera ---- */
/* The projection and view decomposed from a world-to-clip matrix whose view part is rigid (a
 * world program's c69-c72: world = identity): rows 0-3 = clip x y z w over [p 1]. */
typedef struct {
    float px, py, cx, cy;   /* clip x = px x + cx z, clip y = py y + cy z */
    float a, b, k;          /* clip z = a z + b, clip w = k z */
    float view[4][4];       /* world -> view (column form): rows right, up, forward, 0 0 0 1 */
    float eye[3];           /* camera position (world) */
} PhCamera;
/* 0 and *c filled, or -1 when wvp is not a perspective camera with a rigid view (w row without a
 * direction, rows not orthogonal within 1e-3 after removing the projection's centre terms,
 * non-finite values). */
int ph_camera_from_wvp(const float wvp[4][4], PhCamera *c);
void ph_camera_projection(const PhCamera *c, PhMat *p);   /* P: view -> clip */

/* ---- the free camera (entry-view space) ---- */
#define PH_ZOOM_MIN 0.5f        /* zoom factor: 1 = the title's field of view, < 1 wider */
#define PH_ZOOM_MAX 6.0f
#define PH_PITCH_MAX 1.48f      /* radians from the horizon (about 85 degrees) */
#define PH_ROLL_MAX 0.785398f   /* +-45 degrees */
#define PH_MOVE_SPEED 2.5f      /* units per second at full stick */
#define PH_TURN_SPEED 1.6f      /* radians per second at full stick */
#define PH_ROLL_SPEED 0.6f
#define PH_ZOOM_SPEED 1.0f      /* zoom doubles / halves in this many seconds at full input: factor 2^(input * dt / speed) */
typedef struct {
    float pos[3];       /* camera position in the entry view */
    float yaw, pitch;   /* yaw about world up, pitch from the horizon (radians) */
    float roll;         /* about the camera's forward axis (radians) */
    float zoom;         /* PH_ZOOM_MIN..PH_ZOOM_MAX */
    float up[3];        /* world up in the entry view (unit) */
    float yaw0, pitch0; /* the entry camera's own heading / pitch against up (ph_freecam_reset) */
    float max_dist;     /* |pos| limit (units); <= 0: none */
} PhFreeCam;
/* The camera at the entry view (pos 0, the entry camera's orientation, zoom 1, roll 0). up_view:
 * world up in the entry view (normalized here; a zero vector means (0, 1, 0)). */
void ph_freecam_reset(PhFreeCam *c, const float up_view[3], float max_dist);
typedef struct {
    float move[3];      /* -1..1: right, up (world up), forward (along the camera's heading on the ground plane) */
    float yaw, pitch;   /* -1..1 rates: + = turn right, + = look up */
    float roll;         /* -1..1 rate: + = clockwise */
    float zoom;         /* -1..1 rate: + = zoom in */
    float speed;        /* move speed multiplier (1 = PH_MOVE_SPEED; 0 means 1) */
} PhInput;
/* Integrates one step of dt seconds: angles clamped (pitch to +-PH_PITCH_MAX, roll to
 * +-PH_ROLL_MAX), zoom to its range, the position clamped back onto the max_dist sphere. */
void ph_freecam_step(PhFreeCam *c, const PhInput *in, float dt);
/* D: entry view -> photo view (rigid), with a lens offset (lens_x, lens_y: view units on the lens
 * plane, the camera's right / up) sheared back so points at forward distance focus (> 0) keep their
 * place (accumulated depth of field); lens 0 0 or focus <= 0: no shear. */
void ph_freecam_view_delta(const PhFreeCam *c, float lens_x, float lens_y, float focus, PhMat *d);

/* ---- clip space: zoom, tile, jitter ---- */
typedef struct {
    float sx, sy;   /* x' = sx x + ox w, y' = sy y + oy w */
    float ox, oy;
} PhClipXform;
void ph_clip_identity(PhClipXform *t);
/* M = T * Z * P * D * P^-1, Z the free camera's zoom on x and y (zoom = c->zoom). */
void ph_clip_matrix(const PhCamera *cam, const PhMat *d, float zoom, const PhClipXform *t, PhMat *m);

/* ---- the capture plan (accumulation, supersampling, tiles) ---- */
#define PH_GUARD_DEFAULT 0.1f   /* overscan per side of a tile, against screen-space edge effects */
#define PH_TILES_MAX 8          /* per axis */
typedef struct {
    int out_w, out_h;   /* the renderer's output size (a replayed frame) */
    int ss;             /* supersampling per axis: 1, 2 or 4 (others: 1) */
    int target_w;       /* the saved width (height keeps out's aspect, rounded); 0 = out_w */
    int frames;         /* frame budget of the accumulation (at least one per tile) */
    float guard;        /* minimum overscan per side; < 0: PH_GUARD_DEFAULT */
} PhPlanIn;
typedef struct {
    int out_w, out_h;       /* the replayed frame (PhPlanIn's) */
    int final_w, final_h;   /* the saved image */
    int ss;                 /* supersampling per axis (grid = final * ss) */
    int grid_w, grid_h;     /* the rendered pixel grid */
    int tiles_x, tiles_y;
    int crop_w, crop_h;     /* grid pixels each tile keeps (cw * tiles_x = grid_w) */
    int crop_x, crop_y;     /* the kept rectangle's top-left in a replayed frame */
    float kx, ky;           /* the tile's clip scale (frame NDC = (NDC - tile centre) * k) */
    int per_tile;           /* accumulated frames per tile (>= 1) */
    int frames;             /* tiles_x * tiles_y * per_tile */
} PhPlan;
/* 0, or -1 for a bad input (sizes < 16, more than PH_TILES_MAX tiles per axis, a grid that does
 * not fit). With a single tile and target_w = out_w * ... = the frame itself (no guard, crop = out). */
int ph_plan(const PhPlanIn *in, PhPlan *p);
typedef struct {
    int tile_x, tile_y, sample;   /* tile (row 0 at the top) and its sample index */
    float jx, jy;                 /* sub-pixel jitter in grid pixels (-0.5..0.5; Halton 2, 3) */
    float lens_u, lens_v;         /* a point of the unit disc (concentric map of Halton 5, 7); 0 0 at sample 0 */
    PhClipXform t;                /* the tile's clip transform, jitter included */
} PhShot;
/* Frame f (0..frames-1) of the plan: tiles in order, per_tile samples each. */
void ph_plan_shot(const PhPlan *p, int f, PhShot *s);

/* ---- accumulation (linear light) ---- */
typedef struct {
    PhPlan plan;
    float *sum;       /* final_w * final_h * 3 */
    uint32_t *count;  /* final_w * final_h: grid samples each final pixel received */
    int added;        /* frames added */
} PhAccum;
int ph_accum_init(PhAccum *a, const PhPlan *p);   /* 0, -1 out of memory */
void ph_accum_free(PhAccum *a);
/* Adds a replayed frame (XRGB8888, out_w x out_h, pitch in pixels) of shot s: its crop rectangle
 * into the tile's final pixels (each grid pixel to final pixel grid / ss), sRGB -> linear. */
void ph_accum_add(PhAccum *a, const PhShot *s, const uint32_t *px, int pitch);
/* The mean (linear rgb, final_w * final_h * 3) of what was added: each final pixel divided by the
 * samples it received (0 when none). */
void ph_accum_resolve(const PhAccum *a, float *rgb);

/* ---- the look (photo adjustments) and the final image ---- */
enum { PH_FILTER_NONE, PH_FILTER_WARM, PH_FILTER_COOL, PH_FILTER_SEPIA, PH_FILTER_BW, PH_FILTER_NOIR, PH_FILTER_FADED, PH_FILTER_COUNT };
enum { PH_BARS_NONE, PH_BARS_185, PH_BARS_239, PH_BARS_COUNT };   /* letterbox to 1.85:1 / 2.39:1 */
typedef struct {
    float exposure;     /* stops, -3..3 */
    int filter;         /* PH_FILTER_* */
    float saturation;   /* 0..2, 1 = unchanged (applied to the photo only, after the filter) */
    float vignette;     /* 0..1: corner darkening */
    float grain;        /* 0..1: film grain amplitude */
    int bars;           /* PH_BARS_* */
    uint32_t seed;      /* grain noise seed */
} PhLook;
void ph_look_default(PhLook *l);   /* exposure 0, no filter, saturation 1, no vignette, grain or bars */
const char *ph_filter_key(int filter);   /* none|warm|cool|sepia|bw|noir|faded; NULL out of range */
const char *ph_bars_key(int bars);       /* none|1.85|2.39 */
/* The letterbox of a w x h image for bars: rows [0, top) and [h - top, h) are black. */
int ph_bars_rows(int bars, int w, int h);
/* Applies the look to linear rgb (w x h x 3, in place) and writes XRGB8888 (sRGB) to out (w x h):
 * exposure, filter, saturation, vignette, grain (in display space), bars. */
void ph_finish(const PhLook *l, float *rgb, int w, int h, uint32_t *out);
/* A per-channel display-space LUT approximating the look for the live preview (exposure and the
 * filter's tint; saturation, vignette and grain only reach the photo): lut[i] = r | g << 8 | b << 16. */
void ph_look_lut(const PhLook *l, uint32_t lut[256]);

/* The hint shown after the panel is hidden ("Y: show panel"): its opacity elapsed_ms after it was
 * shown: 1 until PH_TOAST_MS - PH_TOAST_FADE_MS, then a linear fade to 0 at PH_TOAST_MS (0 before 0). */
#define PH_TOAST_MS 2000.0
#define PH_TOAST_FADE_MS 500.0
float ph_toast_alpha(double elapsed_ms);

/* ---- files ---- */
/* Writes XRGB8888 (pitch in pixels) as an 8-bit RGB PNG. 0, or -1 with a reason in err. */
int ph_png_write(const char *path, const uint32_t *px, int w, int h, int pitch, char *err, size_t errn);
/* "<dir>/WarriorsReborn_YYYYMMDD_HHMMSS.png" for the broken-down local time tm_* (year with the
 * century, month 1-12), or "..._HHMMSS_N.png" with N = 2.. when exists(path) says the name is
 * taken (exists may be NULL: never taken). Returns the length, -1 when it does not fit. */
int ph_photo_path(char *buf, size_t n, const char *dir, int year, int month, int day, int hour, int min, int sec,
                  int (*exists)(const char *path));

/* ---- the state machine: enter / capture / exit with the settings snapshot ---- */
/* A setting photo mode overrides: get / set the current value, photo = the value while in photo
 * mode; can (NULL: always) says whether the override is possible in this run (else left alone). */
typedef struct {
    const char *name;
    float (*get)(void);
    void (*set)(float v);
    float photo;
    int (*can)(void);
} PhKnob;
#define PH_KNOBS_MAX 48
enum { PH_OFF, PH_ACTIVE, PH_CAPTURING };
typedef struct {
    int stage;                    /* PH_* */
    const PhKnob *knobs;
    int nknobs;
    float saved[PH_KNOBS_MAX];    /* the values at ph_enter */
    unsigned char changed[PH_KNOBS_MAX];   /* overridden by ph_enter (restored by ph_exit) */
    PhPlan plan;
    int frame;                    /* next capture frame */
} PhState;
void ph_init(PhState *s, const PhKnob *knobs, int n);   /* PH_OFF; n clamped to PH_KNOBS_MAX */
/* PH_OFF -> PH_ACTIVE: saves every knob and sets the photo value of those that can (and differ).
 * Returns the number of knobs changed, -1 when not PH_OFF. */
int ph_enter(PhState *s);
/* -> PH_OFF from either stage: every changed knob back to its saved value (exactly, whatever was
 * set in between). Returns the number restored, -1 when already PH_OFF. */
int ph_exit(PhState *s);
/* PH_ACTIVE -> PH_CAPTURING with the plan of in: 0, -1 (not active, or a bad plan). */
int ph_capture_begin(PhState *s, const PhPlanIn *in);
/* The next frame's shot while capturing: 1 (s filled, frame advanced), 0 when every frame of the plan
 * was handed out (the capture is complete) or not capturing. */
int ph_capture_next(PhState *s, PhShot *shot);
float ph_capture_progress(const PhState *s);   /* 0..1 of the frames handed out; 0 when not capturing */
/* PH_CAPTURING -> PH_ACTIVE (done or cancelled). */
void ph_capture_end(PhState *s);

#ifdef __cplusplus
}
#endif
#endif
