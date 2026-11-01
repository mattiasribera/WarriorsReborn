/* --sim60 (experimental, off by default): a 60 Hz world instead of 30 Hz.
 *
 * The step loop FUN_002b2430 runs fixed 1/60 s steps in pairs (only when two steps are due), and
 * each step calls FUN_0016d8d0, which runs the world update (AI, characters, cameras) only on
 * every second call. The world therefore advances 30 times per second with a 1/30 s time step
 * (the float at 0x45cc38, set from the 1/30 constant at 0x410440 and scaled for slow motion).
 *
 * The patches make the loop run single steps, run the world update on every step and halve the
 * world time step everywhere it is set (1/30 -> 1/60, and the slow-motion values with it), so the
 * game advances at the same speed in twice as many updates. Systems that count world updates
 * instead of using the time step are adjusted where found (AI think round-robin, litter physics,
 * character fade-out, button-tap run, the AI brain age divisors, the turn-rate caps), and the 30 Hz
 * scene task (cinematic camera, fixed scenes) runs on every step. The menus' fixed-step clock
 * (1/30 s per frame) is set to 1/60 s per frame so they run one step per frame: two steps per frame would poll the pad twice and lose presses.
 *
 * The patch set is applied as a whole: every site's original bytes are checked first, and on
 * any mismatch nothing is written (the world stays at 30 Hz) and the mismatching sites are
 * reported. */
#ifndef HLE_SIM60_H
#define HLE_SIM60_H
#include <stddef.h>
#include <stdint.h>

#define SIM60_BYTES_MAX 40

typedef struct {
    const char *name;
    uint32_t va;                       /* first byte */
    uint8_t len;                       /* 1..SIM60_BYTES_MAX */
    uint8_t orig[SIM60_BYTES_MAX];     /* expected bytes (the signature) */
    uint8_t repl[SIM60_BYTES_MAX];     /* bytes written */
} Sim60Patch;

extern const Sim60Patch sim60_table[];
extern const int sim60_count;

/* Applies t[0..n) as a whole to memory where guest address va is at host address va + bias
 * (bias 0: the identity-mapped guest), only when enabled and every entry is well formed and
 * matches. Writes "[sim60] ..." (one line, no newline) into report. Returns the number of
 * patches written (n or 0). */
int sim60_apply(const Sim60Patch *t, int n, int enabled, intptr_t bias, char *report, size_t cap);

/* 1 when arg is a sim60 option (--sim60 sets *enabled, --no-sim60 clears it), 0 otherwise. */
int sim60_parse_arg(const char *arg, int *enabled);

/* Applies sim60_table to the loaded title and prints the [sim60] line. Returns 1 when the
 * 60 Hz world is active (also kept in hle_sim60_active). */
int hle_sim60_install(int enabled);
extern int hle_sim60_active;
#endif
