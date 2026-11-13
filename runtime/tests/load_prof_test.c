/* Game-independent test of --load-profile (src/load_prof.c): read classification by host path,
 * the process-wide counters, the load window (opens at the request, ignores loading screens,
 * menus and movie frames, closes at the first frame with the level's draws), the exact [load] line
 * with only the counters added inside the window, unmeasured fields as `?`, and the timeout. */
#include "../src/load_prof.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static LoadFrame frame(double t, double cpu, double ms, uint32_t draws) {
    LoadFrame f = {t, cpu, ms, 4.0, 2.0, 0.0, 0.0, draws, 0};
    return f;
}

int main(void) {
    char line[1024];
    uint64_t c[LOAD_COUNTERS][3];

    /* Read classification. */
    CHECK(load_prof_read_kind("/g/disc/Main.wad2") == LOAD_READ_DATA, "archive read: data");
    CHECK(load_prof_read_kind("/g/disc/XBoxWad.idx") == LOAD_READ_DATA, "index read: data");
    CHECK(load_prof_read_kind("/g/disc/audio/ambience.xwb") == LOAD_READ_AUDIO, "wave bank in the audio folder: audio");
    CHECK(load_prof_read_kind("/g/disc/Sound.XWB") == LOAD_READ_AUDIO, "wave bank by extension, any case: audio");
    CHECK(load_prof_read_kind("C:\\g\\disc\\audio\\x.xsb") == LOAD_READ_AUDIO, "Windows path: audio");
    CHECK(load_prof_read_kind("/g/disc/bik/l99_in.bik") == LOAD_READ_MOVIE, "movie");
    CHECK(load_prof_read_kind(NULL) == LOAD_READ_DATA, "no path: data");

    /* Counters. */
    load_prof_snapshot(c);
    CHECK(c[LOAD_READ_DATA][0] == 0 && c[LOAD_PACK_READ][2] == 0, "counters start at 0");
    load_prof_count(LOAD_READ_DATA, 1000, 2000000);
    load_prof_count(LOAD_READ_DATA, 3000, 1000000);
    load_prof_count(LOAD_COUNTERS, 5, 5);   /* out of range: ignored */
    load_prof_count(-1, 5, 5);
    load_prof_snapshot(c);
    CHECK(c[LOAD_READ_DATA][0] == 2 && c[LOAD_READ_DATA][1] == 4000 && c[LOAD_READ_DATA][2] == 3000000,
          "calls, bytes and ns add up (%llu, %llu, %llu)", (unsigned long long)c[LOAD_READ_DATA][0],
          (unsigned long long)c[LOAD_READ_DATA][1], (unsigned long long)c[LOAD_READ_DATA][2]);

    /* Window: nothing before it opens. */
    LoadWindow w = {0};
    LoadFrame f = frame(500, 100, 16, 300);
    CHECK(load_window_frame(&w, &f, c, line, sizeof line) == 0, "no window: no line");

    /* Open at t 1000 with the counters so far; they must not be reported. */
    load_window_begin(&w, "level87", 1000, 500, c);
    CHECK(w.open, "window open after the request");
    int closed = 0;
    f = frame(1016, 510, 16, 42);   /* the menu's last frame */
    closed |= load_window_frame(&w, &f, c, line, sizeof line);
    f = frame(1300, 700, 284, 2);   /* loading screen, long frame */
    closed |= load_window_frame(&w, &f, c, line, sizeof line);
    load_prof_count(LOAD_READ_DATA, 13 << 20, 46000000);
    load_prof_count(LOAD_READ_DATA, 1 << 20, 4000000);
    load_prof_count(LOAD_READ_AUDIO, 1 << 19, 1000000);
    load_prof_count(LOAD_PACK_READ, 2 << 20, 12000000);
    load_prof_snapshot(c);
    f = frame(1600, 900, 300, 1);
    f.movie = 1;   /* a movie frame with the level behind: does not close */
    f.draws = 300;
    closed |= load_window_frame(&w, &f, c, line, sizeof line);
    CHECK(!closed && w.open, "menu, loading screen and movie frames keep the window open");
    f = frame(2182, 1300, 582, LOAD_PROF_LEVEL_DRAWS);
    f.upload_ms = 195;
    f.pack_ms = 12;
    f.pipeline_ms = 17;
    f.guest_ms = 3;
    CHECK(load_window_frame(&w, &f, c, line, sizeof line) == 1 && !w.open, "first frame with the level's draws closes it");
    const char *want = "[load] level87: 1182 ms to the first level frame (4 frames, longest 582 ms) | guest frames 15 ms | "
                       "file reads 2 (14.0 MB, 50 ms; audio 1 (0.5 MB, 1 ms), movies 0 (0.0 MB, 0 ms)) | "
                       "texture creation 201 ms (pack 12 ms) | pipeline builds 17 ms | pack loader reads 1 (2.0 MB, 12 ms) | "
                       "process CPU 800 ms";
    CHECK(!strcmp(line, want), "line:\n     got  %s\n     want %s", line, want);
    f = frame(2200, 1310, 18, 300);
    CHECK(load_window_frame(&w, &f, c, line, sizeof line) == 0, "closed: later frames add nothing");

    /* Unmeasured renderer fields read `?`; a restart takes the newer request. */
    load_window_begin(&w, "level95", 3000, 2000, c);
    load_window_begin(&w, "level99", 3010, 2001, c);
    f = frame(3500, 2100, 490, 150);
    f.upload_ms = f.pipeline_ms = f.guest_ms = -1;
    f.pack_ms = 0;
    CHECK(load_window_frame(&w, &f, c, line, sizeof line) == 1, "renderer off: closes");
    CHECK(strstr(line, "[load] level99: 490 ms") && strstr(line, "guest frames ? |") && strstr(line, "texture creation ? |") &&
              strstr(line, "pipeline builds ? |"),
          "restart keeps the newer level; unmeasured as `?`: %s", line);

    /* Timeout. */
    load_window_begin(&w, "level2", 10000, 0, c);
    f = frame(10000 + LOAD_PROF_TIMEOUT_MS - 1, 0, 16, 2);
    CHECK(load_window_frame(&w, &f, c, line, sizeof line) == 0, "just inside the timeout: open");
    f = frame(10000 + LOAD_PROF_TIMEOUT_MS + 1, 0, 16, 2);
    CHECK(load_window_frame(&w, &f, c, line, sizeof line) == 1 && !w.open, "past the timeout: closes");
    CHECK(!strcmp(line, "[load] level2: no level frame within 180 s (2 frames)"), "timeout line: %s", line);

    /* A long level name is cut, never overflows. */
    load_window_begin(&w, "a_level_name_far_longer_than_thirty_one_characters", 0, 0, c);
    CHECK(strlen(w.level) == LOAD_PROF_LEVEL_CHARS - 1, "level name cut to %d characters", LOAD_PROF_LEVEL_CHARS - 1);

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
