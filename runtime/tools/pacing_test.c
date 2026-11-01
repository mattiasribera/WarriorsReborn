/* Game-independent test of the frame pacing helpers (src/pacing.c): the cadence statistics fed
 * with synthetic step counters, and the frame-locked clock driven by a fake host clock (on
 * schedule, lag beyond a period, recovery, slow and bursting presents, monotonic reads). No game
 * data, no guest memory. */
#include "../src/pacing.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define U(x) ((unsigned long long)(x))

/* Feed a step counter that advances by the given steps per presented frame. */
static uint32_t feed(PacingStats *s, uint32_t c, const int *steps, int n, int repeat) {
    for (int r = 0; r < repeat; r++)
        for (int i = 0; i < n; i++) pacing_stats_frame(s, c += (uint32_t)steps[i]);
    return c;
}

static void test_stats(void) {
    PacingStats s;
    char buf[160];

    /* Even pacing: a step pair every other frame -> one update per 2 frames, every state held 2. */
    memset(&s, 0, sizeof s);
    static const int even[] = {2, 0};
    feed(&s, 0, even, 2, 50);
    CHECK(s.frames == 99, "even: %llu frames counted (the first only primes the counter)", U(s.frames));
    CHECK(s.updates[0] == 50 && s.updates[1] == 49 && s.updates[2] == 0, "even: updates/frame 0:%llu 1:%llu 2+:%llu",
          U(s.updates[0]), U(s.updates[1]), U(s.updates[2]));
    CHECK(s.holds[1] == 48 && s.holds[0] == 0 && s.holds[2] == 0 && s.holds[3] == 0, "even: holds 1:%llu 2:%llu 3:%llu 4+:%llu",
          U(s.holds[0]), U(s.holds[1]), U(s.holds[2]), U(s.holds[3]));
    CHECK(pacing_stats_hold2(&s) == 1.0, "even: share held 2 frames %.3f", pacing_stats_hold2(&s));

    /* Slipping phase: holds of 3, 2 and 1 frames in turn (the first frame primes the counter). */
    memset(&s, 0, sizeof s);
    static const int slip[] = {2, 2, 0, 0, 2, 0};
    feed(&s, 0, slip, 6, 10);
    CHECK(s.holds[0] == 9 && s.holds[1] == 9 && s.holds[2] == 10, "slip: holds 1:%llu 2:%llu 3:%llu",
          U(s.holds[0]), U(s.holds[1]), U(s.holds[2]));
    CHECK(s.updates[0] == 30 && s.updates[1] == 29, "slip: updates/frame 0:%llu 1:%llu", U(s.updates[0]), U(s.updates[1]));

    /* Odd steps: only even counter values are world updates; a frame crossing two even values
     * counts 2+, its first state is never on screen (no hold recorded for it). */
    memset(&s, 0, sizeof s);
    pacing_stats_frame(&s, 1);
    pacing_stats_frame(&s, 2);   /* 1 update (2) */
    pacing_stats_frame(&s, 3);   /* 0 */
    pacing_stats_frame(&s, 6);   /* 2 updates (4, 6): state 2 held 2 frames */
    pacing_stats_frame(&s, 7);   /* 0 */
    pacing_stats_frame(&s, 7);   /* 0 */
    pacing_stats_frame(&s, 7);   /* 0 */
    pacing_stats_frame(&s, 7);   /* 0 */
    pacing_stats_frame(&s, 8);   /* 1: state 6 held 5 frames -> 4+ */
    CHECK(s.updates[0] == 5 && s.updates[1] == 2 && s.updates[2] == 1, "parity: updates/frame 0:%llu 1:%llu 2+:%llu",
          U(s.updates[0]), U(s.updates[1]), U(s.updates[2]));
    CHECK(s.holds[1] == 1 && s.holds[3] == 1 && s.holds[0] == 0 && s.holds[2] == 0, "parity: holds 2:%llu 4+:%llu",
          U(s.holds[1]), U(s.holds[3]));

    /* A reset (counter goes back or jumps far) or an unreadable frame restarts the sequence
     * without recording a hold across it. */
    memset(&s, 0, sizeof s);
    pacing_stats_frame(&s, 100);
    pacing_stats_frame(&s, 102);
    pacing_stats_frame(&s, 0);              /* reset */
    pacing_stats_frame(&s, 2);              /* first state after the reset: no hold recorded */
    pacing_stats_frame(&s, 2 + 4 * PACING_MAX_STEP_JUMP);   /* jump */
    pacing_stats_break(&s);
    pacing_stats_frame(&s, 50);
    pacing_stats_frame(&s, 52);
    pacing_stats_frame(&s, 52);
    pacing_stats_frame(&s, 54);             /* state 52 held 2 frames: the only hold */
    CHECK(s.holds[0] + s.holds[1] + s.holds[2] + s.holds[3] == 1 && s.holds[1] == 1, "reset: only the hold after the break counts (2:%llu)",
          U(s.holds[1]));
    CHECK(s.frames == 5, "reset: %llu continuous frames counted", U(s.frames));
    CHECK(pacing_stats_hold2(&(PacingStats){0}) == 0, "no states: share 0");

    /* Window difference and the formatted line. */
    PacingStats a, b, w;
    memset(&a, 0, sizeof a);
    uint32_t c = feed(&a, 0, slip, 6, 2);
    b = a;
    feed(&b, c, even, 2, 10);
    pacing_stats_diff(&b, &a, &w);
    CHECK(w.frames == 20 && w.updates[1] == 10 && w.holds[1] == 10 && w.holds[2] == 0, "window: %llu frames, 1:%llu, holds 2:%llu 3:%llu",
          U(w.frames), U(w.updates[1]), U(w.holds[1]), U(w.holds[2]));
    pacing_stats_format(&w, buf, sizeof buf);
    CHECK(!strcmp(buf, "updates/frame 0:10 1:10 2+:0 | holds 1:0 2:10 3:0 4+:0 (2 frames 100.0%)"), "format: \"%s\"", buf);
}

/* ---- PacingClock with a fake host clock ---- */
#define P 16666667LL   /* 60 Hz period, ns (as the Swap pacing) */
#define STEP_TICKS 12222222LL   /* a title's 1/60 s step in 733 MHz ticks */

static uint32_t rnd_state = 12345;
static int64_t rnd(int64_t n) { rnd_state = rnd_state * 1103515245u + 12345u; return (int64_t)((rnd_state >> 8) % (uint32_t)n); }

/* Monotonic reads: every value handed out must be >= the previous one. */
static int64_t last_read;
static int mono_ok = 1;
static int64_t rd(PacingClock *c, int64_t h) {
    int64_t g = pacing_clock_now(c, h);
    if (g < last_read) mono_ok = 0;
    last_read = g;
    return g;
}

/* A title loop: read the clock once per frame `read_after` ns after the present, run fixed steps
 * in pairs whenever two steps have elapsed (as a step loop that wants >= 2 steps), present. The
 * host present times are the 60 Hz deadlines plus a wake-up jitter. Returns the hold histogram
 * of the world states (a world update on each pair). */
static void title_loop(PacingClock *c, int use_clock, int frames, int64_t max_read, int stall_every, PacingStats *s) {
    int64_t h0 = 1000000000LL, last_step = -1;
    uint32_t counter = 0;
    memset(s, 0, sizeof *s);
    for (int k = 0, v = 0; k < frames; k++, v++) {
        if (stall_every && k % stall_every == stall_every - 1) {   /* a missed deadline: the last image stays a period longer */
            v++;
            pacing_stats_frame(s, counter);
        }
        int64_t present = h0 + v * P + rnd(300000);   /* wake-up jitter up to 0.3 ms */
        if (use_clock) pacing_clock_present(c, present);
        int64_t read_h = present + rnd(max_read);
        int64_t t = use_clock ? rd(c, read_h) : read_h;
        int64_t ticks = t * 733333LL / 1000000LL;   /* 733.333 MHz ticks (no 64-bit overflow) */
        if (last_step < 0) last_step = ticks;
        while (ticks - last_step >= 2 * STEP_TICKS) { last_step += 2 * STEP_TICKS; counter += 2; }
        pacing_stats_frame(s, counter);
    }
}

static void test_clock(void) {
    PacingClock c;
    PacingStats s;

    /* Before the first present: the host clock. */
    pacing_clock_init(&c, P);
    last_read = 0;
    CHECK(c.hold == P / 2, "hold defaults to half a period (%lld ns)", (long long)c.hold);
    CHECK(rd(&c, 5000) == 5000 && rd(&c, 900000) == 900000, "unlocked: host time");

    /* On schedule: +1 period per present, held at the frame time early in the frame, then
     * advancing, never reaching the next frame time before the next present. */
    int64_t h = 1000000000LL;
    pacing_clock_present(&c, h);
    CHECK(rd(&c, h + 2000000) == h, "first present: frame time = host time of the present");
    pacing_clock_present(&c, h + P + 250000);   /* woke 0.25 ms late */
    int64_t base = h + P;
    CHECK(rd(&c, h + P + 300000) == base && rd(&c, h + P + 5000000) == base, "on schedule: frame time + 1 period exactly");
    CHECK(rd(&c, h + P + 250000 + P / 2 + 1000000) == base + 1000000, "late in the frame: advances with the host clock");
    int64_t g = rd(&c, h + 2 * P - 1);
    CHECK(g < base + P, "before the next present: below the next frame time (%lld < %lld)", (long long)g, (long long)(base + P));
    for (int k = 2; k < 600; k++) pacing_clock_present(&c, h + k * P + rnd(400000));
    CHECK(rd(&c, h + 599 * P + 500000) == h + 599 * P && c.resyncs == 0, "600 presents with jitter: exact frame times, no resync");

    /* The title loop: step pairs on every other frame with the locked clock; with the host clock
     * the pair slips when the read time crosses the step phase (holds of 1 and 3 frames). */
    rnd_state = 7;
    pacing_clock_init(&c, P);
    last_read = 0;
    title_loop(&c, 1, 3000, 4000000, 0, &s);
    CHECK(pacing_stats_hold2(&s) == 1.0 && s.holds[1] > 1400, "title loop, locked clock: %llu of %llu states held 2 frames",
          U(s.holds[1]), U(s.holds[0] + s.holds[1] + s.holds[2] + s.holds[3]));
    rnd_state = 7;
    title_loop(&c, 0, 3000, 4000000, 0, &s);
    CHECK(s.holds[0] > 0 && s.holds[2] > 0, "title loop, host clock: slips (holds 1:%llu 2:%llu 3:%llu)", U(s.holds[0]),
          U(s.holds[1]), U(s.holds[2]));

    /* Missed deadlines (every 37th frame presented a period late): the frame times stay on the
     * period grid, so on screen every state still lasts 2 periods. */
    rnd_state = 9;
    pacing_clock_init(&c, P);
    last_read = 0;
    title_loop(&c, 1, 3000, 4000000, 37, &s);
    CHECK(pacing_stats_hold2(&s) == 1.0 && c.resyncs == 81, "missed deadlines, locked clock: %llu of %llu states held 2 periods (%llu resyncs)",
          U(s.holds[1]), U(s.holds[0] + s.holds[1] + s.holds[2] + s.holds[3]), U(c.resyncs));
    /* A frame 0.7 period late: one whole period skipped, not the host time. */
    pacing_clock_init(&c, P);
    last_read = 0;
    pacing_clock_present(&c, 1000000000LL);
    pacing_clock_present(&c, 1000000000LL + P + 7 * P / 10);
    CHECK(rd(&c, 1000000000LL + P + 7 * P / 10 + 1000) == 1000000000LL + 2 * P && c.resyncs == 1, "0.7 period late: +2 periods (on the grid)");

    /* Lag beyond a period (a stall): the frame time jumps to the host time; then recovery. */
    pacing_clock_init(&c, P);
    last_read = 0;
    h = 2000000000LL;
    for (int k = 0; k < 10; k++) pacing_clock_present(&c, h + k * P);
    int64_t late = h + 9 * P + 5 * P;   /* five frames late */
    pacing_clock_present(&c, late);
    CHECK(rd(&c, late + 1000) == late && c.resyncs == 1, "stall: resync to the host time, 4 whole periods (%llu resync)", U(c.resyncs));
    pacing_clock_present(&c, late + P + 100000);
    CHECK(rd(&c, late + P + 200000) == late + P && c.resyncs == 1, "recovery: back to +1 period per present");
    /* A frame up to a period late keeps the schedule (no resync, game time within a period). */
    pacing_clock_present(&c, late + 2 * P + P / 2);
    CHECK(rd(&c, late + 2 * P + P / 2 + 1000) == late + 2 * P && c.resyncs == 1, "half a period late: schedule kept (no resync)");

    /* Slow host (presents every 20 ms) and 30 Hz presents (a movie): game time follows real
     * time within about a period; reads stay monotonic. */
    static const int64_t gaps[] = {20000000LL, 2 * P, 1000000LL};
    static const char *names[] = {"20 ms presents", "30 Hz presents", "1 ms presents (catch-up burst)"};
    for (int i = 0; i < 3; i++) {
        pacing_clock_init(&c, P);
        last_read = 0;
        int64_t worst = 0;
        h = 3000000000LL;
        for (int k = 0; k < 600; k++) {
            int64_t hp = h + k * gaps[i];
            pacing_clock_present(&c, hp);
            int64_t d = rd(&c, hp + 100000) - hp;
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
        CHECK(worst <= P / 2 + 400000 || (i == 2 && worst <= P), "%s: game time within half a period of host time (a period when presents run ahead) (worst %.2f ms, %llu resyncs)", names[i], worst / 1e6,
              U(c.resyncs));
    }

    /* An overrun frame (no present for 3 periods): the clock keeps moving and stays monotonic
     * across the late present. */
    pacing_clock_init(&c, P);
    last_read = 0;
    h = 4000000000LL;
    pacing_clock_present(&c, h);
    int64_t a1 = rd(&c, h + 2 * P), a2 = rd(&c, h + 3 * P);
    CHECK(a2 > a1 && a1 > h, "overrun: time advances without a present (%lld -> %lld ns)", (long long)(a1 - h), (long long)(a2 - h));
    pacing_clock_present(&c, h + 3 * P + 1000);
    rd(&c, h + 3 * P + 2000);
    pacing_clock_present(&c, h + 3 * P + 3000);   /* a second present right after */
    rd(&c, h + 3 * P + 4000);
    CHECK(mono_ok, "monotonic across every read in these tests");
}

int main(void) {
    test_stats();
    test_clock();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL OK\n");
    return 0;
}
