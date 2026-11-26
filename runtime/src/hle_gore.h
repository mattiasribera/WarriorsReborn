/* Blood settings for mods: the Lua function ModBlood(option, value).
 *
 * The title's blood (all native code, nothing of it is in its Lua scripts):
 *   - Hits start the particle system "sub_blood_effect" (0x2c2710; class table 0x460394) from
 *     animation events (0x142a50 runs the clip event's sub_* name). Its first argument is a drop
 *     count (1 for every punch seen in a street fight): count > 1 makes a "blood_mist" and count + 1 drop tasks that fly and, on landing, leave
 *     "blood_splat_ground" tasks (the pools on the ground). Blood only starts within 25 m of the
 *     camera, while more than 512 particle tasks are free, and when the no-blood word
 *     [[0x5b9f78]+0x42a] is 0. "sub_blood_gush" (0x16fea0) is a stream attached to a character.
 *   - A ground pool (class 0x45f64c: start 0x2bf340, phase end 0x2bf490) grows for 10/60 s, then
 *     holds for 180/60 s (the duration set at phase 3, `push 0xb4` at 0x2bf4c9) and fades for as
 *     long (phase 4 keeps the duration). At every phase end it dies at once when 512 or fewer
 *     particle tasks are free. A phase lasts +0x58 seconds (float; +0x62 = the same in 1/60 s,
 *     u16), set by 0x2b1080.
 *   - Characters wear one of three blood textures "charblood_d1..3_tex" (ResourceMgr+0x58..0x60,
 *     loaded at 0xa702b) over their skin when their health is under 90 %: d1 from 60 to 90 %, d2
 *     from 30 to 60 %, d3 under 30 % (the character draw 0x8fc70: index (2 - min(hp / 30, 2)) % 3,
 *     health percent from 0x140540 = health / max health * 100 of the human). Healing (e.g. flash)
 *     takes the blood away.
 *
 * ModBlood options (each patch is written the first time its option leaves the retail value and
 * stays; with the retail values the hooks behave as the title):
 *   "drops"     1-4: multiplies a blood effect's drop count (0x2c2751). Every punch seen in a street
 *               fight starts it with 1 (one drop, no mist); 2 or more takes the mist + drops path.
 *   "pool_hold" 0 or 10-1000 s: how long a ground pool stays at full size after the retail 3 s
 *               (0 = retail: fade at once). At the end of the retail hold (0x2bf4ea, the phase-4
 *               branch) the pool gets a new phase of this length at the size and colour it has,
 *               without advancing its state; when it ends (or the pool is pushed out), the retail
 *               3 s fade follows. A pool is not held while 512 or fewer particle tasks are free
 *               (it fades, as the title would end it).
 *   "pool_max"  16-512 (default 128): at most this many held pools; the oldest starts its fade
 *               when a new one would pass it.
 *   "char_gain" 1-4: the character blood stage follows 100 - (100 - health %) * gain, so blood
 *               shows after a smaller loss and reaches the heavier textures sooner.
 *   "char_keep" 0/1: a character's blood stage follows the lowest health it had since it was
 *               created (or since the level started): healing does not wash it off. A character is
 *               its human object and the handle (slot and serial, table 0x5a7570) that names it: the
 *               title reuses a human's memory and its body record (+0xb0) for the next character of
 *               that pool slot (cutscene actors too), which starts clean.
 * While the cutscene letterbox bars show (cinema_bars_listener, the frame drawn before) the blood
 * stage is the retail one: no gain, nothing kept or recorded.
 * The per-level records (held pools, lowest health) start over when a call comes from another Lua
 * state (each level has its own). */
#ifndef HLE_GORE_H
#define HLE_GORE_H
#include <stdint.h>

#define GORE_FADE_SITE 0x002bf4eau   /* mov eax, [esi+0xa4]: first instruction of the phase-4 branch (fade start) */
#define GORE_CONTINUE_EXIT 0x002bf520u /* pop edi; xor al, al; ...: the phase-end handler's "go on" exit */
#define GORE_ROOM_FN 0x002b4b40u     /* more than 512 free particle tasks (blood may start / stay) */
#define GORE_RETAIL_HOLD 180         /* the retail hold, 1/60 s (+0x62 at the end of phase 4) */
#define GORE_DROPS_SITE 0x002c2751u  /* call 0x2b5f10: sub_blood_effect pops its drop count */
#define GORE_CHAR_SITE 0x0008fd9cu   /* call 0x140540: health percent for the blood stage */
#define GORE_POP_INT_FN 0x002b5f10u
#define GORE_HEALTH_PCT_FN 0x00140540u

#define GORE_HOLD_MIN 10.0
#define GORE_HOLD_MAX 1000.0
#define GORE_POOLS_MAX 512
#define GORE_CHARS 64
#define GORE_HANDLES 0x005a7570u     /* handle table: {object, serial} per slot; 0..0x3b are characters */
#define GORE_HANDLE_CHARS 60

typedef struct {
    double drops, hold, gain;
    int pool_max, keep;
} GoreConfig;

/* Applies one option ("drops", "pool_hold", "pool_max", "char_gain", "char_keep") with the value
 * clamped to its range. Returns 0, or -1 for an unknown option or a value that is not a number
 * (nothing changed). Writes the patches the option needs (once). */
int gore_set(const char *option, const char *value);
/* Current settings (tests). */
GoreConfig gore_config(void);
/* Starts the per-level records over (held pools, lowest health per character). */
void gore_reset_level(void);

/* The hook bodies, game-independent (tests call them on planted objects). */
int gore_drops(int n);                       /* scaled drop count */
int gore_pool_end(uint32_t task);            /* end of a pool's retail hold: 1 = hold on, 0 = fade */
float gore_char_pct(uint32_t human, float pct);   /* health percent shown by the blood stage */
/* 1 while the title's in-engine cutscene letterbox bars show (the renderer sets it per frame from the
 * last frame it drew): the blood stage is the retail one and nothing is recorded. */
void gore_set_cutscene(int on);

/* ModBlood(option, value) for the title's Lua (cdecl C function, registered by hle_mods). */
int __attribute__((cdecl)) hle_gore_lua(uint32_t L);
/* Logs every blood effect's drop count (--mods-trace). */
extern int hle_gore_trace;

#endif
