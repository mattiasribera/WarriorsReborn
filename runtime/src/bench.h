/* Benchmark mode (--bench [--bench-out FILE]): the pure part.
 *
 * --bench plays a fixed sequence with no input from the player: the menus are pressed through as
 * the headless runs do (start, start, A x3: load the profile, or start a new game on a fresh save),
 * --warp 7 sends the first level load to mission 7's street (rain, neon, wet ground: the most
 * demanding street; the warp blocks every save write, so the save on disk never changes), and once
 * the level has played BENCH_WARMUP_FRAMES frames the camera pans with the right stick held at
 * BENCH_PAN_STICK for BENCH_FRAMES frames (about 60 s at 60 fps). Then a short report (frame rate,
 * 1 % low, GPU time per frame and per pass, render size, GPU name, settings) goes to the file and
 * the log, and the game exits. This file holds the statistics, the scrubbed settings and the report
 * text, so they can be tested without the game. */
#ifndef BENCH_H
#define BENCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_WARP "7"
#define BENCH_INPUT "3200:start:10,3400:start:10,3600:a,3720:a,3840:a"
#define BENCH_WARMUP_FRAMES 600
#define BENCH_FRAMES 3600
#define BENCH_PAN_STICK 20000   /* about 60 %: past the title's dead zone, a slow turn */
/* No level after this many presented frames: the report says so and the game exits. */
#define BENCH_TIMEOUT_FRAMES 30000
#define BENCH_SETTINGS_CHARS 2048

typedef struct {
    int on;
    const char *out;                     /* report file (NULL: bench-report.txt in the working directory) */
    char settings[BENCH_SETTINGS_CHARS]; /* bench_settings of the command line */
} BenchOptions;
extern BenchOptions bench_options;

typedef struct {
    uint32_t frames;
    double seconds;     /* sum of the frame times */
    double avg_fps;     /* frames / seconds */
    double low1_fps;    /* 1000 / the mean of the slowest 1 % of frame times (at least one frame) */
    double worst_ms;
    double gpu_ms;      /* mean GPU time of the frames that had one (< 0: none) */
} BenchSummary;

/* Statistics of n frame times (ms; values <= 0 are skipped). Sorts a copy, never the input. */
void bench_summarize(const float *frame_ms, const float *gpu_ms, uint32_t n, BenchSummary *out);
/* The command line without file paths: every argument from the first "--" option on, a value with
 * '/' or '\\' in it written as <path>; "--bench" and "--bench-out FILE" left out. */
void bench_settings(int argc, char **argv, char *buf, size_t len);

typedef struct {
    const char *version, *gpu, *settings, *passes, *level;
    int out_w, out_h;
    int render_w_min, render_w_max, render_h_min, render_h_max;   /* over the window */
    double render_w_mean;
    int dynres;                  /* --dynres on */
    const BenchSummary *sum;     /* NULL: failed */
    const char *failure;         /* why there is no summary */
} BenchReport;
/* The report text (a few lines, ends with a newline). Returns its length (truncated to len - 1). */
size_t bench_report(char *buf, size_t len, const BenchReport *r);

#ifdef __cplusplus
}
#endif

#endif
