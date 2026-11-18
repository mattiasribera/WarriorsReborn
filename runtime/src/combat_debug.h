/* --combat-debug: combat hit/hurt volume viewer and frame-data log.
 *
 * How the title tests hits (decompiled; addresses are XBE virtual addresses):
 *   - Every human (pool 0x4ff878, 60 slots of 0x668) has a collision body at human+0x170 (owner back
 *     pointer body+0x34). body+0x6c lists its part shapes (spheres and capsules on bones); a part id
 *     is shape+0x29, its flags shape+0x2a (1 strike capable, 2 strike on, 4 hurt volume).
 *   - Clip events turn strike parts on and off (FUN_00013d70, fired by the anim cursor FUN_000155c0
 *     when the whole-frame counter reaches the event frame): event 0xf -> message 0x8f ->
 *     FUN_001625d0 -> FUN_0013f460(part, 1) -> FUN_0024bc40 sets bit (part - 2) of the strike mask
 *     body+0xa0 and flag 2 of that part's shape; event 0x10 turns it off, 0x13 / 0x14 the whole
 *     body (charges), and FUN_0013f460(-1, 0) clears all. A held weapon's swing adds temporary
 *     spheres to body+0xa4 that live body+0xb0 world updates (FUN_00162370).
 *   - Once per world update the human update (0x16b427) calls FUN_00251130(world [0x5a71d8], body).
 *     For every strike shape it places the shape at its previous position (shape+0x18) and sweeps
 *     it by (current - previous) against the hurt shapes of every body whose bounds overlap and that
 *     is in line of sight: the pair tests world+0x7c[a * 5 + b] (sphere/sphere FUN_00254d30,
 *     sphere/capsule FUN_00254f80, ...) solve for the first contact in [0, 1] of that sweep, so a hit
 *     is continuous between consecutive updates. A contact calls the attacker's vtable +0x7c
 *     (FUN_0014dbc0: damage FUN_00178d20(move), reaction, AddDamage 0x1867c0); weapon spheres call
 *     the weapon's +0x78 (FUN_002a5250).
 * The viewer hooks those entries (watch stubs, nothing changes in the title's behaviour) and logs:
 *   [combat] strike ...: one line per strike window (part on to off) with its tests, sweep length,
 *      the closest approach to every other character's hurt volumes and the hits;
 *   [combat] move ...: one line per attack move with its startup / active / recovery frames;
 *   [combat] hit / damage ...: the contacts and damage.
 * Frames are 30 Hz frames of game time (one world step is 1/60 s in both modes). */
#ifndef COMBAT_DEBUG_H
#define COMBAT_DEBUG_H
#include <stdint.h>

/* ---- pure helpers (tests/combat_debug_test.c) ---- */

/* Closest distance between segment p0-p1 and segment q0-q1 (either may be a point). */
float cd_seg_seg_dist(const float p0[3], const float p1[3], const float q0[3], const float q1[3]);

/* Frame data of one character's moves, fed once per world update. */
typedef struct {
    int move;               /* current move id, -1 none */
    uint32_t start;         /* step the move started */
    uint32_t first_on;      /* step of its first strike-on, 0 none */
    uint32_t last_off;      /* step of its last strike-off */
    int on;                 /* a strike volume is on now */
    int tests;              /* world updates with a strike volume on */
    int windows;            /* strike windows (on edges) */
    int hits;               /* contacts */
    int valid;              /* start is known */
} CdMove;
typedef struct {
    int move;
    float startup, active, recovery;   /* 30 Hz frames: first on - start, last off - first on, end - last off */
    int tests, windows, hits;
} CdMoveReport;
/* One world update at step with strike volumes on (on != 0) or off. Returns 1 and fills *out when
 * the previous move ended (its id changed) after having had a strike window. */
int cd_move_update(CdMove *m, int move, uint32_t step, int on, CdMoveReport *out);

/* ---- runtime ---- */

int combat_debug_parse_arg(const char *arg, int *enabled);
int combat_debug_install(int enabled);
extern int combat_debug_on;

/* Volumes for the 3D view, captured at the frame's Swap (combat_debug_snapshot). */
enum { CD_VOL_HURT, CD_VOL_STRIKE, CD_VOL_WEAPON, CD_VOL_HIT };
typedef struct {
    float a[3], b[3];  /* sphere: a == b; capsule: its two end points */
    float r;
    uint8_t kind;      /* CD_VOL_* */
} CdVolume;
#define CD_MAX_VOLUMES 512
/* Fills out with the volumes of every character now; returns the count. */
int combat_debug_snapshot(CdVolume *out, int max);

#endif
