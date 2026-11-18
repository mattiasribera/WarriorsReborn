/* Frame generation: options, the presentation rule and the slow-source guard (framegen.h). */
#include "framegen.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

FrameGenOptions framegen_options;

int framegen_parse_arg(const char *arg, FrameGenOptions *o) {
    if (!strcmp(arg, "--frame-gen")) {
        o->on = 1;
        return 1;
    }
    if (!strcmp(arg, "--frame-gen-of")) {
        o->on = o->of = 1;
        return 1;
    }
    if (!strcmp(arg, "--no-frame-gen")) {
        o->on = o->of = 0;
        return 1;
    }
    return 0;
}

int framegen_rate_ok(int hz) {
    if (hz <= 0) return 0;
    const int k = (hz + 60) / 120;   /* nearest multiple of 120 */
    const int d = hz - 120 * k;
    return k >= 1 && d >= -1 && d <= 1;
}

FrameGenDecision framegen_decide(const FrameGenOptions *opts, int supported, int presenting, int sim60, int display_hz, char *line,
                                 unsigned cap) {
    FrameGenDecision d;
    if (!opts->on) d = FRAMEGEN_OFF;
    else if (!supported) d = FRAMEGEN_UNAVAILABLE;
    else if (!sim60) d = FRAMEGEN_NO_SIM60;
    else if (!presenting) d = FRAMEGEN_READBACK;
    else if (!framegen_rate_ok(display_hz)) d = FRAMEGEN_RATE;
    else d = FRAMEGEN_SHOW;
    if (line && cap) {
        const char *of = opts->of ? " with optical flow" : "";
        switch (d) {
        case FRAMEGEN_OFF: snprintf(line, cap, "[frame-gen] off"); break;
        case FRAMEGEN_UNAVAILABLE: snprintf(line, cap, "[frame-gen] unavailable on this device: frames are shown as rendered"); break;
        case FRAMEGEN_NO_SIM60: snprintf(line, cap, "[frame-gen] off: it needs the 60 Hz simulation (--sim60)"); break;
        case FRAMEGEN_READBACK: snprintf(line, cap, "[frame-gen] frames are read back: generated frames are built%s, not presented", of); break;
        case FRAMEGEN_RATE:
            if (display_hz > 0)
                snprintf(line, cap, "[frame-gen] off: the display refreshes at %d Hz, not 120 Hz (fullscreen with --refresh-match picks 120 Hz)",
                         display_hz);
            else snprintf(line, cap, "[frame-gen] off: the display reports no refresh rate");
            break;
        case FRAMEGEN_SHOW: snprintf(line, cap, "[frame-gen] on%s: 120 frames a second on the %d Hz display", of, display_hz); break;
        }
    }
    return d;
}

int framegen_full_screen(const void *v, unsigned stride, unsigned n) {
    if (!v || n < 3) return 0;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (unsigned i = 0; i < n; i++) {
        const float *f = (const float *)((const char *)v + (size_t)i * stride);
        if (!(f[3] > 0)) return 0;
        const float x = f[0] / f[3], y = f[1] / f[3];
        x0 = x < x0 ? x : x0;
        x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0;
        y1 = y > y1 ? y : y1;
    }
    const float e = 1.0f - FRAMEGEN_FULL_EDGE;
    return x0 <= -e && x1 >= e && y0 <= -e && y1 >= e;
}

double framegen_resume_hold(int pauses) {
    double h = FRAMEGEN_HOLD_MS;
    for (int i = 1; i < pauses && h < FRAMEGEN_HOLD_MAX_MS; i++) h *= 2;
    return h < FRAMEGEN_HOLD_MAX_MS ? h : FRAMEGEN_HOLD_MAX_MS;
}

int framegen_pace(FrameGenPace *p, double interval_ms, int *changed) {
    if (changed) *changed = 0;
    if (!(interval_ms > 0) || interval_ms > FRAMEGEN_GAP_MS) {   /* a pause: start over, keep the state */
        p->avg = 0;
        p->held = 0;
        return !p->slow;
    }
    p->avg = p->avg > 0 ? p->avg + (interval_ms - p->avg) * 0.1 : interval_ms;
    if (!p->slow && (p->shown += interval_ms) >= FRAMEGEN_CALM_MS) p->pauses = 0;
    const int other = p->slow ? p->avg < FRAMEGEN_FAST_MS : p->avg > FRAMEGEN_SLOW_MS;
    p->held = other ? p->held + interval_ms : 0;
    if (p->held >= (p->slow ? framegen_resume_hold(p->pauses) : FRAMEGEN_HOLD_MS)) {
        p->slow = !p->slow;
        p->held = 0;
        if (p->slow) p->pauses++;
        else p->shown = 0;
        if (changed) *changed = 1;
    }
    return !p->slow;
}

void framegen_clock_add(FrameGenClock *c, double t_ms, int waited) {
    if (!waited) {   /* the frame was on screen already: when is unknown */
        c->last = 0;
        return;
    }
    const double nominal = c->period > 0 ? c->period : 1000.0 / 60.0, dt = t_ms - c->last;
    if (c->last > 0 && dt > 0) {
        /* Whole frames apart (a frame missed, a sample without a wait between them). */
        const double k = (double)(long)(dt / nominal + 0.5);
        const double one = k >= 1 && k <= 8 ? dt / k : 0, ref = 1000.0 / 60.0;
        if (one > ref * (1 - FRAMEGEN_CLOCK_SPAN) && one < ref * (1 + FRAMEGEN_CLOCK_SPAN)) {
            c->period = c->period > 0 ? c->period + (one - c->period) * (c->n < 30 ? 1.0 / (c->n + 1) : 0.02) : one;
            c->n++;
        }
    }
    c->last = t_ms;
}

double framegen_clock_period(const FrameGenClock *c) { return c->n >= FRAMEGEN_CLOCK_LOCK ? c->period : 0; }
