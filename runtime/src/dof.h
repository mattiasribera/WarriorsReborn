/* --cutscene-dof: cinematic depth of field in the title's in-engine
 * cutscenes. The options, the gate that fades it in and out with the cutscene letterbox bars
 * (cinema_bars.h), the camera-cut test on the title's camera, and the math the renderer runs on
 * the GPU (shaders/dof_*.frag: the focus pick and smoothing, the circle of confusion) with the
 * parameters dof_params computes (RDof). Keep dof_focus_pick / dof_focus_step / dof_coc in step
 * with shaders/dof_focus.frag and dof_common.glsl. */
#ifndef DOF_H
#define DOF_H
#include <stdint.h>
#include "renderer.h"

#define DOF_STRENGTH_DEFAULT 1.0f
#define DOF_STRENGTH_MIN 0.25f
#define DOF_STRENGTH_MAX 2.0f
typedef struct {
    int on;          /* --cutscene-dof / --no-cutscene-dof */
    float strength;  /* --cutscene-dof-strength 0.25-2: scales the circle of confusion */
    int speaker;     /* --cutscene-dof-speaker 0|1 (default 1): focus on the character who speaks */
} DofConfig;
#define DOF_DEFAULTS {0, DOF_STRENGTH_DEFAULT, 1}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a depth-of-field option, -1 for a missing or out-of-range value (c unchanged). */
int dof_arg(DofConfig *c, const char *opt, const char *val);

/* The renderer parameters for a strength (clamped to 0.25-2): the CoC scales with it; the focus
 * pick and its timing are fixed. */
void dof_params(float strength, RDof *p);

/* The bars gate: the weight moves linearly toward 1 while the frame shows the cutscene bars and
 * toward 0 without them, over DOF_FADE_S seconds (dt at most 0.1 s per step); the weight used is
 * its smoothstep. *started is set when the weight leaves 0 (a new cutscene: the focus snaps). */
#define DOF_FADE_S 0.3f
typedef struct {
    float t;   /* 0-1, linear */
} DofGate;
float dof_gate_step(DofGate *g, int bars, float dt, int *started);

/* 1 when the title's camera jumped between two frames (D3D view matrices, row vectors: world x M =
 * view): its position moved more than DOF_CUT_DIST view units or its view direction or up axis
 * turned more than DOF_CUT_DEG degrees, or a matrix is not finite. */
#define DOF_CUT_DIST 1.5f
#define DOF_CUT_DEG 20.0f
int dof_cut(const float prev[16], const float cur[16]);
/* The camera position in the world of a view matrix (eye x M = 0). */
void dof_eye(const float m[16], float eye[3]);

/* The focus meter: the weighted `pct` percentile of the view distances d[0..n) (weights w; d <= 0
 * or not finite: nothing drawn there, left out), not nearer than focus_min. Returns 0 when nothing
 * was drawn there. */
float dof_focus_pick(const float *d, const float *w, int n, float pct, float focus_min);
/* The grid the GPU meters (shaders/dof_focus.frag): DOF_GRID_X x DOF_GRID_Y points over the centre
 * of the frame, u DOF_GRID_U0-U1, v DOF_GRID_V0-V1, each weighted by dof_grid_weight. */
#define DOF_GRID_X 9
#define DOF_GRID_Y 7
#define DOF_GRID_U0 0.28f
#define DOF_GRID_U1 0.72f
#define DOF_GRID_V0 0.22f
#define DOF_GRID_V1 0.78f
float dof_grid_weight(float u, float v);

/* One smoothing step of the focus (log2 view distance) toward the meter (log2; meter 0 / invalid:
 * the focus holds): exponential with time constant tau; snap sets it. */
float dof_focus_step(float focus_log, float meter_log, int meter_valid, float dt, float tau, int snap);

/* The signed circle of confusion in target pixels (negative: nearer than the focus) of view
 * distance d with focus distance f, for a target `height` pixels high. */
float dof_coc(const RDof *p, float f, float d, float height);

/* Characters stay sharp: the focus pass also scans the character
 * mask (renderer_rain_chars_on's depth of the frame's RENDERER_CHARACTER draws) on a denser
 * DOF_CGRID_X x DOF_CGRID_Y grid over the central area (u DOF_CGRID_U0-U1, v DOF_CGRID_V0-V1); a
 * sample shows a character where the visible depth is the mask's (dof_char_visible). */
#define DOF_CGRID_X 16
#define DOF_CGRID_Y 12
#define DOF_CGRID_U0 0.20f
#define DOF_CGRID_U1 0.80f
#define DOF_CGRID_V0 0.15f
#define DOF_CGRID_V1 0.85f
/* 1 when a pixel whose visible view distance is ds shows the character mask's surface at view
 * distance dc (dc <= 0 or not finite: no character there), within the depth formats' tolerance
 * (shaders/ssao_common.glsl is_char). */
int dof_char_visible(float dc, float ds);
/* The sharp range [range[0], range[1]] (view units) of a frame: with visible characters on the
 * character grid (cd[0..cn): their view distances, 0 where there is none), from the nearest one
 * divided by (1 + p->char_margin) to the farthest times (1 + p->char_margin), both at least
 * p->focus_min; without, the centre meter (dof_focus_pick of d, w over the 9 x 7 grid) at both
 * ends: today's centre autofocus. Returns 2 (characters), 1 (meter) or 0 (nothing metered: range
 * unchanged, the focus holds). */
int dof_focus_range(const RDof *p, const float *d, const float *w, int n, const float *cd, int cn, float range[2]);
/* The signed circle of confusion with a sharp range: 0 for a character pixel and for view
 * distances in [near, far]; nearer, dof_coc with the focus at near; farther, dof_coc with the
 * focus at far (both clamped to focus_min). near == far is dof_coc exactly. */
float dof_coc_range(const RDof *p, float near, float far, float d, int character, float height);

/* Focus on the speaker (--cutscene-dof-speaker). The in-engine cutscenes
 * play one 2D stereo dialogue track (vags/speeches lNN_cN) and bake their lip sync into the scene's
 * joint animation (the .scn's Maya source is `*_lipsync_complete.ma`), so the game's own data that
 * names the speaker is the face: every human shares one 34-joint rig and the character vertex
 * program's bone palette holds 32 skinning matrices (3 constant rows each, slot k = joint k + 2;
 * the title's rig), so the jaw, lips and mouth corners move against the head only on the
 * character whose line plays. Each frame's character draws give one DofFace (dof_face_palette over
 * the palette, plus the view distance and screen centre of some of its vertices); dof_speaker_step
 * tracks the characters, measures each one's mouth speed and picks the speaker with hysteresis. */
#define DOF_FACES_MAX 32                     /* faces recorded per frame */
#define DOF_PAL_SLOTS 32
#define DOF_PAL_HEAD 4                       /* bip_sw_head (joint 6) */
#define DOF_MOUTH_SLOTS 7                    /* jaw, lower lip, mouth corners (4), upper lip */
extern const int dof_mouth_slot[DOF_MOUTH_SLOTS];   /* 6 7 8 9 10 11 13 */
typedef struct {
    float head[12];                   /* the head slot's rows (groups the draws of one character) */
    float key[3];                     /* its translation (tracks the character over frames) */
    float mouth[DOF_MOUTH_SLOTS][9];  /* each mouth joint's 3x3 against the head: inverse(head) x slot */
    float dist;                       /* view distance of the sampled vertices (median; 0: unknown) */
    float ndc[2];                     /* their mean screen position (-1..1) */
} DofFace;
/* The face of a palette (c: constants, `base`: the row of slot 0; slot k at base + 3k .. +2, rows of
 * a column-vector 3x4 matrix). Returns 0 when the head's matrix is not finite or not invertible
 * (not a human's palette); dist and ndc are left 0. */
int dof_face_palette(const float (*c)[4], int base, DofFace *f);

/* The tracker. Mouth speed: the sum over the mouth joints of the change of their 3x3 against the
 * head (Frobenius) per second between two animation steps (a frame where no tracked character's
 * palette changed is not a step: the title animates cutscenes at 30 Hz without --sim60; after 0.1 s
 * without one the mouths count as resting), smoothed
 * with time constant DOF_SPK_TAU. A character becomes the speaker when his speed is at least
 * DOF_SPK_ON and (with a speaker already) DOF_SPK_RATIO times the speaker's, for DOF_SPK_PICK
 * seconds in a row, and has been for at least DOF_SPK_HOLD (no flicker on short lines and pauses);
 * the speaker is dropped after DOF_SPK_DROP seconds under DOF_SPK_OFF or out of view (in view: drawn,
 * its vertices' centre on the screen, nearer than DOF_SPK_MAX_DIST). The speaker
 * weight moves linearly to 1 with a speaker and to 0 without over DOF_SPK_FADE seconds. */
#define DOF_SPK_TRACKS 12
#define DOF_SPK_MATCH 0.75f     /* units of the palette: a face this near a track's last key is the same character */
#define DOF_SPK_JUMP 2.0f       /* a mouth change this large in one step is another character (a new track) */
#define DOF_SPK_TAU 0.3f
#define DOF_SPK_ON 6.0f         /* level51's intro: a talking mouth 6-25 per second, the listeners 0.2-4 */
#define DOF_SPK_OFF 3.0f
#define DOF_SPK_RATIO 2.0f
#define DOF_SPK_PICK 0.2f
#define DOF_SPK_HOLD 0.8f
#define DOF_SPK_DROP 0.8f
#define DOF_SPK_FADE 0.4f
#define DOF_SPK_MAX_DIST 15.0f  /* view units: a farther character is never the speaker (an extra in a wide shot) */
typedef struct {
    int used, seen;
    float key[3], mouth[DOF_MOUTH_SLOTS][9];
    float speed;          /* smoothed mouth speed */
    float dist, ndc[2];   /* of the last frame it was seen in */
    float unseen;         /* seconds out of view */
    uint32_t id;
} DofTrack;
typedef struct {
    DofTrack t[DOF_SPK_TRACKS];
    int speaker, pending;          /* track indices, -1: none */
    float pending_s, held_s, quiet_s, since_step;
    float weight, dist;
    uint32_t next_id;
} DofSpeaker;
typedef struct {
    float weight;   /* 0-1: how much the speaker sets the focus (0: step 1's sharp range) */
    float dist;     /* the speaker's view distance (the last one while fading out) */
    int changed;    /* the speaker changed this step (another character, or none) */
    uint32_t id;    /* the speaker's track id (0: none) */
    float speed;    /* his mouth speed */
} DofSpeakerOut;
void dof_speaker_reset(DofSpeaker *s);
/* One frame: the faces of its character draws (several draws of one palette are one character;
 * dist 0 or |ndc| > 1.05: not in view), the seconds since the last frame. */
void dof_speaker_step(DofSpeaker *s, const DofFace *f, int n, float dt, DofSpeakerOut *o);

/* The signed circle of confusion with a speaker: as dof_coc_range for a pixel that is no character;
 * a character pixel gets dof_coc_range's own value (the sharp range is the speaker's) times
 * p->char_blur, at most p->char_cap of the far field's largest CoC either way, times the speaker
 * weight `spk`: the speaker sharp, a character nearer or farther soft but never as soft as the
 * background. spk 0 is dof_coc_range exactly. */
float dof_coc_speaker(const RDof *p, float near, float far, float d, int character, float spk, float height);
/* The sharp range around a speaker at view distance d: d / (1 + char_margin) to d x (1 + char_margin),
 * both at least focus_min. */
void dof_speaker_range(const RDof *p, float d, float range[2]);
#endif
