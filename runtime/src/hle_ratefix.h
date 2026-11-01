/* Rate fixes for the 60 Hz presentation (on by default, --no-rate-fixes turns them off).
 *
 * The title asked for 30 fps (D3DPRESENT_INTERVAL_TWO); the port presents at 60 Hz. The world
 * simulation runs on the real clock, but the HUD manager (FUN_000da2a0, object 0x4d6298) calls
 * every widget's update (vtable slot 5, +0x14) once per rendered frame, and some widgets add a
 * fixed amount per call with no frame time; at 60 Hz those run faster than on the Xbox. The
 * graffiti minigame (the player's tag state) counts world updates and clamps its step to 30 ms,
 * a full update at 30 Hz.
 *
 * The graffiti fixes are world-side: the tag update FUN_001939d0 is reached only from the world
 * update (0x16d8d0 -> 0x17c980 -> 0x179b10 -> 0x193ea0), which runs on every second 1/60 s step
 * (30 Hz, as on the Xbox) unless --sim60 is on. They apply only with --sim60 (sim60_only); with
 * the 30 Hz world the title's bytes are already right. Lockpick and arrow are HUD-side (per
 * presented frame) and apply in both modes.
 *
 * Each fix replaces one or two operands (a pointer to a read-only constant, or an immediate) in
 * title code with the value for 60 calls per second, after checking the instruction bytes
 * around them. A fix whose bytes differ is skipped as a whole and reported; nothing else is
 * touched.
 *
 * Spray time (comfort, not a rate fix): the graffiti spray can lasts a real-time number of
 * milliseconds by difficulty (13000/11000/9500 in the .data rows at 0x45d57c). --spray-time P
 * scales them to P percent (default 110, 100 = the Xbox values). It rides on the same pass:
 * applied with the rate fixes, off with --no-rate-fixes. */
#ifndef HLE_RATEFIX_H
#define HLE_RATEFIX_H
#include <stddef.h>
#include <stdint.h>

#define RATEFIX_SIG_MAX 36
#define RATEFIX_WRITES_MAX 3

typedef struct {
    uint32_t va;                     /* operand address, inside the checked bytes */
    uint8_t width;                   /* 1 (imm8) or 4 (imm32 / address); 0 ends the list */
    uint32_t value;                  /* new value, little-endian */
} RateFixWrite;

typedef struct {
    const char *name;
    uint32_t sig_va;                 /* first byte of the checked instruction bytes */
    uint8_t sig[RATEFIX_SIG_MAX];
    uint8_t sig_len;
    RateFixWrite writes[RATEFIX_WRITES_MAX];   /* applied together or not at all */
    uint8_t sim60_only;              /* world-side: only right with the 60 Hz world (--sim60) */
} RateFix;

extern const RateFix ratefix_table[];
extern const int ratefix_count;

/* Applies the enabled fixes of t[0..n) to memory where guest address va is at host address
 * va + bias (bias 0: the identity-mapped guest). Writes "[ratefix] ..." (one line, no newline)
 * into report. Returns the number of fixes applied. */
int ratefix_apply(const RateFix *t, int n, int enabled, intptr_t bias, char *report, size_t cap);

/* 1 when arg is a rate-fix option (sets *enabled), 0 otherwise. */
int ratefix_parse_arg(const char *arg, int *enabled);

#define RATEFIX_SPRAY_VA 0x0045d57cu   /* first graffiti difficulty row: {can ms, penalty ms, speed} */
#define RATEFIX_SPRAY_DEFAULT 110
#define RATEFIX_SPRAY_MIN 100
#define RATEFIX_SPRAY_MAX 200

/* 1 when s is an integer percent in RATEFIX_SPRAY_MIN..MAX (sets *percent), 0 otherwise. */
int ratefix_parse_spray(const char *s, int *percent);

/* Builds the spray-time fix for percent (the three can durations scaled, rounded to the ms,
 * checked against the shipped rows). Returns 0 and builds nothing for 100. */
int ratefix_spray_fix(int percent, RateFix *out);

/* Copies ratefix_table into out (the sim60_only entries only when sim60 is set), plus the
 * spray-time fix when percent != 100. Returns the count. */
int ratefix_build(int spray_percent, int sim60, RateFix *out, int cap);

/* The whole pass at bias (see ratefix_apply): builds, applies and writes the [ratefix] line,
 * naming the sim60_only fixes left out with the 30 Hz world. Returns the number applied. */
int ratefix_install_at(int enabled, int spray_percent, int sim60, intptr_t bias, char *report, size_t cap);

/* Applies the rate fixes and spray time to the loaded title and prints the [ratefix] line.
 * sim60: the 60 Hz world is active (hle_sim60_install's result). */
int hle_ratefix_install(int enabled, int spray_percent, int sim60);
#endif
