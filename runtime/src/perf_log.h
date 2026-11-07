/* Always-on performance log lines: pure logic, no clocks, no Vulkan, no
 * allocation; tested by tests/perf_log_test.c, used by src/hle_d3d.c on the host thread.
 *
 *   [hitch] frame F: T ms (game G, hle H, render R, gpu P, tex-upload U, pipeline-compile C ms,
 *           pace S, wait W, other O ms) level L     (one line)
 *
 * for a presented frame longer than PERF_HITCH_MS. `pace`: the guest's 60 Hz pacing sleep before
 * the frame; `wait`: the part of render blocked on the device; `other` = T - G - R - S, the time
 * no field accounts for (never below 0; `?` when G or R is not measured). A field that cannot be
 * measured reads `?`; a frame of a loading screen or a movie ends with ` (loading)`. At most one line per
 * PERF_HITCH_INTERVAL_MS (the hitches in between are counted and reported as
 * `[hitch] N more suppressed` before the next line, or when the level summary is printed) and
 * PERF_HITCH_MAX_LINES lines per session in all.
 *
 *   [level-summary] level L: N frames, avg A fps, 1% low B fps, worst W ms, cpu C ms/frame, gpu P ms/frame, vram peak V MiB, hitches H, tex misses M
 *
 * when a level loads and at exit, over the presented frames of the level that are not loading
 * frames. The 1% low is the frame rate at the 99th percentile frame time, taken from a fixed
 * histogram of PERF_HIST_BIN_MS bins (the bin's lower edge is the frame time used; frames of
 * PERF_HIST_BINS x PERF_HIST_BIN_MS or more share one overflow bin, read as that limit). */
#ifndef PERF_LOG_H
#define PERF_LOG_H
#include <stddef.h>
#include <stdint.h>

#define PERF_HITCH_MS 25.0
#define PERF_HITCH_INTERVAL_MS 1000.0
#define PERF_HITCH_MAX_LINES 200u
#define PERF_HIST_BIN_MS 0.25
#define PERF_HIST_BINS 1024   /* 0-256 ms */
#define PERF_LEVEL_CHARS 32

/* One presented frame, in milliseconds; a negative value = not measured (`?`). */
typedef struct {
    double total;     /* since the previous presented frame */
    double game;      /* the guest's frame without the 60 Hz pacing sleep (its draw recording included) */
    double hle;       /* of it, the draw recording (Direct3D HLE; measured with --stats) */
    double render;    /* host replay and renderer_end / renderer_present */
    double gpu;       /* GPU timestamps of the latest completed frame */
    double upload;    /* texture uploads (renderer_texture_create) */
    double pipeline;  /* pipelines compiled on demand */
    double cpu;       /* process CPU time since the previous presented frame */
    int loading;      /* a loading screen or a movie */
    double pace;      /* the guest's 60 Hz pacing sleep before this frame (sleep and spin) */
    double wait;      /* of render, blocked on the device: the frame slot's fence, swapchain acquire and present */
} PerfFrame;

/* Rate limit of the [hitch] lines. Zero-initialized = ready. */
typedef struct {
    double last;          /* time of the last line */
    uint32_t lines;       /* [hitch] lines printed (both kinds) */
    uint32_t suppressed;  /* hitches not printed since the last line */
    int any;
} PerfHitchGate;

/* A frame of total_ms: 1 when it is a hitch. */
int perf_is_hitch(double total_ms);
/* A hitch at now_ms (any clock in milliseconds): 1 when its line is printed. *flush is set to the
 * suppressed count to report first (`N more suppressed`, one line, counted), 0 for none. */
int perf_hitch_gate(PerfHitchGate *g, double now_ms, uint32_t *flush);
/* The suppressed count to report now (0 for none; the line is counted), e.g. at a summary. */
uint32_t perf_hitch_flush(PerfHitchGate *g);

/* Formats one value: "%.1f", or "?" when negative. Returns buf. */
const char *perf_fmt(char *buf, size_t cap, double v);
/* The `other` field: total - game - render - pace, at least 0; -1 when total, game or render
 * is not measured (an unmeasured pace counts 0). */
double perf_other_ms(const PerfFrame *f);
/* The [hitch] line (no newline); level NULL or "" = "?". Returns the length. */
int perf_hitch_line(char *buf, size_t cap, uint32_t frame, const PerfFrame *f, const char *level);
int perf_suppressed_line(char *buf, size_t cap, uint32_t n);

/* Per-level summary. */
typedef struct {
    uint32_t frames, loading_frames, hitches;
    double sum_ms, worst_ms;
    uint32_t hist[PERF_HIST_BINS + 1];   /* + the overflow bin */
    double cpu_ms;            /* over the counted frames */
    uint32_t cpu_n;
    double gpu_ms;
    uint32_t gpu_n;
    uint64_t vram_peak;       /* bytes */
    int vram_known;
    uint32_t tex_misses;
    int tex_known;            /* a texture pack is in use */
} PerfLevel;

void perf_level_reset(PerfLevel *l);
/* Adds a presented frame (loading frames are only counted as such). */
void perf_level_frame(PerfLevel *l, const PerfFrame *f);
/* A device-local memory sample (bytes in use); the peak is kept. */
void perf_level_vram(PerfLevel *l, uint64_t bytes);
/* Texture-pack misses since the last call (known: a pack is in use). */
void perf_level_tex_misses(PerfLevel *l, uint32_t misses, int known);
/* The 99th percentile frame time (ms; 0 without frames) and its frame rate. */
double perf_level_p99_ms(const PerfLevel *l);
double perf_level_low1_fps(const PerfLevel *l);
double perf_level_avg_fps(const PerfLevel *l);
/* The [level-summary] line (no newline); level NULL or "" = "?". Returns the length. */
int perf_level_line(char *buf, size_t cap, const PerfLevel *l, const char *level);

#endif
