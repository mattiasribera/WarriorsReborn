/* --rate-probe (diagnostic, off by default): snapshots of guest memory at fixed points of game
 * time, for a differential "rate diff" between the default 30 Hz world and --sim60.
 *
 * A system that counts world updates instead of using the world time step runs twice as fast
 * with the 60 Hz world. Running the same scripted input in both modes and comparing, per field,
 * how much it changes per game second finds such systems (tools/ratediff/). This module is
 * game-independent: it parses the options, decides when a snapshot is due from a millisecond
 * game clock, and writes the snapshots; guest memory is read through a callback.
 *
 * File format (little endian, all fields u32):
 *   header:   'RPRB' (0x42525052), version 1, interval_ms, flags (bit 0: --sim60 active), nranges,
 *             then per range {addr, size, deref, off}
 *   snapshot: 'SNAP' (0x50414e53), frame (Swap number), game_ms, nranges,
 *             then per range {base, kind, count} and a payload:
 *             kind 0 (unreadable): none; kind 1 (full): size bytes;
 *             kind 2 (changed dwords since the previous snapshot): count x {offset, value}.
 * A range is written in full the first time, after an unreadable sample, when its base moved
 * (a range behind a pointer) and whenever the change list would not be smaller. */
#ifndef RATEPROBE_H
#define RATEPROBE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define RATEPROBE_MAX_RANGES 48
#define RATEPROBE_DEFAULT_INTERVAL_MS 100u
#define RATEPROBE_MAGIC 0x42525052u   /* "RPRB" */
#define RATEPROBE_SNAP 0x50414e53u    /* "SNAP" */
#define RATEPROBE_VERSION 1u
#define RATEPROBE_FLAG_SIM60 1u

enum { RATEPROBE_NONE = 0, RATEPROBE_FULL = 1, RATEPROBE_DELTA = 2 };

/* A guest range: addr..addr+size, or with deref set [addr]+off..+size (behind a pointer).
 * addr, off and size are multiples of 4. */
typedef struct {
    uint32_t addr, size, deref, off;
} RateProbeRange;

/* "FILE[:INTERVAL_MS]": the interval is taken only from a last ':' followed by digits (so a
 * Windows path such as C:\x.rp stays a path); 1..60000 ms. Returns 0, or -1 when malformed. */
int rateprobe_parse_spec(const char *spec, char *path, size_t cap, uint32_t *interval_ms);

/* "ADDR:SIZE" or "@PTR[+OFF]:SIZE" (hex with 0x or decimal). Returns 0, or -1 when malformed,
 * unaligned, empty or wrapping past 4 GB. */
int rateprobe_parse_range(const char *spec, RateProbeRange *r);

/* Snapshot schedule on a millisecond game clock: due at the first reading, then each time the
 * clock reaches the next multiple of the interval (missed boundaries are skipped, not caught
 * up), and at once when the clock went backwards (a reset). */
typedef struct {
    uint32_t interval_ms, next_ms, last_ms;
    int started;
} RateProbeClock;
int rateprobe_due(RateProbeClock *c, uint32_t game_ms);

/* Returns a host pointer to len readable guest bytes at addr, or NULL. */
typedef const void *(*RateProbeRead)(uint32_t addr, uint32_t len, void *ctx);

typedef struct RateProbe RateProbe;
/* Writes the header to f (owned by the probe from now on). NULL on bad arguments or no memory. */
RateProbe *rateprobe_open(FILE *f, uint32_t interval_ms, uint32_t flags, const RateProbeRange *r, int n);
/* Called once per presented frame with the game clock; writes a snapshot when one is due.
 * Returns 1 when a snapshot was written, 0 otherwise, -1 on a write error. */
int rateprobe_frame(RateProbe *p, uint32_t frame, uint32_t game_ms, RateProbeRead rd, void *ctx);
void rateprobe_counts(const RateProbe *p, uint32_t *snapshots, uint64_t *bytes);
void rateprobe_close(RateProbe *p);   /* flushes and closes the file */

/* --poke ADDR:HEX[/EXPECT] (diagnostic): bytes written into guest memory once the XBE is loaded,
 * before the title starts; with /EXPECT only when the original bytes match (same length). Used
 * by tools/cutscene_sweep/ to start a chosen level and chapter from the profile load. */
#define RATEPROBE_POKE_MAX 32
#define RATEPROBE_MAX_POKES 64
typedef struct {
    uint32_t addr, len;
    uint8_t bytes[RATEPROBE_POKE_MAX], expect[RATEPROBE_POKE_MAX];
    int has_expect;
} RateProbePoke;
/* Returns 0, or -1 when malformed (odd or non-hex digits, empty, longer than RATEPROBE_POKE_MAX,
 * an expectation of another length, wrapping past 4 GB). */
int rateprobe_parse_poke(const char *spec, RateProbePoke *k);
/* Writes the poke at host pointer mem (the guest address mapped). Returns 0, or -1 (nothing
 * written) when the original bytes differ from the expectation. */
int rateprobe_apply_poke(const RateProbePoke *k, uint8_t *mem);

/* 1 when argv[*i] is a rate-probe option (consuming its value: *i advances), 0 when it is not,
 * -1 when it is one but its value is missing or malformed. */
typedef struct {
    char path[512];
    uint32_t interval_ms;
    RateProbeRange ranges[RATEPROBE_MAX_RANGES];
    int nranges;
    RateProbePoke pokes[RATEPROBE_MAX_POKES];   /* --poke, also accepted without --rate-probe */
    int npokes;
} RateProbeOptions;
int rateprobe_parse_arg(int argc, char **argv, int *i, RateProbeOptions *o);

/* The probe of this run (set by main when --rate-probe was given), called from Swap. */
extern RateProbe *rateprobe_active;
#endif
