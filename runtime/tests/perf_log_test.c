/* Game-independent test of the always-on performance log (src/perf_log.c): the hitch threshold,
 * the rate limit (one line per second, the suppressed count, 200 lines per session), the exact
 * [hitch] and [level-summary] line formats with unmeasured fields as `?`, and the summary math
 * (average, 1% low from the fixed histogram, worst, cpu, gpu, vram peak, hitches, tex misses,
 * loading frames left out). */
#include "../src/perf_log.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static PerfFrame frame(double ms) {
    PerfFrame f = {ms, -1, -1, -1, -1, -1, -1, -1, 0, -1, -1};
    return f;
}

int main(void) {
    char line[512];

    /* Threshold. */
    CHECK(!perf_is_hitch(16.7) && !perf_is_hitch(25.0) && perf_is_hitch(25.01) && perf_is_hitch(400), "hitch: longer than 25 ms");

    /* Rate limit: one line per second, the suppressed ones reported before the next line. */
    PerfHitchGate g = {0};
    uint32_t flush = 99;
    CHECK(perf_hitch_gate(&g, 5000, &flush) && flush == 0, "first hitch printed, nothing to flush");
    CHECK(!perf_hitch_gate(&g, 5100, &flush) && !perf_hitch_gate(&g, 5999, &flush), "hitches within the second suppressed");
    CHECK(perf_hitch_gate(&g, 6000, &flush) && flush == 2, "a second later: printed after `2 more suppressed` (flush %u)", flush);
    CHECK(g.lines == 3, "lines counted: hitch, suppressed, hitch (%u)", g.lines);
    CHECK(perf_hitch_flush(&g) == 0, "flush with nothing suppressed: no line");
    perf_hitch_gate(&g, 6500, &flush);
    CHECK(perf_hitch_flush(&g) == 1 && g.lines == 4 && perf_hitch_flush(&g) == 0, "summary flush reports the pending count once");
    CHECK(perf_hitch_gate(&g, 6600, &flush) == 0, "the flush does not restart the one-second window");

    /* Session cap: 200 lines of both kinds. */
    PerfHitchGate c = {0};
    uint32_t printed = 0, flushed = 0;
    for (int i = 0; i < 1000; i++) {
        if (perf_hitch_gate(&c, i * 500.0, &flush)) printed++;
        if (flush) flushed++;
    }
    CHECK(printed + flushed == PERF_HITCH_MAX_LINES && c.lines == PERF_HITCH_MAX_LINES, "at most 200 lines per session (%u hitch + %u suppressed)",
          printed, flushed);
    CHECK(!perf_hitch_gate(&c, 1e9, &flush) && flush == 0 && perf_hitch_flush(&c) == 0, "after the cap: nothing more");
    PerfHitchGate e = {0};
    e.lines = PERF_HITCH_MAX_LINES - 1;
    e.any = 1;
    e.suppressed = 3;
    CHECK(!perf_hitch_gate(&e, 1e6, &flush) && flush == 3 && e.lines == PERF_HITCH_MAX_LINES, "last slot: the suppressed line, not the hitch");

    /* Line formats. */
    PerfFrame f = {41.25, 12.0, 3.46, 27.5, 8.04, 0, 19.95, 30, 0, 0.5, 2.25};
    perf_hitch_line(line, sizeof line, 1234, &f, "level51");
    CHECK(!strcmp(line, "[hitch] frame 1234: 41.2 ms (game 12.0, hle 3.5, render 27.5, gpu 8.0, tex-upload 0.0, pipeline-compile 19.9 ms, "
                        "pace 0.5, wait 2.2, other 1.2 ms) level level51"),
          "hitch line: %s", line);
    /* A late pacing wake-up at light load: other = interval - game - render - pace. */
    f = (PerfFrame){29.3, 1.8, -1, 1.0, 3.4, 0, 0, 2, 0, 25.9, 0.2};
    perf_hitch_line(line, sizeof line, 271, &f, "");
    CHECK(!strcmp(line, "[hitch] frame 271: 29.3 ms (game 1.8, hle ?, render 1.0, gpu 3.4, tex-upload 0.0, pipeline-compile 0.0 ms, "
                        "pace 25.9, wait 0.2, other 0.6 ms) level ?"),
          "pace and other: %s", line);
    /* Unmeasured time is never negative (the guest's and the host's parts of one interval). */
    f = (PerfFrame){26, 20, -1, 10, -1, 0, 0, 2, 0, -1, -1};
    perf_hitch_line(line, sizeof line, 9, &f, "level1");
    CHECK(strstr(line, "pace ?, wait ?, other 0.0 ms) level level1") != NULL, "other clamped at 0, unmeasured pace and wait: %s", line);
    f = frame(250);
    f.loading = 1;
    perf_hitch_line(line, sizeof line, 7, &f, NULL);
    CHECK(!strcmp(line, "[hitch] frame 7: 250.0 ms (game ?, hle ?, render ?, gpu ?, tex-upload ?, pipeline-compile ? ms, pace ?, wait ?, other ? ms) "
                        "level ? (loading)"),
          "unmeasured fields and loading: %s", line);
    perf_suppressed_line(line, sizeof line, 12);
    CHECK(!strcmp(line, "[hitch] 12 more suppressed"), "suppressed line: %s", line);

    /* Summary math: 990 frames of 16 ms and 10 of 40 ms: the 99th percentile is 16 ms. */
    static PerfLevel l;
    perf_level_reset(&l);
    perf_level_line(line, sizeof line, &l, "");
    CHECK(!strcmp(line, "[level-summary] level ?: 0 frames, avg ? fps, 1% low ? fps, worst ? ms, cpu ? ms/frame, gpu ? ms/frame, vram peak ? MiB, "
                        "hitches 0, tex misses ?"),
          "empty summary: %s", line);
    for (int i = 0; i < 990; i++) {
        PerfFrame k = frame(16);
        k.cpu = 10;
        k.gpu = i < 495 ? 4 : 6;
        perf_level_frame(&l, &k);
    }
    for (int i = 0; i < 10; i++) {
        PerfFrame k = frame(40);
        k.cpu = 30;
        perf_level_frame(&l, &k);
    }
    PerfFrame ld = frame(3000);
    ld.loading = 1;
    ld.cpu = 3000;
    perf_level_frame(&l, &ld);
    CHECK(l.frames == 1000 && l.loading_frames == 1 && l.hitches == 10, "loading frame left out, 10 hitches (%u frames, %u hitches)", l.frames,
          l.hitches);
    CHECK(perf_level_p99_ms(&l) == 16.0, "p99 16 ms (%.2f)", perf_level_p99_ms(&l));
    double avg = perf_level_avg_fps(&l);
    CHECK(avg > 61.57 && avg < 61.58, "avg fps 1000 / 16.24 = %.3f", avg);
    /* 985 frames of 16 ms, 15 of 40: the 99th percentile frame (the 990th) is 40 ms. */
    static PerfLevel m;
    perf_level_reset(&m);
    for (int i = 0; i < 985; i++) { PerfFrame k = frame(16); perf_level_frame(&m, &k); }
    for (int i = 0; i < 15; i++) { PerfFrame k = frame(40); perf_level_frame(&m, &k); }
    CHECK(perf_level_p99_ms(&m) == 40.0 && perf_level_low1_fps(&m) == 25.0, "p99 40 ms -> 1%% low 25 fps (%.2f)", perf_level_p99_ms(&m));
    /* Histogram range: a frame past 256 ms is the worst frame, not the bin edge. */
    static PerfLevel o;
    perf_level_reset(&o);
    for (int i = 0; i < 50; i++) { PerfFrame k = frame(i < 49 ? 16 : 900); perf_level_frame(&o, &k); }
    CHECK(perf_level_p99_ms(&o) == 256.0 && o.worst_ms == 900, "over the range: at least 256, worst 900 (%.1f)", perf_level_p99_ms(&o));
    static PerfLevel q;
    perf_level_reset(&q);
    for (int i = 0; i < 10; i++) { PerfFrame k = frame(i < 9 ? 16 : 33.1); perf_level_frame(&q, &k); }
    CHECK(perf_level_p99_ms(&q) == 33.0, "p99 at the bin's lower edge, 0.25 ms bins (%.2f)", perf_level_p99_ms(&q));

    perf_level_vram(&l, 900ull << 20);
    perf_level_vram(&l, 1500ull << 20);
    perf_level_vram(&l, 1200ull << 20);
    perf_level_tex_misses(&l, 5, 1);
    perf_level_tex_misses(&l, 2, 1);
    perf_level_line(line, sizeof line, &l, "level81");
    CHECK(!strcmp(line, "[level-summary] level level81: 1000 frames, avg 61.6 fps, 1% low 62.5 fps, worst 40.0 ms, cpu 10.2 ms/frame, "
                        "gpu 5.0 ms/frame, vram peak 1500 MiB, hitches 10, tex misses 7"),
          "summary line: %s", line);
    perf_level_reset(&l);
    CHECK(l.frames == 0 && l.hitches == 0 && !l.vram_known && l.tex_misses == 0 && l.hist[64] == 0, "reset clears the level");

    printf("%s\n", failures ? "FAILED" : "all perf log tests passed");
    return failures != 0;
}
