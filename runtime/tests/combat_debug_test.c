/* Game-independent checks of the --combat-debug helpers: the swept
 * segment distance the log reports per hit test, and the frame data per move (startup, active,
 * recovery in 30 Hz frames of game time from 1/60 s world steps). */
#include "../src/combat_debug.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

int main(void) {
    /* Distances. */
    const float o[3] = {0, 0, 0}, x1[3] = {1, 0, 0}, y1[3] = {0, 1, 0}, p[3] = {0.5f, 2, 0};
    CHECK(near(cd_seg_seg_dist(o, o, p, p), sqrtf(4.25f)), "point to point");
    CHECK(near(cd_seg_seg_dist(o, x1, p, p), 2), "point above the middle of a segment: 2 (%.4f)", cd_seg_seg_dist(o, x1, p, p));
    const float q0[3] = {0.5f, -1, 1}, q1[3] = {0.5f, 1, 1};
    CHECK(near(cd_seg_seg_dist(o, x1, q0, q1), 1), "crossing segments one unit apart: 1");
    const float r0[3] = {3, 0, 0}, r1[3] = {4, 0, 0};
    CHECK(near(cd_seg_seg_dist(o, x1, r0, r1), 2), "collinear, end to end: 2");
    const float s0[3] = {0, 0.5f, 0}, s1[3] = {1, 0.5f, 0};
    CHECK(near(cd_seg_seg_dist(o, x1, s0, s1), 0.5f), "parallel segments: 0.5");
    CHECK(near(cd_seg_seg_dist(o, y1, o, x1), 0), "touching at a shared end: 0");
    /* A fast swing passing a target between two samples: both end points are far, the sweep is not. */
    const float a0[3] = {-1, 0.1f, 0}, a1[3] = {1, 0.1f, 0}, t[3] = {0, 0, 0};
    CHECK(cd_seg_seg_dist(a0, a0, t, t) > 1 && cd_seg_seg_dist(a1, a1, t, t) > 1 && near(cd_seg_seg_dist(a0, a1, t, t), 0.1f),
          "a sweep through the target is 0.1 away though both ends are over 1 away");

    /* Frame data: a move seen from its start, steps of 2 (30 Hz world). */
    CdMove m = {.move = -2};
    CdMoveReport r = {0};
    int done = 0;
    done |= cd_move_update(&m, 5, 100, 0, &r);                     /* first move: start unknown */
    done |= cd_move_update(&m, 12, 110, 0, &r);                    /* move 12 starts at step 110 */
    CHECK(!done, "no report for a move without strike windows, nor for the first one seen");
    for (uint32_t s = 112; s < 120; s += 2) done |= cd_move_update(&m, 12, s, 0, &r);
    for (uint32_t s = 120; s < 130; s += 2) done |= cd_move_update(&m, 12, s, 1, &r);   /* on 120..128 */
    for (uint32_t s = 130; s < 150; s += 2) done |= cd_move_update(&m, 12, s, 0, &r);   /* off at 130 */
    CHECK(!done, "no report while the move lasts");
    done = cd_move_update(&m, 16, 150, 0, &r);
    CHECK(done && r.move == 12, "the move's report comes when the next move starts (move %d)", r.move);
    CHECK(near(r.startup, 5) && near(r.active, 5) && near(r.recovery, 10),
          "30 Hz: startup 5, active 5, recovery 10 frames (%.1f %.1f %.1f)", r.startup, r.active, r.recovery);
    CHECK(r.tests == 5 && r.windows == 1, "5 hit tests in 1 window (%d, %d)", r.tests, r.windows);

    /* The same move with a 60 Hz world: steps of 1, the same game times. */
    CdMove n = {.move = -2};
    cd_move_update(&n, 1, 0, 0, &r);
    cd_move_update(&n, 12, 110, 0, &r);
    for (uint32_t s = 111; s < 120; s++) cd_move_update(&n, 12, s, 0, &r);
    for (uint32_t s = 120; s < 130; s++) cd_move_update(&n, 12, s, 1, &r);
    for (uint32_t s = 130; s < 150; s++) cd_move_update(&n, 12, s, 0, &r);
    done = cd_move_update(&n, 16, 150, 0, &r);
    CHECK(done && near(r.startup, 5) && near(r.active, 5) && near(r.recovery, 10) && r.tests == 10,
          "60 Hz: same frames, twice the tests (%.1f %.1f %.1f, %d tests)", r.startup, r.active, r.recovery, r.tests);

    /* A move that ends while its strike is still on: active up to the end, recovery 0. */
    CdMove k = {.move = -2};
    cd_move_update(&k, 1, 0, 0, &r);
    cd_move_update(&k, 7, 10, 1, &r);
    cd_move_update(&k, 7, 12, 1, &r);
    done = cd_move_update(&k, 8, 14, 0, &r);
    CHECK(done && near(r.startup, 0) && near(r.active, 2) && near(r.recovery, 0), "cut off while on: 0 / 2 / 0 (%.1f %.1f %.1f)",
          r.startup, r.active, r.recovery);
    /* Two windows in one move (a double strike). */
    CdMove w = {.move = -2};
    cd_move_update(&w, 1, 0, 0, &r);
    cd_move_update(&w, 9, 10, 0, &r);
    cd_move_update(&w, 9, 12, 1, &r);
    cd_move_update(&w, 9, 14, 0, &r);
    cd_move_update(&w, 9, 16, 1, &r);
    cd_move_update(&w, 9, 18, 0, &r);
    done = cd_move_update(&w, 3, 30, 0, &r);
    CHECK(done && r.windows == 2 && near(r.startup, 1) && near(r.active, 3) && near(r.recovery, 6),
          "two windows: startup 1, active 3 (first on to last off), recovery 6 (%d, %.1f %.1f %.1f)", r.windows, r.startup, r.active,
          r.recovery);

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
