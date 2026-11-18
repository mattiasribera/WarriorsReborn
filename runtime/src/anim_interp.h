/* Character animation rates and interpolation (--anim-interp): the pure part.
 *
 * The title's characters are GPU-skinned draws (gpu_skin.h) whose constants hold a bone palette: 3
 * rows (a 3 x 4 matrix, translation in column 3) per bone from the program's palette base. Each draw
 * has an identity across frames (hle_d3d.c skin_record: program, streams, first vertex and count;
 * the n-th draw of an identity in a frame matches last frame's n-th).
 *
 * AnimRate measures how often each identity's palette changes (every frame, every other frame, in
 * bursts) and, for the camera and other draws, how often their constants change: the evidence for
 * which motion steps slower than the display (--stats [anim] lines).
 *
 * anim_blend_bones blends two palettes and anim_bones_snap tells a snap (a bone moving or turning
 * more than a step should) from motion, so such a pair is not blended. The palette rows are
 * skinning matrices (the bone's pose times the inverse of its bind pose, relative to the character),
 * so they are lerped as they are: every joint stays where both poses put it, and sheared or
 * non-uniformly scaled bones (the title's jaw and lips) keep their shape. Re-orthonormalising the
 * rotation part instead moved the lips by centimetres (their matrices map points about 1.6 units
 * from the origin). A lerp shrinks a turning bone towards its joint by 1 - cos(angle / 2) at the
 * midpoint (6% at 40 degrees), hence the snap limit on turns. */
#ifndef ANIM_INTERP_H
#define ANIM_INTERP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Identities tracked per frame (a fight frame draws about 30-60 character parts). */
#define ANIM_RATE_SLOTS 1024
/* Hold lengths counted: 1, 2, 3 and 4 or more frames. */
#define ANIM_RATE_HOLDS 4

typedef struct {
    uint64_t key, hash;
    uint32_t since;   /* the frame its hash last changed (or first appeared) */
    uint32_t used;
} AnimRateSlot;

/* One kind of draw (characters, objects): this and last frame's identities. */
typedef struct {
    AnimRateSlot tab[2][ANIM_RATE_SLOTS];
    uint32_t count[2];       /* identities in each table (filled to 3/4 at most) */
    int cur;
    uint32_t frame;          /* frames begun */
    uint64_t draws;          /* matched draws (seen last frame too) in the window */
    uint64_t fresh;          /* of those, with a hash unlike last frame's */
    uint64_t hold[ANIM_RATE_HOLDS];   /* completed holds: frames a hash stayed before it changed */
    uint64_t dropped;        /* draws not tracked (table full) */
} AnimRate;

/* Clears everything (the window and the identities). */
void anim_rate_init(AnimRate *r);
/* Starts a frame: this frame's identities become last frame's. */
void anim_rate_frame(AnimRate *r);
/* The n-th draw of identity id in this frame with constants hashing to hash. Returns 1 when it was
 * drawn last frame with another hash (new pose), 0 when with the same hash (held), -1 when not drawn
 * last frame (or not tracked). */
int anim_rate_draw(AnimRate *r, uint64_t id, uint64_t hash);
/* Clears the window's counts (identities kept). */
void anim_rate_window(AnimRate *r);
/* "draws D, new P%, holds 1/2/3/4+: a / b / c / d" */
void anim_rate_format(const AnimRate *r, char *out, size_t cap);

/* FNV-1a over n rows of 4 floats (bit patterns). */
uint64_t anim_hash_rows(const float (*rows)[4], int n);

/* Palette size: bones of 3 rows from base, at most ANIM_MAX_BONES and up to row 191. The title's
 * character programs read 32 bones from c-36 (row 60); the rows after them hold other constants
 * (c69-c72, row 165, is the world-to-clip matrix), which must neither count as a pose change nor be
 * blended as bones. */
#define ANIM_MAX_BONES 32
int anim_palette_bones(int base);

/* out = blend of palettes a and b at t in [0, 1] (0 = a, 1 = b, exactly) for nbones bones from row
 * base: each bone's 3 x 4 rows lerped. Other rows of out are not written. */
void anim_blend_bones(const float (*a)[4], const float (*b)[4], int base, int nbones, float t, float (*out)[4]);

/* A bone moves more than ANIM_SNAP_DIST units or turns more than ANIM_SNAP_DEG between a and b: a
 * snap, or a turn the lerp would shrink too much (16% at 115 degrees), shown as is. The title's
 * bones are relative to the character (its placement is in the world-to-clip matrix), within about
 * 1.5 units of its origin; in fights limbs move up to about 1.4 units and turn up to about 115
 * degrees in one 1/30 s step. Non-finite values count as a snap.
 * Returns 1 for a snap. */
#define ANIM_SNAP_DIST 2.0f
#define ANIM_SNAP_DEG 90.0f
int anim_bones_snap(const float (*a)[4], const float (*b)[4], int base, int nbones);

/* --anim-interp: a draw whose motion rows (a character's bone palette, a world-to-clip matrix) step
 * slower than the display (held for `period` frames, then new values) is shown blended between its
 * last two states, t = (frames since the change + 1) / period: with new values every other frame
 * the display shows A, mid(A, B), B, mid(B, C), ... evenly spaced, at the cost of (period - 1)
 * display frames of delay. Values that change every frame (period 1), ones held longer than
 * ANIM_MAX_PERIOD frames before they moved (a start from rest), a bone snap (anim_bones_snap) and a
 * change in a frame marked as a cut (anim_poses_frame) are shown as they are. */
#define ANIM_MAX_PERIOD 4
/* Draw identities tracked per frame (a cutscene frame draws about 700 GPU vertex-program draws). */
#define ANIM_POSE_SLOTS 4096

typedef struct {
    int on;
} AnimInterpOptions;
/* The command line's --anim-interp / --no-anim-interp (main.c). */
extern AnimInterpOptions anim_interp_options;

/* The motion rows of a draw's constants: nbones bones of 3 rows from row base (blended as bones;
 * nbones 0 = none) and lin_n rows from lin_first (lerped; 0 = none), all inside the 192 rows. */
typedef struct {
    int base, nbones, lin_first, lin_n;
} AnimRows;

typedef struct {
    uint64_t key;
    uint32_t since, period, used, rows;
    uint32_t cur, prev;   /* offsets (rows) of the current and the previous motion rows in the frame's buffer; prev UINT32_MAX = none */
} AnimPoseSlot;

typedef struct {
    AnimPoseSlot tab[2][ANIM_POSE_SLOTS];
    uint32_t count[2];
    float (*buf[2])[4];
    size_t cap[2], used[2];   /* rows */
    int cur, cut;
    uint32_t frame;
    uint64_t draws, blended, snaps;   /* counts since anim_poses_window */
} AnimPoses;

/* Empty (no allocation until the first draw). */
void anim_poses_init(AnimPoses *p);
void anim_poses_free(AnimPoses *p);
/* Starts a frame; cut: the camera cut in this frame, so values that change in it are not blended
 * with the ones before. */
void anim_poses_frame(AnimPoses *p, int cut);
/* The n-th draw of identity id this frame, its constants c (192 rows) with motion rows m. Returns 1
 * and writes the motion rows of out with the values to show when they differ from c's, else 0 (out
 * not written). Other rows of out are never written. */
int anim_poses_draw(AnimPoses *p, uint64_t id, const float (*c)[4], const AnimRows *m, float (*out)[4]);
void anim_poses_window(AnimPoses *p);

#ifdef __cplusplus
}
#endif

#endif
