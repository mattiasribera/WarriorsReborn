/* --combat-debug: pure helpers (combat_debug.h), unit-tested in tests/combat_debug_test.c. */
#include "combat_debug.h"
#include <math.h>

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

float cd_seg_seg_dist(const float p0[3], const float p1[3], const float q0[3], const float q1[3]) {
    const float d1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const float d2[3] = {q1[0] - q0[0], q1[1] - q0[1], q1[2] - q0[2]};
    const float r[3] = {p0[0] - q0[0], p0[1] - q0[1], p0[2] - q0[2]};
    const float a = dot3(d1, d1), e = dot3(d2, d2), f = dot3(d2, r);
    const float eps = 1e-12f;
    float s, t;
    if (a <= eps && e <= eps) {
        s = t = 0;
    } else if (a <= eps) {
        s = 0;
        t = clamp01(f / e);
    } else {
        const float c = dot3(d1, r);
        if (e <= eps) {
            t = 0;
            s = clamp01(-c / a);
        } else {
            const float b = dot3(d1, d2), den = a * e - b * b;
            s = den > eps ? clamp01((b * f - c * e) / den) : 0;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = clamp01(-c / a); }
            else if (t > 1) { t = 1; s = clamp01((b - c) / a); }
        }
    }
    float d = 0;
    for (int i = 0; i < 3; i++) {
        const float x = p0[i] + d1[i] * s - (q0[i] + d2[i] * t);
        d += x * x;
    }
    return sqrtf(d);
}

static float frames(uint32_t steps) { return (float)steps * 0.5f; }   /* 1/60 s steps -> 30 Hz frames */

int cd_move_update(CdMove *m, int move, uint32_t step, int on, CdMoveReport *out) {
    int done = 0;
    if (move != m->move) {
        if (m->valid && m->first_on && m->move >= 0) {
            const uint32_t off = m->on ? step : m->last_off;
            out->move = m->move;
            out->startup = frames(m->first_on - m->start);
            out->active = frames(off - m->first_on);
            out->recovery = frames(step - off);
            out->tests = m->tests;
            out->windows = m->windows;
            out->hits = m->hits;
            done = 1;
        }
        m->valid = m->move != -2;   /* the first move seen has no known start */
        m->move = move;
        m->start = step;
        m->first_on = 0;
        m->last_off = 0;
        m->tests = m->windows = m->hits = 0;
        m->on = 0;
    }
    if (on) {
        if (!m->on) {
            m->windows++;
            if (!m->first_on) m->first_on = step ? step : 1;
        }
        m->tests++;
    } else if (m->on) {
        m->last_off = step;
    }
    m->on = on;
    return done;
}

