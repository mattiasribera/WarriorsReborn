/* Voice resampling (see resample.h). */
#include "resample.h"
#include <math.h>
#include <string.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif

#define H RESAMPLE_SINC_HALF
#define TAPS (2 * H)
#ifndef RESAMPLE_PHASES
#define RESAMPLE_PHASES 1024   /* kernel rows per source frame; linear interpolation between rows */
#endif
#define PH RESAMPLE_PHASES
#define KAISER_BETA 8.96       /* 0.1102 * (A - 8.7) for A = 90 dB stopband */

/* poly[p][m] = h(m - (H - 1) - p / PH), the windowed sinc (cutoff at the source Nyquist) at tap m
 * for an output position p / PH past the source frame floor(pos); row PH (one frame on) closes the
 * last interpolation interval. Each row is normalized to unity DC gain; rows 0 and PH are exact
 * unit impulses (the sinc's zeros fall on the integer taps). */
static float poly[PH + 1][TAPS] __attribute__((aligned(16)));
/* The same (unnormalized) kernel as one curve for the stretched kernel at step > 1:
 * wide[q] = h(q / WPH - H), q = 0 .. TAPS * WPH. */
#define WPH 512
static float wide[TAPS * WPH + 2];
static int built;

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 64 && term > 1e-12 * sum; k++) {
        double q = x / (2 * k);
        term *= q * q;
        sum += term;
    }
    return sum;
}

static void build_kernel(void) {
    const double pi = 3.14159265358979323846, i0b = bessel_i0(KAISER_BETA);
    for (int p = 0; p <= PH; p++) {
        double row[TAPS], sum = 0;
        for (int m = 0; m < TAPS; m++) {
            double x = m - (H - 1) - (double)p / PH, r = x / H;
            if (p == 0 || p == PH) row[m] = fabs(x) < 1e-9 ? 1 : 0;
            else if (fabs(r) >= 1) row[m] = 0;
            else row[m] = sin(pi * x) / (pi * x) * bessel_i0(KAISER_BETA * sqrt(1 - r * r)) / i0b;
            sum += row[m];
        }
        for (int m = 0; m < TAPS; m++) poly[p][m] = (float)(row[m] / sum);
    }
    for (int q = 0; q <= TAPS * WPH; q++) {
        double x = (double)q / WPH - H, r = x / H;
        wide[q] = fabs(r) >= 1 ? 0 : fabs(x) < 1e-9 ? 1 : (float)(sin(pi * x) / (pi * x) * bessel_i0(KAISER_BETA * sqrt(1 - r * r)) / i0b);
    }
    wide[TAPS * WPH + 1] = 0;
    built = 1;
}

int resample_parse(const char *name, ResampleKind *kind) {
    if (!strcmp(name, "linear")) { *kind = RESAMPLE_LINEAR; return 0; }
    if (!strcmp(name, "sinc")) { *kind = RESAMPLE_SINC; return 0; }
    return -1;
}
const char *resample_name(ResampleKind kind) { return kind == RESAMPLE_SINC ? "sinc" : "linear"; }

static double scale_of(double step) { return step <= 1 ? 1 : step < RESAMPLE_MAX_SCALE ? step : RESAMPLE_MAX_SCALE; }

int resample_reach(ResampleKind kind, double step) {
    if (kind != RESAMPLE_SINC) return 1;
    return step <= 1 ? H : (int)ceil(H * scale_of(step));
}
int resample_max_reach(ResampleKind kind) { return kind == RESAMPLE_SINC ? RESAMPLE_MAX_REACH : 1; }

/* Positions are never negative (the caller keeps history before them), so (int) is floor. */
static void linear_block(const int16_t *src, int ch, double pos, double step, float *out, int n) {
    for (int k = 0; k < n; k++) {
        double p = pos + k * step;
        int i = (int)p;
        float t = (float)(p - i);
        for (int c = 0; c < ch; c++) {
            float a = src[i * ch + c], b = src[(i + 1) * ch + c];
            out[k * ch + c] = a + (b - a) * t;
        }
    }
}

/* step <= 1: the 64 taps of the two kernel rows around the phase, blended by the fraction. */
#ifdef __SSE2__
static inline __m128 lo_ps(__m128i v) { return _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(v, v), 16)); }
static inline __m128 hi_ps(__m128i v) { return _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpackhi_epi16(v, v), 16)); }
static inline float hsum(__m128 v) {
    v = _mm_add_ps(v, _mm_movehl_ps(v, v));
    return _mm_cvtss_f32(_mm_add_ss(v, _mm_shuffle_ps(v, v, 1)));
}
#endif

static void sinc_block(const int16_t *src, int ch, double pos, double step, float *out, int n) {
    for (int k = 0; k < n; k++) {
        double p = pos + k * step;
        int i = (int)p;
        double fp = (p - i) * PH;
        int ph = (int)fp;
        float t = (float)(fp - ph);
        const float *a = poly[ph], *b = poly[ph + 1];
        const int16_t *x = src + (i - H + 1) * ch;
#ifdef __SSE2__
        __m128 sa = _mm_setzero_ps(), sb = _mm_setzero_ps();
        if (ch == 1) {
            for (int m = 0; m < TAPS; m += 8) {
                __m128i v = _mm_loadu_si128((const __m128i *)(x + m));
                __m128 x0 = lo_ps(v), x1 = hi_ps(v);
                sa = _mm_add_ps(sa, _mm_add_ps(_mm_mul_ps(_mm_load_ps(a + m), x0), _mm_mul_ps(_mm_load_ps(a + m + 4), x1)));
                sb = _mm_add_ps(sb, _mm_add_ps(_mm_mul_ps(_mm_load_ps(b + m), x0), _mm_mul_ps(_mm_load_ps(b + m + 4), x1)));
            }
            float ya = hsum(sa), yb = hsum(sb);
            out[k] = ya + (yb - ya) * t;
        } else {   /* lanes L R L R: each coefficient used twice */
            for (int m = 0; m < TAPS; m += 4) {
                __m128i v = _mm_loadu_si128((const __m128i *)(x + 2 * m));
                __m128 x0 = lo_ps(v), x1 = hi_ps(v), a4 = _mm_load_ps(a + m), b4 = _mm_load_ps(b + m);
                sa = _mm_add_ps(sa, _mm_add_ps(_mm_mul_ps(_mm_unpacklo_ps(a4, a4), x0), _mm_mul_ps(_mm_unpackhi_ps(a4, a4), x1)));
                sb = _mm_add_ps(sb, _mm_add_ps(_mm_mul_ps(_mm_unpacklo_ps(b4, b4), x0), _mm_mul_ps(_mm_unpackhi_ps(b4, b4), x1)));
            }
            __m128 la = _mm_add_ps(sa, _mm_movehl_ps(sa, sa)), lb = _mm_add_ps(sb, _mm_movehl_ps(sb, sb));   /* [L, R, ..] */
            float ya[2], yb[2];
            _mm_storel_pi((__m64 *)ya, la);
            _mm_storel_pi((__m64 *)yb, lb);
            out[2 * k] = ya[0] + (yb[0] - ya[0]) * t;
            out[2 * k + 1] = ya[1] + (yb[1] - ya[1]) * t;
        }
#else
        float sa[2] = {0, 0}, sb[2] = {0, 0};
        for (int m = 0; m < TAPS; m++)
            for (int c = 0; c < ch; c++) {
                sa[c] += a[m] * x[m * ch + c];
                sb[c] += b[m] * x[m * ch + c];
            }
        for (int c = 0; c < ch; c++) out[k * ch + c] = sa[c] + (sb[c] - sa[c]) * t;
#endif
    }
}

/* step > 1 (source faster than the output): the kernel stretched by the step, so its cutoff
 * is the output Nyquist. The weights are read off the kernel curve and the sum renormalized
 * (exact DC gain). */
static void sinc_wide_block(const int16_t *src, int ch, double pos, double step, float *out, int n) {
    double s = scale_of(step), dq = WPH / s;
    int reach = (int)ceil(H * s), taps = 2 * reach;
    float w[2 * RESAMPLE_MAX_REACH + 2];
    for (int k = 0; k < n; k++) {
        double p = pos + k * step;
        int i = (int)p;
        double q = ((-reach + 1 - (p - i)) / s + H) * WPH;   /* curve index of the first tap */
        float wsum = 0;
        for (int j = 0; j < taps; j++, q += dq) {
            float v = 0;
            if (q > 0 && q < TAPS * WPH) {
                int qi = (int)q;
                float fr = (float)(q - qi);
                v = wide[qi] + (wide[qi + 1] - wide[qi]) * fr;
            }
            w[j] = v;
            wsum += v;
        }
        const int16_t *x = src + (i - reach + 1) * ch;
        float acc[2] = {0, 0};
        for (int j = 0; j < taps; j++)
            for (int c = 0; c < ch; c++) acc[c] += w[j] * x[j * ch + c];
        for (int c = 0; c < ch; c++) out[k * ch + c] = acc[c] / wsum;
    }
}

void resample_block(ResampleKind kind, const int16_t *src, int ch, double pos, double step, float *out, int n) {
    if (kind != RESAMPLE_SINC) { linear_block(src, ch, pos, step, out, n); return; }
    if (!built) build_kernel();
    if (step <= 1) sinc_block(src, ch, pos, step, out, n);
    else sinc_wide_block(src, ch, pos, step, out, n);
}
