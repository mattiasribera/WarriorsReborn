/* Ally AI diagnostics (--ai-debug).
 *
 * The title keeps one AI brain per character slot: 60 records of 0x2d0 bytes at 0x599080
 * (brain i belongs to character pool slot i, 0x4ff878 + 0x668 x i):
 *   +0x00 character pointer, +0x14 its state handler (called by the brain tick 0x1a8870), +0x20 top
 *   of the goal stack (int8, -1 empty) over +0x34 (goal objects; push 0x1ab740), each a class whose
 *   vtable is followed in .rdata by its name (Fight, Spectate, FollowAndAttack, ...), +0x22 queued actions (int8) in a ring
 *   of 8 at +0x5c starting at +0x23 (action objects are named the same way: Attack, MoveMelee,
 *   Block, ...), +0x28 age in world updates, +0x1f8 the gang record (gang index at +0x30, current
 *   tactic at +0x40, whose vtable +0x14 returns the tactic type), +0x1dc the characters attacking
 *   it (4 pointers; the attack gate 0x1aad10 admits a new one while fewer than +0x136, or +0x137
 *   for the player, are listed).
 * Character +0x180 is the player index (int8, -1 for AI). Positions: 32-byte records at 0x4fefe8
 * (x, y, z) by slot; health: int16 at +0x144 of the 0x180-byte body records at 0x5178d8 by slot.
 * Object handles: high word = slot in the table at 0x5a7570 (8 bytes: pointer, then a 16-bit
 * serial that must equal the handle's low word).
 *
 * The player's crew are the characters in the player's gang record. The target is the brain's +0x110
 * handle (the Fight goal's, 0x1d10c6), else the first dword of the goal (after its vtable) that is a
 * live character handle other than its owner.
 *
 * --ai-debug prints, from the title's Swap on the guest thread, an [ai] line whenever a crew member
 * or a character fighting the crew changes goal, action or target, and an [ai] summary per 10 s
 * of game time: per crew member the share of time spent with an enemy within AI_NEAR_M metres while
 * not engaging (no attack-type goal or action), the enemy health removed, the hits taken. Nothing
 * of the title is changed. */
#ifndef HLE_AIDBG_H
#define HLE_AIDBG_H
#include <stdint.h>

#define AI_SLOTS 60
#define AI_BRAINS 0x00599080u
#define AI_BRAIN_STRIDE 0x2d0u
#define AI_CHARS 0x004ff878u
#define AI_CHAR_STRIDE 0x668u
#define AI_POS 0x004fefe8u
#define AI_POS_STRIDE 0x20u
#define AI_BODIES 0x005178d8u
#define AI_BODY_STRIDE 0x180u
#define AI_BODY_HEALTH 0x144u
#define AI_HANDLES 0x005a7570u
#define AI_NEAR_M 6.0f
#define AI_NAME_MAX 24

/* Returns a host pointer to len readable guest bytes at addr, or NULL. */
typedef const void *(*AiRead)(uint32_t addr, uint32_t len, void *ctx);

typedef struct {
    int used;
    int player;                 /* -1 for AI */
    uint32_t gang;              /* gang record pointer (+0x1f8) */
    int ally;                   /* in the player's gang record, not the player */
    char goal[AI_NAME_MAX];     /* "" when none */
    char action[AI_NAME_MAX];   /* front of the action ring, "" when the ring is empty */
    int target;                 /* slot of the goal's target, -1 */
    float pos[3];
    int hp;
    int attackers;              /* characters in its attackers list (brain +0x1dc, 4 entries) */
    int max_attackers;          /* brain +0x136 (+0x137 when the player is the target) */
    int listed;                 /* it is in its target's attackers list (holds an attack slot) */
} AiChar;

/* Class name of an object from its vtable (the ASCII name after the vtable's code pointers in the
 * XBE image); "" when the vtable is unreadable or has no name. */
void ai_class_name(AiRead rd, void *ctx, uint32_t vtable, char *out, int cap);
/* Slot of a character handle (alive in the handle table), -1 otherwise. */
int ai_handle_slot(AiRead rd, void *ctx, uint32_t handle);
/* Reads all 60 brains; returns the player's slot or -1. */
int ai_snapshot(AiRead rd, void *ctx, AiChar out[AI_SLOTS]);

/* Engaging an enemy: holding one of its attack slots, an Attack/Block/Dive action or a grapple goal.
 * MoveMelee alone is not: the Fight goal also circles with it while it waits for a slot. */
int ai_is_engaging(const AiChar *c);
/* A non-crew character fighting the crew: it targets a crew member or the player. */
int ai_is_crew_enemy(const AiChar *all, int i, int player);

typedef struct {
    double near_ms[AI_SLOTS];       /* time with an enemy within AI_NEAR_M */
    double idle_near_ms[AI_SLOTS];  /* ... while not engaging */
    double engaged_ms[AI_SLOTS];
    int dealt[AI_SLOTS];            /* health removed from the enemy it was targeting */
    int hits_taken[AI_SLOTS];
    int pile_samples, pile_sum, pile_max;   /* crew members per targeted enemy */
    int enemy_damage;               /* all health removed from crew enemies */
    int last_hp[AI_SLOTS];
    int last_target[AI_SLOTS];
    int have_last;
} AiStats;
/* Adds one sample covering dt_ms of game time. */
void ai_stats_add(AiStats *s, const AiChar *all, int player, double dt_ms);

/* Runtime glue: --ai-debug. With it, the Fight goal's call of the attack gate (0x1d138c ->
 * 0x1abb80, thiscall on the brain: 1 = attack now, 0 = no slot, 2 = own cooldown brain+0x1d4,
 * 3 = cannot attack, 4-8 = position / gang / crowd rules) is routed through a counter, and the
 * summaries add the crew's answers per code. This is the one byte change --ai-debug makes. */
#define AI_GATE_CALL 0x001d138cu
#define AI_GATE_FN 0x001abb80u
#define AI_GATE_CODES 10
extern int hle_aidbg_active;
/* Called after each gate answer when set (guest thread): --smart-allies re-targets on a refusal. */
extern void (*ai_gate_after)(uint32_t brain, uint32_t code);
/* When set, replaces each answer before ai_gate_after and the Fight goal see it (--enemy-tactics). */
extern uint32_t (*ai_gate_filter)(uint32_t brain, uint32_t code);
/* Routes the gate call through the counter / ai_gate_after (once; for --ai-debug or --smart-allies). */
int hle_aidbg_install(void);
void hle_aidbg_swap(uint32_t swap, uint32_t game_ms, int last);
#endif
