/* Per-character speed caps for mods: the Lua function ModSpeed(character, scale).
 *
 * How the title moves a character (native code, not reachable from its Lua): every human update
 * calls its locomotion function [human+0x190]; on foot that is 0x16b600, which picks a target speed
 * from the brain's desired speed (brain+0x100: the stick for the player, the move goals for the AI)
 * and the gait speeds, keeps it on the stack ([esp+0x10]) and then accelerates the current speed
 * toward it (0x16b872 onward: current speed from 0x2b17a0 / 0x51750, step limited by the time step)
 * and turns it into the velocity. Every path through the target choice reaches 0x16b872
 * (`mov ecx, esi; call 0x2b17a0`, esi = the human).
 *
 * The player moves another way: his stick's speeds come from the gait getters 0x13fa40-0x13fc20,
 * which multiply the locomotion block's speeds ([human+0xb0]+0x164..0x17c) by the title's own
 * per-character multiplier human+0x35c (1.0 at creation, 0x14bc0f; the title lowers it to 0.65 after
 * one recovery state, 0x156870, and raises it by x1.002 per update up to 1.0, 0x16b02c-0x16b069).
 * Measured: capping +0x35c slows the player and not an AI fighter running
 * to his target; scaling the target at 0x16b872 slows the AI and not the player. ModSpeed does both.
 *
 * ModSpeed(character, scale): scale 0.3-1 (clamped; nil = only read) multiplies that character's
 * target speed at 0x16b872 and keeps his +0x35c at or under the scale (the ramp at 0x16b02c runs
 * the retail rise, then the cap), so he walks, runs and sprints at that share of his speed and
 * accelerates as the title does. 1 removes the cap: the target at once, +0x35c rising back at the
 * retail rate per 30 Hz frame (the same rate per second with --sim60). The first cap below 1 writes
 * both hooks (or neither, when either site holds other bytes). A cap belongs to the handle: a new
 * character in the same human slot (another handle serial) starts without one. Returns the character's cap after the call (1 = none), or nil when
 * the handle is not a live human.
 *
 * Why the player's multiplier is not a limp: the title animates a
 * character on foot from his actual speed (0x17fcc0 -> 0x17f640 -> 0x17f410: the speed as a blend
 * position between his walk, run, jog and sprint speeds, each times +0x35c), so the player under a
 * cap plays his run a little slower and shorter (measured x0.85: 15 % slower, stride period 5.5 %
 * longer), which reads as slow motion. Scaling his velocity after the locomotion function instead
 * feeds back (the next update accelerates from the scaled speed: 4.5 m/s, not 6.6) and changes the
 * blend just the same.
 *
 * ModSprint(character, allowed): allowed 0 stops a player-controlled character from sprinting:
 * the player's locomotion function 0x1655d0 asks 0x14b1f0 whether he sprints (the sprint control
 * held, stamina left) at 0x1657ff (`call 0x14b1f0`, ecx = the human); the hook answers "no" for a
 * blocked character, so the title picks his run (the next test, 0x14b070) and plays it at its own
 * rate. 1 allows it again; nil only reads. The block belongs to the handle like a cap. Returns 1
 * when he may sprint, 0 when blocked, nil when the handle is not a live human. The first block writes
 * the hook (or nothing when the site holds other bytes; then he keeps his sprint). AI characters do
 * not use 0x1655d0: the block does not change them.
 *
 * ModComboSlowMo(allowed): allowed 0 removes the slow motion the title plays during some fight
 * combos (X+A, A, A and others, most of all with Rage on) in single player. A fight animation's
 * event 0x2e starts it and 0x2f ends it (0x13d70, the human's animation events): for a player-
 * controlled human (+0x180, his player number, not -1) and only when one player plays ([[0x5b9f78]
 * + 0x220] == 1), the title calls 0x335ec0 (ecx = the camera owner, his player number pushed), which
 * marks that player's slow motion and sets the world time step to [0x460e98] (the follow camera's
 * slow-motion share, CfgFollowCamera's last argument, 0.2) times 1/30 s; with two players the test
 * fails and nothing slows, so co-op never shows it. The hook at 0x146ae (`call 0x335ec0`) skips the
 * call while removed (the pushed number is popped as 0x335ec0's ret 4 would), which is what co-op
 * does; the end event (0x335ef0) then finds no slow motion marked and sets the normal step, as it
 * always does. 1 lets the title start it again; nil only reads. Returns 1 when it may start, 0 when
 * removed, nil when the site holds other bytes (nothing written). The first call with 0 or 1 writes
 * the hook; each start is logged while it is in place (`[huspeed] combo slow motion (player N):
 * skipped` / `: as the title`). Slow motion set by the level scripts (HuSetSlowMo) is not changed. */
#ifndef HLE_HUSPEED_H
#define HLE_HUSPEED_H
#include <stdint.h>

#define HUSPEED_SITE 0x0016b872u      /* mov ecx, esi; call 0x2b17a0: the target speed is chosen */
#define HUSPEED_SPEED_FN 0x002b17a0u  /* the call at the site (the human's velocity) */
#define HUSPEED_TARGET_OFS 0x10u      /* the target speed: [esp+0x10] at the site (float) */
#define HUSPEED_RAMP_SITE 0x0016b02cu /* movss xmm0, [0x40ffc8]: the retail ramp of human+0x35c */
#define HUSPEED_RAMP_END 0x0016b06au  /* lea ecx, [esp+0x48]: the first instruction after it */
#define HUSPEED_MULT 0x35cu           /* human: the title's speed multiplier (float) */
#define HUSPEED_RAMP 1.002f           /* its retail rise per 30 Hz update (0x42e938) */
#define HUSPEED_HANDLES 0x005a7570u   /* handle table: {object, serial} per slot (handle >> 16) */
#define HUSPEED_POOL 0x004ff878u      /* humans: HUSPEED_SLOTS x HUSPEED_STRIDE */
#define HUSPEED_STRIDE 0x668u
#define HUSPEED_SLOTS 60
#define HUSPEED_MIN 0.3
#define HUSPEED_SPRINT_SITE 0x001657ffu /* call 0x14b1f0 in the player's locomotion (ecx = the human) */
#define HUSPEED_SPRINT_FN 0x0014b1f0u   /* does the player-controlled human sprint now (al) */
#define HUSPEED_COMBO_SITE 0x000146aeu  /* call 0x335ec0: a fight animation starts single-player slow motion */
#define HUSPEED_COMBO_FN 0x00335ec0u    /* thiscall (player number): marks it, world step = [0x460e98] / 30 */

/* The title's ramp of human+0x35c, then the cap (the ramp hook body; tests call it). */
void huspeed_update(uint32_t human);
/* 1 with the 60 Hz world (--sim60; set by main). */
extern int huspeed_sim60;
/* The target speed one human moves toward, after his cap (the hook body; tests call it). */
float huspeed_target(uint32_t human, float target);
/* Sets (scale >= 0) or only reads (scale < 0) the cap of a handle. Returns 0 and the cap in *cap,
 * or -1 when the handle is not a live human. Writes the hook on the first cap below 1. */
int huspeed_set(uint32_t handle, double scale, float *cap);
/* 1 once the hook is written, -1 when the site held other bytes (nothing written), 0 before. */
int huspeed_patched(void);
/* Caps in use (tests). */
int huspeed_active(void);

/* 1 when the human's sprint is blocked (the sprint hook body; tests call it). */
int huspeed_sprint_blocked(uint32_t human);
/* Blocks (allowed 0), allows (1) or only reads (-1) a handle's sprint. Returns 0 and in *may 1 (he may
 * sprint) or 0, or -1 when the handle is not a live human. Writes the hook on the first block. */
int huspeed_sprint_set(uint32_t handle, int allowed, int *may);
/* 1 once the sprint hook is written, -1 when its site held other bytes, 0 before. */
int huspeed_sprint_patched(void);

/* Removes (0), allows (1) or only reads (-1) the combo slow motion. Returns 1 (it may start), 0
 * (removed), or -1 when the site held other bytes (nothing written). 0 or 1 writes the hook. */
int huspeed_combo_set(int allowed);
/* 1 once the combo hook is written, -1 when its site held other bytes, 0 before. */
int huspeed_combo_patched(void);
/* Starts seen by the hook, and how many it skipped (tests, the log). */
void huspeed_combo_counts(int *started, int *skipped);

/* ModSpeed(character, scale) for the title's Lua (cdecl C function, registered by hle_mods with
 * the title's lua_pushnumber / lua_pushnil). */
void huspeed_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L));
int __attribute__((cdecl)) hle_huspeed_lua(uint32_t L);
/* ModSprint(character, allowed) for the title's Lua. */
int __attribute__((cdecl)) hle_husprint_lua(uint32_t L);
/* ModComboSlowMo(allowed) for the title's Lua. */
int __attribute__((cdecl)) hle_combo_slowmo_lua(uint32_t L);
#endif
