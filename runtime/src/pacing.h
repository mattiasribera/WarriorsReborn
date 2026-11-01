/* Frame pacing helpers, game-independent (no guest memory, no host clock reads).
 *
 * PacingStats: the cadence of a fixed-step simulation as seen on screen. Fed once per presented
 * frame with a step counter whose even values mark a world update, it counts world updates per
 * presented frame (0, 1, 2+) and how many presented frames each world state stays on screen
 * (1, 2, 3, 4+). With a 30 Hz world on a 60 Hz display every state should be held 2 frames. */
#ifndef PACING_H
#define PACING_H
#include <stddef.h>
#include <stdint.h>

#define PACING_MAX_STEP_JUMP 1000u   /* a larger counter change is taken as a reset, not as steps */

typedef struct {
    uint64_t updates[3];   /* presented frames with 0, 1, 2+ world updates */
    uint64_t holds[4];     /* world states held 1, 2, 3, 4+ presented frames */
    uint64_t frames;       /* frames counted (counter readable and continuous) */
    uint32_t last;         /* counter at the previous frame */
    uint32_t run;          /* frames the current world state has been on screen */
    int have_last, have_state;
} PacingStats;

/* One presented frame: counter = the step counter after the frame's steps. */
void pacing_stats_frame(PacingStats *s, uint32_t counter);
/* The counter could not be read this frame: the sequence restarts at the next readable value. */
void pacing_stats_break(PacingStats *s);
/* Share of recorded world states held exactly 2 frames (0..1; 0 without any). */
double pacing_stats_hold2(const PacingStats *s);
/* Difference of two snapshots (a window): out = now - before, counts only. */
void pacing_stats_diff(const PacingStats *now, const PacingStats *before, PacingStats *out);
/* "updates/frame 0:a 1:b 2+:c | holds 1:d 2:e 3:f 4+:g (2 frames x.x%)" */
void pacing_stats_format(const PacingStats *s, char *buf, size_t len);

/* PacingClock: a frame-locked view of time for the guest. A title that steps its simulation
 * from a clock it reads once per frame sees that clock advance by exactly one display period per
 * presented frame while the host keeps up, so its fixed steps fall on the same frames every time
 * (a 30 Hz world on a 60 Hz display: a step pair on every other frame). Times are nanoseconds
 * on the host monotonic clock; the caller supplies them (tests use a fake clock).
 *
 * - Before the first present the clock is the host clock.
 * - At each present the frame time advances by one period. When it differs from the host time
 *   of the present by more than half a period (a missed deadline, a slow host, a lower present
 *   rate such as a 30 fps movie), it moves by whole periods to the one nearest the host time:
 *   game speed follows real time (never more than about one period off) and the frame times
 *   stay on the period grid, so a missed deadline does not shift the phase of the steps.
 * - Within a frame the time holds at the frame time for `hold` ns after the present, then
 *   advances with the host clock (code that waits on the clock without presenting still sees
 *   it move). It never reaches the next frame time before that frame is presented on schedule
 *   (hold > 0), and it never goes backwards. */
#define PACING_PERIOD_60HZ_NS 16666667   /* the Swap pacing period */

typedef struct {
    int64_t period, hold;   /* ns */
    int64_t base;           /* guest time of the last present */
    int64_t host_at;        /* host time of the last present */
    int64_t last;           /* last guest time handed out */
    int locked;             /* a present has been seen */
    uint64_t presents, resyncs;
} PacingClock;

void pacing_clock_init(PacingClock *c, int64_t period_ns);   /* hold = period / 2 */
void pacing_clock_present(PacingClock *c, int64_t host_ns);
int64_t pacing_clock_now(PacingClock *c, int64_t host_ns);
#endif
