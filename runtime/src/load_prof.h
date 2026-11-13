/* --load-profile: where the time goes when a level is entered. Pure
 * logic: no clocks, no file or renderer calls; tested by tests/load_prof_test.c.
 *
 * A window opens at the level load request (the MenuLoadLevel site, hle_warp.c) and closes at
 * the first presented frame of the level: at least LOAD_PROF_LEVEL_DRAWS draws and no movie quad
 * (loading screens draw 1-2 quads, menus about 40, a level a few hundred). One line then:
 *
 *   [load] level87: 1182 ms to the first level frame (39 frames, longest 594 ms) | guest frames 310 ms |
 *          file reads 58 (13.9 MB, 46 ms; audio 0 (0.0 MB, 0 ms), movies 0 (0.0 MB, 0 ms)) |
 *          texture creation 212 ms (pack 12 ms) | pipeline builds 17 ms | pack loader reads 3 (0.4 MB, 12 ms) |
 *          process CPU 800 ms
 *
 * Counters (load_prof_count) are added from any thread; their times are summed over threads, so
 * they can overlap each other and exceed the wall time. `guest frames`: the guest's own frame time
 * (game logic, its decompression and parsing, its waits for file reads) without the 60 Hz pacing
 * sleep. `texture creation`, `pipeline builds`: render thread. A window still open after
 * LOAD_PROF_TIMEOUT_MS reports `no level frame within ...` and closes. */
#ifndef LOAD_PROF_H
#define LOAD_PROF_H
#include <stddef.h>
#include <stdint.h>

#define LOAD_PROF_LEVEL_DRAWS 100u
#define LOAD_PROF_TIMEOUT_MS 180000.0
#define LOAD_PROF_LEVEL_CHARS 32

enum {
    LOAD_READ_DATA,    /* guest NtReadFile: archives and everything else */
    LOAD_READ_AUDIO,   /* guest NtReadFile of audio files (.xwb .xsb .xgs .wav, an audio folder) */
    LOAD_READ_MOVIE,   /* guest NtReadFile of .bik movies */
    LOAD_PACK_READ,    /* texture pack loader threads: read and decode of a replacement */
    LOAD_COUNTERS
};

/* Process-wide counters: calls, bytes, nanoseconds (atomic adds). */
extern int load_prof_on;
void load_prof_count(int counter, uint64_t bytes, uint64_t ns);
void load_prof_snapshot(uint64_t out[LOAD_COUNTERS][3]);
/* The counter of a guest file read by its host path. */
int load_prof_read_kind(const char *host_path);

typedef struct {
    double t_ms, cpu_ms;   /* now: wall clock and process CPU time */
    double frame_ms;       /* interval since the previous presented frame */
    double guest_ms;       /* guest frame time without pacing (< 0: not measured) */
    double upload_ms;      /* renderer texture creation in this frame (< 0: not measured) */
    double pack_ms;        /* of it, texture pack replacements */
    double pipeline_ms;    /* pipeline builds in this frame (< 0: not measured) */
    uint32_t draws;
    int movie;             /* a movie quad was drawn */
} LoadFrame;

typedef struct {
    int open;
    char level[LOAD_PROF_LEVEL_CHARS];
    double t0, cpu0, longest, guest_ms, upload_ms, pack_ms, pipeline_ms;
    uint32_t frames;
    uint64_t c0[LOAD_COUNTERS][3];
} LoadWindow;

/* Opens (or restarts) the window; c0 = load_prof_snapshot at the request. */
void load_window_begin(LoadWindow *w, const char *level, double t_ms, double cpu_ms, const uint64_t c0[LOAD_COUNTERS][3]);
/* One presented frame while the window is open; c = load_prof_snapshot now. Returns 1 and writes
 * the line (without a newline) when the window closes, else 0. */
int load_window_frame(LoadWindow *w, const LoadFrame *f, const uint64_t c[LOAD_COUNTERS][3], char *line, size_t len);

#endif
