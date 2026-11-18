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
 * the handle is not a live human. */
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

/* ModSpeed(character, scale) for the title's Lua (cdecl C function, registered by hle_mods with
 * the title's lua_pushnumber / lua_pushnil). */
void huspeed_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L));
int __attribute__((cdecl)) hle_huspeed_lua(uint32_t L);
#endif
