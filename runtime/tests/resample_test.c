/* Game-independent resampler test (src/resample.c and the mixer FIFO in src/audio.c).
 *   resample_test [sinc|linear]   run the checks with that resampler (default sinc); prints
 *                                 "ALL OK" or FAIL lines and the CPU cost per voice.
 * The spectral checks (images above the source Nyquist about -90 dB, passband flat to
 * 0.45 fs_in, no aliasing when the source is faster than the output) are the quality target:
 * linear interpolation fails them by design. The mixer checks (voice start and end, looping,
 * stereo, ratio changes mid-stream) hold for both resamplers. Synthetic signals only. */
#define _GNU_SOURCE
#include "../src/audio.h"
#include "../src/resample.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static ResampleKind kind = RESAMPLE_SINC;
static const double PI = 3.14159265358979323846;

/* ---- spectral checks on resample_block ---- */
#define NFFT 16384

static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(a * k), wi = sin(a * k);
                double *ur = re + i + k, *ui = im + i + k, *vr = re + i + k + len / 2, *vi = im + i + k + len / 2;
                double xr = *vr * wr - *vi * wi, xi = *vr * wi + *vi * wr;
                *vr = *ur - xr; *vi = *ui - xi;
                *ur += xr; *ui += xi;
            }
    }
}

/* Amplitude of the sinusoid of angular step w per sample (least squares over n samples of x,
 * stride apart), relative to amp. */
static double fit_gain(const float *x, int stride, int n, double w, double phase0, double amp) {
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (int k = 0; k < n; k++) {
        double s = sin(w * k + phase0), c = cos(w * k + phase0), v = x[k * stride];
        ss += s * s; cc += c * c; sc += s * c; xs += x[k * stride] * s; xc += v * c;
    }
    double det = ss * cc - sc * sc, a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    return sqrt(a * a + b * b) / amp;
}

/* Energy of x (stride apart, NFFT samples, Blackman-Harris window) in [lo, hi) Hz at 48 kHz. */
static double band_energy(const float *x, int stride, double lo, double hi) {
    static double re[NFFT], im[NFFT];
    for (int k = 0; k < NFFT; k++) {
        double t = 2 * PI * k / (NFFT - 1);
        re[k] = x[k * stride] * (0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2 * t) - 0.01168 * cos(3 * t));
        im[k] = 0;
    }
    fft(re, im, NFFT);
    double e = 0;
    for (int b = 0; b <= NFFT / 2; b++) {
        double f = (double)b * AUDIO_RATE / NFFT;
        if (f >= lo && f < hi) e += re[b] * re[b] + im[b] * im[b];
    }
    return e;
}

/* Resamples a sine (one tone per channel) from rate to 48 kHz; per channel: gain at the tone (dB),
 * and the energy above the source Nyquist (or, when the tone is above the output Nyquist, the
 * whole output) relative to the tone, in dB. */
static void sine_case(int ch, double rate, const double f[2], double gain_db[2], double spur_db[2]) {
    const double amp = 16384;
    double step = rate / AUDIO_RATE;
    int reach = resample_max_reach(kind) + 2, pos0 = reach;
    int len = pos0 + (int)(NFFT * step) + reach + 4;
    int16_t *src = malloc(sizeof(int16_t) * len * ch);
    float *out = malloc(sizeof(float) * NFFT * ch);
    for (int i = 0; i < len; i++)
        for (int c = 0; c < ch; c++) src[i * ch + c] = (int16_t)lrint(amp * sin(2 * PI * f[c] * i / rate + 0.3));
    resample_block(kind, src, ch, pos0, step, out, NFFT);
    for (int c = 0; c < ch; c++) {
        double w = 2 * PI * f[c] / AUDIO_RATE, ph = 2 * PI * f[c] * pos0 / rate + 0.3;
        if (f[c] < AUDIO_RATE / 2) {
            gain_db[c] = 20 * log10(fit_gain(out + c, ch, NFFT, w, ph, amp));
            double tone = band_energy(out + c, ch, f[c] - 40, f[c] + 40);
            double lo = rate / 2 < AUDIO_RATE / 2 ? rate / 2 : AUDIO_RATE / 2;
            spur_db[c] = 10 * log10(band_energy(out + c, ch, lo, AUDIO_RATE / 2 + 1) / tone);
        } else {   /* above the output Nyquist: everything that comes out is an alias */
            gain_db[c] = 0;
            double e = 0;
            for (int k = 0; k < NFFT; k++) e += (double)out[k * ch + c] * out[k * ch + c];
            spur_db[c] = 10 * log10(e / NFFT / (amp * amp / 2) + 1e-30);
        }
    }
    free(src);
    free(out);
}

static void test_spectral(void) {
    static const double rates[] = {22050, 26500, 32250, 11025};
    static const double rel[] = {0.05, 0.25, 0.45};
    double worst_gain = 0, worst_spur = -300;
    for (int r = 0; r < 4; r++)
        for (int t = 0; t < 3; t++) {
            double f[2] = {rel[t] * rates[r], rel[t] * rates[r]}, g[2], s[2];
            sine_case(1, rates[r], f, g, s);
            CHECK(fabs(g[0]) <= 0.1, "passband: %.0f Hz tone (%.2f fs_in) from %.0f Hz, gain %+.3f dB (flat within 0.1 dB)", f[0],
                  rel[t], rates[r], g[0]);
            CHECK(s[0] <= -85, "images: %.0f Hz tone from %.0f Hz, energy above the source Nyquist %.1f dB (<= -85 dB)", f[0],
                  rates[r], s[0]);
            worst_gain = fmax(worst_gain, fabs(g[0]));
            worst_spur = fmax(worst_spur, s[0]);
        }
    double fs[2] = {1000, 0.45 * 26500}, g[2], s[2];   /* stereo music: a tone per channel */
    sine_case(2, 26500, fs, g, s);
    CHECK(fabs(g[0]) <= 0.1 && fabs(g[1]) <= 0.1 && s[0] <= -85 && s[1] <= -85,
          "stereo 26500 Hz: L %+.3f dB / %.1f dB images, R %+.3f dB / %.1f dB images", g[0], s[0], g[1], s[1]);
    double hi[2] = {30000, 30000}, lo[2] = {10000, 10000};   /* source faster than the output */
    sine_case(1, 96000, hi, g, s);
    CHECK(s[0] <= -85, "aliasing: 30 kHz tone from 96 kHz (step 2) comes out at %.1f dB (<= -85 dB)", s[0]);
    worst_spur = fmax(worst_spur, s[0]);
    sine_case(1, 96000, lo, g, s);
    CHECK(fabs(g[0]) <= 0.1, "passband: 10 kHz tone from 96 kHz, gain %+.3f dB", g[0]);
    printf("info worst passband deviation %.3f dB, worst image/alias level %.1f dB (%s)\n", worst_gain, worst_spur,
           resample_name(kind));

    /* Ratio 1 (48 kHz sources such as Bink movie audio): the samples pass through unchanged. */
    int16_t src[1024];
    float out[512];
    for (int i = 0; i < 1024; i++) src[i] = (int16_t)(rand() % 65536 - 32768);
    int r0 = resample_max_reach(kind) + 1;
    resample_block(kind, src, 1, r0, 1.0, out, 512);
    float err = 0;
    for (int k = 0; k < 512; k++) err = fmaxf(err, fabsf(out[k] - src[r0 + k]));
    CHECK(err < 1e-2f, "ratio 1 passes the source through (max error %.4f)", err);
}

/* ---- mixer checks (src/audio.c FIFO with the selected resampler) ---- */
typedef struct {
    int ch;
    double rate, amp, f[2];
    long pos, len, loop;   /* len > 0: the source ends after len frames; loop > 0: wraps every loop frames */
} SineSrc;

static double sine_at(const SineSrc *s, int c, double x) { return s->amp * sin(2 * PI * s->f[c] * x / s->rate); }

static int sine_fill(void *ctx, int16_t *dst, int max) {
    SineSrc *s = ctx;
    int n = max;
    if (s->len > 0 && s->pos + n > s->len) n = (int)(s->len - s->pos);
    if (s->loop > 0 && s->pos % s->loop + n > s->loop) n = (int)(s->loop - s->pos % s->loop);   /* stop at the loop end */
    for (int i = 0; i < n; i++) {
        long x = s->loop > 0 ? (s->pos + i) % s->loop : s->pos + i;
        for (int c = 0; c < s->ch; c++) dst[i * s->ch + c] = (int16_t)lrint(sine_at(s, c, (double)x));
    }
    s->pos += n;
    return n > 0 ? n : 0;
}

static int sinc_delay(void) { return kind == RESAMPLE_SINC ? RESAMPLE_SINC_DELAY : 0; }

/* Mixes chunks of 240 frames with the voice's rate set per chunk; compares each output channel
 * with the ideal sine at the accumulated source position (catches position jumps) and bounds the
 * step between output frames (catches clicks). */
static void check_stream(const char *what, SineSrc *s, const uint32_t *rates, int nrates, int chunks_per_rate) {
    AudioVoice *v = voice_new(s->ch, rates[0], sine_fill, s);
    voice_play(v, 1);
    int total = nrates * chunks_per_rate * AUDIO_CHUNK;
    int16_t *out = malloc(sizeof(int16_t) * 2 * total);
    double *x = malloc(sizeof(double) * total), pos = -sinc_delay(), fmax_out = 0;
    for (int r = 0, k = 0; r < nrates; r++) {
        voice_set_rate(v, rates[r]);
        double step = (double)rates[r] / AUDIO_RATE;
        for (int c = 0; c < s->ch; c++) fmax_out = fmax(fmax_out, s->f[c] * step * AUDIO_RATE / s->rate);
        for (int j = 0; j < chunks_per_rate; j++) {
            audio_lock();
            audio_mix(out + 2 * k, AUDIO_CHUNK);
            audio_unlock();
            for (int i = 0; i < AUDIO_CHUNK; i++, k++) { x[k] = pos; pos += step; }
        }
    }
    voice_free(v);
    double err = 0, d1 = 0, wmax = 2 * PI * fmax_out / AUDIO_RATE;
    int skip = 2 * (resample_max_reach(kind) + sinc_delay()) + 8;
    for (int k = skip; k < total; k++)
        for (int c = 0; c < 2; c++) {
            int sc = s->ch == 2 ? c : 0;
            double ideal = sine_at(s, sc, s->loop > 0 ? fmod(x[k], (double)s->loop) : x[k]);
            err = fmax(err, fabs(out[2 * k + c] - ideal));
            d1 = fmax(d1, fabs((double)out[2 * k + c] - out[2 * (k - 1) + c]));
        }
    double d1_bound = 1.05 * s->amp * wmax + 4;   /* the steepest slope of the fastest tone */
    /* A position jump of one source frame would be an error of 28% or more here. Linear
     * interpolation itself is off by up to (pi f / fs)^2 / 2 (4.4% at 2.5 kHz from 26.5 kHz). */
    double tol = kind == RESAMPLE_SINC ? 0.005 : 0.06;
    CHECK(err <= tol * s->amp, "%s: output follows the source position (max error %.1f, %.2f%% of the amplitude)", what, err,
          100 * err / s->amp);
    CHECK(d1 <= d1_bound, "%s: no clicks (largest step between output frames %.1f, bound %.1f)", what, d1, d1_bound);
    free(out);
    free(x);
}

static void test_mixer(void) {
    /* Ratio changes mid-stream across step 1 (64 kHz is faster than the output), from a looping
     * mono source (441 frames = 20 cycles of 1 kHz at 22050 Hz, so the loop is seamless). */
    static const uint32_t rates[] = {22050, 24000, 20000, 44100, 64000, 30000, 22050};
    SineSrc loop = {1, 22050, 12000, {1000, 0}, 0, 0, 441};
    check_stream("mono loop, rate changes 22050..64000 Hz", &loop, rates, 7, 4);
    /* Stereo music rate with a short loop (53 frames: 2 cycles of 1 kHz, 5 of 2.5 kHz). */
    static const uint32_t music[] = {26500, 27000, 26500};
    SineSrc st = {2, 26500, 10000, {1000, 2500}, 0, 0, 53};
    check_stream("stereo loop at 26500 Hz", &st, music, 3, 6);
}

/* Constant source: lead frames of silence, then value, left frames in all, then the end. */
typedef struct { int16_t value; int left, lead; } ConstSrc;
static int const_fill(void *ctx, int16_t *dst, int max) {
    ConstSrc *c = ctx;
    int n = c->left < max ? c->left : max;
    for (int i = 0; i < n; i++, c->lead--) dst[i] = c->lead > 0 ? 0 : c->value;
    c->left -= n;
    return n;
}

static void test_edges(void) {
    /* Voice start: a constant source comes out exact from the first frame (the history before
     * the first source frame repeats it), and the source is read as far as with linear. */
    ConstSrc c = {1000, 1000, 0};
    AudioVoice *v = voice_new(1, 24000, const_fill, &c);
    voice_play(v, 1);
    int16_t out[480 * 2];
    audio_lock();
    audio_mix(out, 480);
    audio_unlock();
    int exact = 1;
    for (int i = 0; i < 960; i++) exact &= out[i] == 1000;
    CHECK(exact, "voice start: a constant source is exact from the first output frame (%d %d .. %d)", out[0], out[1], out[959]);
    CHECK(c.left == 1000 - 241, "voice start: 480 output frames read 241 source frames, as linear does (%d left)", c.left);
    voice_free(v);

    /* Voice end: 900 frames at 22050 Hz (after 100 of silence, so the start does not count the
     * repeated first frame) last 900 / step output frames, then the voice stops. */
    ConstSrc e = {10000, 1000, 100};
    v = voice_new(1, 22050, const_fill, &e);
    voice_play(v, 1);
    int16_t end[3000 * 2];
    audio_lock();
    audio_mix(end, 3000);
    audio_unlock();
    int above = 0, peak = 0, tail = 0;
    for (int k = 0; k < 3000; k++) {
        above += end[2 * k] > 5000;
        peak = abs(end[2 * k]) > peak ? abs(end[2 * k]) : peak;
        if (k >= 3000 - 300) tail |= end[2 * k] | end[2 * k + 1];
    }
    double want = 900 / (22050.0 / AUDIO_RATE);
    CHECK(fabs(above - want) <= 3, "voice end: the sound lasts %d output frames (source length %.1f)", above, want);
    /* A band-limited 0 -> full step read between its frames rings by up to about 14%. */
    CHECK(peak <= 11500, "voice start/end: the ringing at the edges stays bounded (peak %d for 10000)", peak);
    CHECK(!voice_playing(v) && !tail, "voice end: the voice stops and the output is silent after it");
    e.left = 500;   /* the source restarts (as after a seek), at full level */
    voice_play(v, 1);
    audio_lock();
    audio_mix(end, 480);
    audio_unlock();
    CHECK(end[0] == 10000 && end[2] == 10000, "voice replay after its end starts clean (%d %d)", end[0], end[2]);
    voice_free(v);

    /* Voice start with a sine: the band-limited start does not overshoot the amplitude. */
    SineSrc s = {1, 22050, 12000, {3000, 0}, 0, 0, 0};
    v = voice_new(1, 22050, sine_fill, &s);
    voice_play(v, 1);
    audio_lock();
    audio_mix(out, 480);
    audio_unlock();
    peak = 0;
    for (int i = 0; i < 960; i++) peak = abs(out[i]) > peak ? abs(out[i]) : peak;
    CHECK(peak <= 12600, "voice start: a sine from phase 0 peaks at %d (amplitude 12000)", peak);
    voice_free(v);
}

/* ---- cost ---- */
static double cost_us_per_second(int ch, double rate) {
    double step = rate / AUDIO_RATE;
    int reach = resample_max_reach(kind) + 2, len = reach * 2 + (int)(AUDIO_RATE * step) + 16;
    int16_t *src = calloc((size_t)len * ch, sizeof(int16_t));
    for (int i = 0; i < len * ch; i++) src[i] = (int16_t)(rand() % 20000 - 10000);
    float out[AUDIO_CHUNK * 2];
    double best = 1e30, sink = 0;
    for (int rep = 0; rep < 5; rep++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int k = 0; k + AUDIO_CHUNK <= AUDIO_RATE; k += AUDIO_CHUNK) {
            resample_block(kind, src, ch, reach + k * step, step, out, AUDIO_CHUNK);
            sink += out[0];
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
        best = us < best ? us : best;
    }
    free(src);
    if (sink == 12345.678) printf(" ");
    return best;
}

static void report_cost(void) {
    double mono = cost_us_per_second(1, 22050), stereo = cost_us_per_second(2, 26500), fast = cost_us_per_second(1, 64000);
    printf("info cost (%s): %.0f us per second of output for a mono 22050 Hz voice (%.3f%% of a core), stereo 26500 Hz %.0f us "
           "(%.3f%%), mono 64000 Hz %.0f us; 61 mono voices %.1f%% of a core\n",
           resample_name(kind), mono, mono / 1e4, stereo, stereo / 1e4, fast, 61 * mono / 1e4);
}

int main(int argc, char **argv) {
    if (argc > 1 && resample_parse(argv[1], &kind)) { fprintf(stderr, "usage: %s [sinc|linear]\n", argv[0]); return 2; }
    audio_set_resampler(kind);
    printf("resampler: %s\n", resample_name(kind));
    test_spectral();
    test_mixer();
    test_edges();
    report_cost();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
