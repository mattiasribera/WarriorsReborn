/* Frame pacing helpers (see pacing.h). */
#include "pacing.h"
#include <stdio.h>
#include <string.h>

void pacing_stats_break(PacingStats *s) {
    s->have_last = 0;
    s->have_state = 0;
    s->run = 0;
}

void pacing_stats_frame(PacingStats *s, uint32_t counter) {
    if (!s->have_last || counter < s->last || counter - s->last > PACING_MAX_STEP_JUMP) {
        pacing_stats_break(s);   /* first value, reset or jump: prime the sequence */
        s->have_last = 1;
        s->last = counter;
        return;
    }
    uint32_t n = counter / 2 - s->last / 2;   /* even values in (last, counter] */
    s->last = counter;
    s->frames++;
    s->updates[n < 2 ? n : 2]++;
    if (!n) {
        if (s->have_state) s->run++;
        return;
    }
    if (s->have_state) s->holds[s->run < 4 ? s->run - 1 : 3]++;
    s->have_state = 1;
    s->run = 1;
}

double pacing_stats_hold2(const PacingStats *s) {
    uint64_t n = s->holds[0] + s->holds[1] + s->holds[2] + s->holds[3];
    return n ? (double)s->holds[1] / (double)n : 0;
}

void pacing_stats_diff(const PacingStats *now, const PacingStats *before, PacingStats *out) {
    *out = *now;
    for (int i = 0; i < 3; i++) out->updates[i] -= before->updates[i];
    for (int i = 0; i < 4; i++) out->holds[i] -= before->holds[i];
    out->frames -= before->frames;
}

void pacing_stats_format(const PacingStats *s, char *buf, size_t len) {
    snprintf(buf, len, "updates/frame 0:%llu 1:%llu 2+:%llu | holds 1:%llu 2:%llu 3:%llu 4+:%llu (2 frames %.1f%%)",
             (unsigned long long)s->updates[0], (unsigned long long)s->updates[1], (unsigned long long)s->updates[2],
             (unsigned long long)s->holds[0], (unsigned long long)s->holds[1], (unsigned long long)s->holds[2],
             (unsigned long long)s->holds[3], 100.0 * pacing_stats_hold2(s));
}


/* ---- PacingClock ---- */
void pacing_clock_init(PacingClock *c, int64_t period_ns) {
    memset(c, 0, sizeof *c);
    c->period = period_ns;
    c->hold = period_ns / 2;
}

void pacing_clock_present(PacingClock *c, int64_t host_ns) {
    int64_t next = c->base + c->period;
    if (!c->locked) {
        c->locked = 1;
        next = host_ns;
    } else if (host_ns - next > c->period / 2 || next - host_ns > c->period / 2) {
        /* More than half a period off real time (a missed deadline, a slow host, a lower
         * present rate): skip whole periods to the one nearest the host time, so the frame
         * times stay on the period grid and fixed steps keep their phase. */
        int64_t lag = host_ns - next;
        next += (lag >= 0 ? (lag + c->period / 2) / c->period : -((-lag + c->period / 2) / c->period)) * c->period;
        c->resyncs++;
    }
    c->base = next > c->last ? next : c->last;
    c->host_at = host_ns;
    c->presents++;
}

int64_t pacing_clock_now(PacingClock *c, int64_t host_ns) {
    int64_t g = host_ns;
    if (c->locked) {
        int64_t d = host_ns - c->host_at;
        g = c->base + (d > c->hold ? d - c->hold : 0);
    }
    if (g < c->last) g = c->last;
    c->last = g;
    return g;
}
