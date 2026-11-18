/* Smarter allies (--smart-allies, off by default).
 *
 * The title's AI picks a fight target in 0x1bc4f0: for each of up to 16 enemies it adds the score
 * of 0x1ba4e0(self, candidate, mask) (distance, the leader's distance and the CfgSetTargetingPoints
 * bonuses) and the goal's own adjustment, and keeps the best. The score ignores how many fighters
 * the candidate already has, while the attack gate 0x1aad10 lets only brain+0x136 of them (+0x137
 * on the player; CfgGang args 2-3, 2 for every story gang) attack at once: the rest of a crew that
 * chose the same enemy circles it (Fight goal 0x1ca320: a MoveTo ring action 0x210e40 held for
 * 2000 ms, 6000 ms in one case) and looks like it stands and watches.
 *
 * With --smart-allies, for the player's crew only (characters in the player's gang record, not the
 * player; enemies and other gangs keep the title's code path):
 *   - target score (call 0x1bc58a -> allies_score): a candidate whose attack slots are all taken
 *     by others loses ALLY_FULL_PENALTY, every other crew member already on it ALLY_SHARE_PENALTY
 *     (spread over the enemies instead of queueing); a candidate fighting the player gains
 *     ALLY_PROTECT_BONUS, one holding (Grabbing goal) or on top of (Mounting) the player or a crew
 *     member ALLY_GRAB_BONUS
 *     (cover each other, help a held Warrior);
 *   - circling (call 0x1ca5d5 -> stub): the ring wait is capped at ALLY_CIRCLE_MS, so a crew
 *     member that could not attack asks the gate again sooner (re-engage faster);
 *   - refusals (the Fight goal's gate call 0x1d138c, stub shared with --ai-debug): see
 *     allies_pick_open below (in War Party 59% of the crew's gate answers were 0 = refused).
 * Each site is patched only when its bytes match; without the flag no byte of the title changes. */
#ifndef HLE_ALLIES_H
#define HLE_ALLIES_H
#include <stdint.h>

#define ALLY_FULL_PENALTY 14
#define ALLY_SHARE_PENALTY 5
#define ALLY_PROTECT_BONUS 10
#define ALLY_GRAB_BONUS 16
#define ALLY_CIRCLE_MS 700u

#define ALLY_SCORE_CALL 0x001bc58au    /* call 0x1ba4e0 in the target choice 0x1bc4f0 */
#define ALLY_SCORE_FN 0x001ba4e0u
#define ALLY_CIRCLE_CALL 0x001ca5d5u   /* call 0x210e40 (ring MoveTo action) in the Fight goal's 0x1ca320 */
#define ALLY_MOVETO_FN 0x00210e40u

typedef struct {
    int crew;              /* the chooser is a crew member (not the player) */
    int attackers, max_attackers;   /* the candidate's attackers list and its limit */
    int self_listed;       /* the chooser is already in that list */
    int crew_on_it;        /* other crew members targeting the candidate */
    int targets_player;    /* the candidate's target is the player */
    int grabbing_friend;   /* the candidate holds or pins the player or a crew member (Grabbing / Mounting goal) */
} AllyScoreIn;

/* Score change for one candidate (0 when the chooser is not crew). */
int allies_score_adjust(const AllyScoreIn *in);
/* Ring wait for a circling character: crew -> min(ms, ALLY_CIRCLE_MS), others unchanged. */
uint32_t allies_circle_value(int crew, uint32_t ms);

/* A refused crew member (attack gate answer 0) switches to the nearest enemy fighting the crew or
 * the player, within ALLY_SWITCH_M, that can take an attacker now (fewer attackers than its limit,
 * and not in its post-hit pause brain+0x1d8 while nobody attacks it), through the title's target
 * setter 0x1adab0; at most once per ALLY_SWITCH_MS per crew member. */
#define ALLY_SWITCH_M 8.0f
#define ALLY_SWITCH_MS 1000u
#define ALLY_TARGET_SET 0x001adab0u
typedef struct {
    int slot;              /* character slot */
    float dist;            /* from the refused crew member */
    int open;              /* can take an attacker now */
} AllyCand;
/* Index into c[] of the nearest open candidate within ALLY_SWITCH_M other than slot current, -1. */
int allies_pick_open(const AllyCand *c, int n, int current);

/* The two call sites above are shared with --enemy-tactics (hle_tactics.h): ai_sites_install patches
 * whichever is not patched yet (bytes checked; returns how many are patched). The score call then
 * runs the title's score, the --smart-allies adjustment (when on), then ai_score_hook (when set);
 * the circle call passes the ring action's arguments (args[0] brain, [1] target character, [2] min,
 * [3] max, [4] 0, [5] ms) to the --smart-allies wait cap (when on), then to ai_circle_hook, which may
 * build another action in the same action slot instead (returns 1) or let the ring action be built
 * with the arguments it changed (returns 0). */
int ai_sites_install(void);
extern int (*ai_score_hook)(uint32_t self, uint32_t cand, int score);
extern int (*ai_circle_hook)(uint32_t action, uint32_t *args);

/* --smart-allies / --no-smart-allies: 1 (sets *on) when arg is one of them. */
int allies_parse_arg(const char *arg, int *on);
/* Patches the sites when on; returns the number patched (0 when off). */
int hle_allies_install(int on);
#endif
