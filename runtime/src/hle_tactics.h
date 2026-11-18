/* Smarter enemy group tactics (--enemy-tactics, off by default).
 *
 * How the title fights a group (XBE addresses; brains, handles and gangs as in hle_aidbg.h):
 *   - Every fighter runs a Fight goal (update 0x1d10c0). It re-picks its target every 1000 ms
 *     (0x1bc4f0: best of 0x1ba4e0's CfgSetTargetingPoints score over the enemy gang's list), picks
 *     an attack id 0-44 with 0x1a9f90 (a weighted draw over the brain's 45 weights brain+0x280, the
 *     Lua AI table of its class (Att_Normal, Att_FurySold, ...), or the table brain+0x1f4 points to
 *     when set), then asks the attack gate 0x1abb80. The gate answers 1 (attack now) only while its
 *     target's attackers list (target brain+0x1dc, 4 entries) has a free entry under the limit
 *     (brain+0x136, +0x137 for the player; CfgGang args 2-3: 2 for nearly every gang) and, when
 *     nobody attacks it, its post-hit pause (target brain+0x1d8) is over; its own cooldown is
 *     brain+0x1d4 (answer 2). Any other answer makes it circle the target (0x1ca320: a MoveMelee
 *     action 0x210e40 that keeps a 2-4 m ring and side-steps, 2000 ms) - usually in front of the
 *     player, where it walked in. A listed attacker keeps its entry while it keeps the target, so
 *     the same two attack again and again while the rest wait.
 *   - The title already punishes long blocking: while a fighter's target blocks facing it, its
 *     counter brain+0x134 grows (state handler 0x215713, cap 36) and, above (7 - level) x 6, the
 *     Fight goal (0x1ca200) answers with attack id 0x15 (a guard break). The level is the gang's
 *     byte +0x70 of its CfgGang record (0x7f020, arg 7): 3 for the Warriors, but 0-2 for most enemy
 *     gangs, and with 1 the threshold (36) is never passed.
 *
 * With --enemy-tactics, for fighters outside the player's gang whose target is the player or one
 * of the crew (enemy-vs-enemy fights and the crew keep the title's code path):
 *   - take turns (attack gate answer, the Fight goal's call 0x1d138c, stub shared with --ai-debug):
 *     an attacker that has kept attacking the same target for TACT_TURN_MS (gate answers 1 with no
 *     refusal, rest or TACT_GAP_MS pause between) steps back at its next attack decision when
 *     another enemy is waiting for that target: its attackers-list entry is cleared and it
 *     rests TACT_REST_MS (its own cooldown brain+0x1d4), so the waiting one steps in. The number
 *     of simultaneous attackers and the target's post-hit pause stay the title's.
 *   - surround: enemies that wait for the player keep the title's ring (MoveMelee: distance and
 *     side-steps), but its side-step choice (call 0x211c7b of 0x2117e0: 2 = a step to its right, 6
 *     = to its left, for 10 updates; 8 = none, kept) is steered towards a flanking angle, spread over the player's
 *     sides and back as seen from its attackers (tactics_flank_assign; with no attacker, from its
 *     facing), so they walk around the player while they wait; their ring wait (call 0x1ca5d5,
 *     shared with --smart-allies) is capped at TACT_RING_MS so they ask the gate again sooner.
 *     Enemies on a crew member keep the title's ring;
 *   - punish blocking (call 0x1ca21c of the gang level): the level is raised to TACT_GUARD_LEVEL
 *     (gangs with level 0 keep 0), so a player who blocks facing an enemy for about a second gets
 *     a guard break;
 *   - punish standing still (attack choice calls 0x1d1332 / 0x1d1511): while the player has not
 *     moved TACT_IDLE_M nor swung for TACT_IDLE_MS, enemies attacking the player draw from a copy
 *     of their weights with grab and heavy finisher ids raised (tactics_weights);
 *   - pressure (target score call 0x1bc58a, shared with --smart-allies): a crew member below
 *     TACT_WEAK_PCT % of its health gains TACT_WEAK_BONUS, a Warrior holding or pinning one of the
 *     chooser's gang gains TACT_HELP_BONUS (help a grabbed friend).
 * Reaction times stay the title's (no input is read; every decision is still taken at the title's
 * own decision points and timers). Each site is patched only when its bytes match; without the
 * flag no byte of the title changes. */
#ifndef HLE_TACTICS_H
#define HLE_TACTICS_H
#include <stdint.h>

#define TACT_TURN_MS 3000u
#define TACT_REST_MS 1500u
#define TACT_WAIT_MS 1500u      /* refused within this long = waiting for the target */
#define TACT_GAP_MS 4000u       /* no attack decision for this long ends an attack streak */
#define TACT_FLANK_TOL_RAD 0.45f   /* within ~25 degrees of its flanking angle: the title's side-steps */
#define TACT_RING_MS 1200u
#define TACT_GUARD_LEVEL 3
#define TACT_IDLE_MS 2500u
#define TACT_IDLE_M 0.75f
#define TACT_WEAK_PCT 40
#define TACT_WEAK_BONUS 6
#define TACT_HELP_BONUS 12
#define TACT_MAX_WAIT 16

#define TACT_GATE_CALL 0x001d138cu     /* = AI_GATE_CALL (hle_aidbg.h) */
#define TACT_GUARD_CALL 0x001ca21cu    /* call 0x7f020 (gang level) in the Fight goal's 0x1ca200 */
#define TACT_GUARD_FN 0x0007f020u
#define TACT_CHOICE_CALL1 0x001d1332u  /* call 0x1a9f90 (attack choice) in the Fight goal update */
#define TACT_CHOICE_CALL2 0x001d1511u
#define TACT_CHOICE_FN 0x001a9f90u
#define TACT_STRAFE_CALL 0x00211c7bu   /* call 0x2117e0 (side-step choice) in the MoveMelee update 0x211a90 */
#define TACT_STRAFE_FN 0x002117e0u

#define TACT_ATTACK_IDS 45
#define TACT_ID_GUARD_BREAK 0x15
#define TACT_ID_GRAB 0x16

/* Flanking angles for n waiting enemies around their target: ang[i] is enemy i's angle around the
 * target (radians, 0 = the target's facing, counter-clockwise, any range). The targets spread
 * evenly over the target's sides and back, from pi/2 (left) to 3 pi/2 (right); one waiter goes to
 * its nearer side. They are handed out in angle order, so no one crosses another's path. Writes
 * out[i] in [0, 2 pi); n <= TACT_MAX_WAIT. */
void tactics_flank_assign(const float *ang, int n, float *out);

/* Side-step for an enemy at angle cur around its target (radians, counter-clockwise) that should be
 * at goal: 2 (the MoveMelee's +right step, which turns it counter-clockwise around the target it
 * faces) or 6 (clockwise); 0 within TACT_FLANK_TOL_RAD (keep the title's choice). */
int tactics_strafe_dir(float cur, float goal);

/* Take turns: a slot holder that held its slot held_ms gives it up when others wait. */
int tactics_turn_over(uint32_t held_ms, int waiters);

/* Guard-break level for a gang level k (0 stays 0: that gang never guard-breaks). */
int tactics_guard_level(int k);

/* Attack weights while the player stands still: grab (TACT_ID_GRAB) and guard break x3 (at
 * least 30 when it was allowed at all), the 3-hit finishers (ids 6-9) x2; ids at 0 stay 0, so an
 * enemy never gets a move it does not have. Capped at 255. */
void tactics_weights(const uint8_t in[TACT_ATTACK_IDS], uint8_t out[TACT_ATTACK_IDS]);

/* Player passivity: fed with the game ms clock, the player's position and whether a strike volume
 * is on. Passive once it has neither moved TACT_IDLE_M from where it stood nor swung for
 * TACT_IDLE_MS. */
typedef struct {
    float anchor[3];
    uint32_t active_ms;
    int init;
} TactIdle;
int tactics_idle_update(TactIdle *t, uint32_t now, const float pos[3], int striking);

typedef struct {
    int enemy;              /* the chooser is outside the player's gang */
    int cand_crew;          /* the candidate is a crew member (not the player) */
    int cand_hp, cand_hp_max;
    int cand_holds_chooser_gang;   /* the candidate (player or crew) holds / pins one of the chooser's gang */
} TactScoreIn;
int tactics_score_adjust(const TactScoreIn *in);

/* --enemy-tactics / --no-enemy-tactics: 1 (sets *on) when arg is one of them. */
int tactics_parse_arg(const char *arg, int *on);
/* Patches the sites when on; returns the number patched (0 when off). With observe (--ai-debug)
 * and not on, only the gate and attack choice calls are routed, to print [tactics] statistics
 * (enemy attack decisions on the player and the crew, attack ids, player hits) with the title's
 * behaviour unchanged. */
int hle_tactics_install(int on, int observe);
#endif
