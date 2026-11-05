/* I3DL2-style room reverb (see reverb.h).
 *
 * Levels: the early reflections carry (lRoom + lReflections) of energy per ear in total, and the
 * late reverb (lRoom + lReverb) integrated over its whole decay, independent of the decay time
 * (the FDN output is scaled by sqrt(1 - g^2), g the feedback gain over the mean line length, as
 * the I3DL2-derived OpenAL EFX reverb does). */
#include "reverb.h"
#include <math.h>
#include <string.h>

static const double PI = 3.14159265358979323846;
/* Line lengths at density 100 (mutually prime, 25-54 ms); density 0 scales them by 0.45. */
static const int line_base[REVERB_LINES] = {1201, 1361, 1559, 1741, 1949, 2129, 2357, 2591};
static const int ap_base[2] = {113, 337};
/* Early reflection tap positions (fraction of the reflection window) and relative amplitudes. */
static const float er_frac[2][REVERB_ER_TAPS] = {{0.00f, 0.11f, 0.23f, 0.37f, 0.50f, 0.62f, 0.77f, 0.91f},
                                                 {0.04f, 0.16f, 0.29f, 0.41f, 0.56f, 0.70f, 0.83f, 0.95f}};
static const float er_amp[REVERB_ER_TAPS] = {1.0f, -0.82f, 0.71f, -0.60f, 0.52f, -0.43f, 0.37f, -0.30f};
/* Output and input sign patterns: rows of an 8x8 Hadamard matrix (mutually orthogonal). */
static const float sign_l[REVERB_LINES] = {1, -1, 1, -1, 1, -1, 1, -1};
static const float sign_r[REVERB_LINES] = {1, 1, -1, -1, 1, 1, -1, -1};
static const float sign_in[REVERB_LINES] = {1, 1, 1, 1, -1, -1, -1, -1};

float reverb_mb_to_gain(int32_t mb) { return mb <= -10000 ? 0.0f : powf(10.0f, (float)mb / 2000.0f); }

void reverb_i3dl2_default(ReverbI3dl2 *p) {
    *p = (ReverbI3dl2){-10000, 0, 0.0f, 1.0f, 0.5f, -10000, 0.02f, -10000, 0.04f, 100.0f, 100.0f, 5000.0f};
}

void reverb_init(Reverb *r) {
    memset(r, 0, sizeof *r);
    ReverbI3dl2 p;
    reverb_i3dl2_default(&p);
    reverb_set_i3dl2(r, &p);
}

static float clampf(float v, float lo, float hi) { return !(v >= lo) ? lo : v > hi ? hi : v; }
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* One-pole low-pass y = b0 x + a1 y1 with gain dc at 0 Hz and dc * ratio at angular frequency w
 * (ratio <= 1; 1 gives a plain gain). */
static void one_pole(double dc, double ratio, double w, float *b0, float *a1) {
    double p = 0;
    if (ratio < 0.9999) {
        double r2 = ratio * ratio, c = cos(w), q = 1 - r2 * c, den = 1 - r2;
        p = (q - sqrt(q * q - den * den)) / den;
    }
    *b0 = (float)(dc * (1 - p));
    *a1 = (float)p;
}

void reverb_set_i3dl2(Reverb *r, const ReverbI3dl2 *in) {
    ReverbI3dl2 p = *in;
    p.room = clampi(p.room, -10000, 0);
    p.room_hf = clampi(p.room_hf, -10000, 0);
    p.room_rolloff = clampf(p.room_rolloff, 0.0f, 10.0f);
    p.decay_time = clampf(p.decay_time, 0.1f, 20.0f);
    p.decay_hf_ratio = clampf(p.decay_hf_ratio, 0.1f, 2.0f);
    p.reflections = clampi(p.reflections, -10000, 1000);
    p.reflections_delay = clampf(p.reflections_delay, 0.0f, 0.3f);
    p.reverb = clampi(p.reverb, -10000, 2000);
    p.reverb_delay = clampf(p.reverb_delay, 0.0f, 0.1f);
    p.diffusion = clampf(p.diffusion, 0.0f, 100.0f);
    p.density = clampf(p.density, 0.0f, 100.0f);
    p.hf_reference = clampf(p.hf_reference, 20.0f, 20000.0f);
    int was = r->active;
    r->p = p;
    r->active = p.room > -10000;
    if (!r->active) return;
    if (!was) {   /* a room after silence starts from empty lines */
        memset(r->in, 0, sizeof r->in);
        memset(r->ap, 0, sizeof r->ap);
        memset(r->line, 0, sizeof r->line);
        memset(r->line_y1, 0, sizeof r->line_y1);
        r->room_y1 = 0;
    }
    double w = 2 * PI * fmin(p.hf_reference, 0.45 * REVERB_RATE) / REVERB_RATE, room = reverb_mb_to_gain(p.room);
    one_pole(1.0, reverb_mb_to_gain(p.room_hf), w, &r->room_b0, &r->room_a1);

    /* early reflections over [reflections delay, + reverb delay (at least 5 ms)) */
    double er0 = p.reflections_delay * REVERB_RATE, span = fmax(p.reverb_delay, 0.005f) * REVERB_RATE, norm = 0;
    for (int k = 0; k < REVERB_ER_TAPS; k++) norm += er_amp[k] * er_amp[k];
    double er = room * reverb_mb_to_gain(p.reflections) / sqrt(norm);
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < REVERB_ER_TAPS; k++) {
            r->er_tap[c][k] = (int)lrint(er0 + span * er_frac[c][k]);
            r->er_gain[c][k] = (float)(er * er_amp[k]);
        }
    r->late_delay = (int)lrint((p.reflections_delay + p.reverb_delay) * REVERB_RATE);

    /* late reverb */
    r->ap_g = 0.7f * p.diffusion / 100.0f;
    for (int i = 0; i < 2; i++) {
        r->ap_len[i] = ap_base[i];
        if (r->ap_pos[i] >= r->ap_len[i]) r->ap_pos[i] = 0;
    }
    double scale = 0.45 + 0.55 * p.density / 100.0, t = p.decay_time, hf = p.decay_hf_ratio > 1 ? 1 : p.decay_hf_ratio, mean = 0;
    for (int i = 0; i < REVERB_LINES; i++) {
        int len = (int)lrint(line_base[i] * scale);
        r->line_len[i] = len;
        if (r->line_pos[i] >= len) r->line_pos[i] = 0;
        double g = pow(10.0, -3.0 * len / (REVERB_RATE * t)), ghf = pow(10.0, -3.0 * len / (REVERB_RATE * t * hf));
        one_pole(g, ghf / g, w, &r->line_b0[i], &r->line_a1[i]);
        mean += len;
    }
    mean /= REVERB_LINES;
    double gm = pow(10.0, -3.0 * mean / (REVERB_RATE * t));
    r->late_gain = (float)(room * reverb_mb_to_gain(p.reverb) * sqrt(1 - gm * gm));
}

int reverb_active(const Reverb *r) { return r->active; }

void reverb_process(Reverb *r, const float *send, float *acc, int n) {
    if (!r->active) return;
    const uint32_t mask = REVERB_IN_LEN - 1;
    const float inj = 0.35355339f;   /* 1 / sqrt(REVERB_LINES): the injected energy is 1 */
    for (int k = 0; k < n; k++) {
        uint32_t pos = r->in_pos;
        float x = r->room_b0 * send[k] + r->room_a1 * r->room_y1;
        x = (x + 1e-20f) - 1e-20f;   /* flush denormals in the decaying tail */
        r->room_y1 = x;
        r->in[pos] = x;
        float l = 0, rr = 0;
        for (int t = 0; t < REVERB_ER_TAPS; t++) {
            l += r->in[(pos - (uint32_t)r->er_tap[0][t]) & mask] * r->er_gain[0][t];
            rr += r->in[(pos - (uint32_t)r->er_tap[1][t]) & mask] * r->er_gain[1][t];
        }
        float v = r->in[(pos - (uint32_t)r->late_delay) & mask];
        for (int i = 0; i < 2; i++) {   /* Schroeder allpass diffusers */
            float *b = &r->ap[i][r->ap_pos[i]], o = *b, y = o - r->ap_g * v;
            *b = v + r->ap_g * y;
            v = y;
            if (++r->ap_pos[i] >= r->ap_len[i]) r->ap_pos[i] = 0;
        }
        float y[REVERB_LINES], sum = 0, ol = 0, orr = 0;
        for (int i = 0; i < REVERB_LINES; i++) {
            float o = r->line[i][r->line_pos[i]], f = r->line_b0[i] * o + r->line_a1[i] * r->line_y1[i];
            f = (f + 1e-20f) - 1e-20f;
            r->line_y1[i] = f;
            y[i] = f;
            sum += f;
            ol += sign_l[i] * f;
            orr += sign_r[i] * f;
        }
        sum *= 2.0f / REVERB_LINES;   /* Householder feedback: y - (2/N) sum(y) */
        for (int i = 0; i < REVERB_LINES; i++) {
            r->line[i][r->line_pos[i]] = y[i] - sum + sign_in[i] * inj * v;
            if (++r->line_pos[i] >= r->line_len[i]) r->line_pos[i] = 0;
        }
        acc[k * 2] += l + ol * r->late_gain;
        acc[k * 2 + 1] += rr + orr * r->late_gain;
        r->in_pos = (pos + 1) & mask;
    }
}
