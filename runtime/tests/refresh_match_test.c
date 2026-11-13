/* Game-independent test of --refresh-match (src/refresh_match.c): the option pair, the
 * multiple-of-60 tolerance (59.94 and 119.88 Hz as SDL2 reports them), the target mode choice
 * (same size, highest multiple, nearest the exact rate) and the [video] line of every outcome. */
#include "../src/refresh_match.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

int main(void) {
    char line[256];
    int pick = 7;

    /* Options. */
    int on = 1;
    CHECK(refresh_match_parse_arg("--no-refresh-match", &on) && on == 0, "--no-refresh-match turns it off");
    CHECK(refresh_match_parse_arg("--refresh-match", &on) && on == 1, "--refresh-match turns it on");
    CHECK(!refresh_match_parse_arg("--refresh", &on) && !refresh_match_parse_arg("--fullscreen", &on) && on == 1,
          "other arguments are not taken");

    /* Multiples of 60 within 1 Hz. */
    CHECK(refresh_match_is_multiple(60) && refresh_match_is_multiple(59) && refresh_match_is_multiple(61), "60, 59 (59.94), 61");
    CHECK(refresh_match_is_multiple(120) && refresh_match_is_multiple(119) && refresh_match_is_multiple(240) &&
              refresh_match_is_multiple(239) && refresh_match_is_multiple(180),
          "120, 119 (119.88), 180, 240, 239");
    CHECK(!refresh_match_is_multiple(144) && !refresh_match_is_multiple(143) && !refresh_match_is_multiple(165) &&
              !refresh_match_is_multiple(75) && !refresh_match_is_multiple(100) && !refresh_match_is_multiple(50) &&
              !refresh_match_is_multiple(58) && !refresh_match_is_multiple(122),
          "144, 143, 165, 75, 100, 50, 58, 122 are not");
    CHECK(!refresh_match_is_multiple(0) && !refresh_match_is_multiple(-60) && !refresh_match_is_multiple(1), "0, negative, 1 are not");

    /* The pick: same size only, highest multiple, nearest the exact rate, first listed. */
    const RefreshMode desk = {1920, 1080, 144};
    const RefreshMode laptop[] = {{1920, 1080, 144}, {1920, 1080, 120}, {1920, 1080, 60}, {1600, 900, 144}, {1600, 900, 120}};
    CHECK(refresh_match_pick(laptop, N(laptop), &desk) == 1, "144 Hz laptop: 1920x1080 @ 120 before 60");
    const RefreshMode other_size[] = {{2560, 1440, 120}, {1920, 1080, 144}, {1920, 1080, 60}};
    CHECK(refresh_match_pick(other_size, N(other_size), &desk) == 2, "a 120 Hz mode of another size is not taken: 60");
    const RefreshMode ntsc[] = {{1920, 1080, 119}, {1920, 1080, 120}, {1920, 1080, 60}};
    CHECK(refresh_match_pick(ntsc, N(ntsc), &desk) == 1, "120 before 119 (nearest the exact multiple)");
    const RefreshMode only_ntsc[] = {{1920, 1080, 144}, {1920, 1080, 59}};
    CHECK(refresh_match_pick(only_ntsc, N(only_ntsc), &desk) == 1, "59 (59.94) counts as 60");
    const RefreshMode fast[] = {{1920, 1080, 60}, {1920, 1080, 240}, {1920, 1080, 120}};
    CHECK(refresh_match_pick(fast, N(fast), &desk) == 1, "240 before 120 and 60");
    const RefreshMode dup[] = {{1920, 1080, 120}, {1920, 1080, 120}};
    CHECK(refresh_match_pick(dup, N(dup), &desk) == 0, "equal modes (other pixel formats): the first");
    const RefreshMode none[] = {{1920, 1080, 144}, {1920, 1080, 100}, {1600, 900, 60}};
    CHECK(refresh_match_pick(none, N(none), &desk) == -1 && refresh_match_pick(NULL, 0, &desk) == -1, "no multiple of this size: -1");

    /* Decisions and their lines. */
    CHECK(refresh_match_decide(1, 1, &desk, laptop, N(laptop), &pick, line, sizeof line) == REFRESH_SWITCH && pick == 1 &&
              !strcmp(line, "[video] display 144 Hz is not a multiple of 60: switched to 1920x1080 @ 120 Hz (--no-refresh-match keeps 144)"),
          "switch: %s", line);
    pick = 7;
    CHECK(refresh_match_decide(0, 1, &desk, laptop, N(laptop), &pick, line, sizeof line) == REFRESH_OFF && pick == -1 &&
              !strcmp(line, "[video] display refresh left at 144 Hz (--no-refresh-match)"),
          "off: %s", line);
    pick = 7;
    CHECK(refresh_match_decide(1, 0, &desk, laptop, N(laptop), &pick, line, sizeof line) == REFRESH_WINDOWED && pick == -1 &&
              !strcmp(line, "[video] display refresh left at 144 Hz (windowed: --refresh-match only switches in fullscreen)"),
          "windowed: %s", line);
    const RefreshMode d120 = {1920, 1080, 120}, d59 = {1920, 1080, 59}, d0 = {1920, 1080, 0};
    pick = 7;
    CHECK(refresh_match_decide(1, 1, &d120, laptop, N(laptop), &pick, line, sizeof line) == REFRESH_ALREADY && pick == -1 &&
              !strcmp(line, "[video] display 120 Hz is a multiple of 60: refresh left as is"),
          "already 120: %s", line);
    CHECK(refresh_match_decide(1, 1, &d59, laptop, N(laptop), &pick, line, sizeof line) == REFRESH_ALREADY && pick == -1,
          "already 59 (59.94)");
    CHECK(refresh_match_decide(1, 1, &d0, laptop, N(laptop), &pick, line, sizeof line) == REFRESH_UNKNOWN && pick == -1 &&
              !strcmp(line, "[video] display refresh rate unknown: left as is"),
          "unknown: %s", line);
    CHECK(refresh_match_decide(1, 1, &desk, none, N(none), &pick, line, sizeof line) == REFRESH_NO_MODE && pick == -1 &&
              !strcmp(line, "[video] display 144 Hz is not a multiple of 60, but it has no 1920x1080 mode at a multiple of 60: "
                            "refresh left at 144 Hz (frames alternate between refresh counts)"),
          "no mode: %s", line);
    char tiny[8];
    refresh_match_decide(1, 1, &desk, laptop, N(laptop), &pick, tiny, sizeof tiny);
    CHECK(strlen(tiny) == 7, "a short buffer is truncated, terminated");

    /* The watchdog after the exclusive switch (REFRESH_MATCH_WATCH_S seconds). */
    CHECK(refresh_match_watch(0.5, 0, 0, 0) == REFRESH_WATCH_WAIT && refresh_match_watch(2.9, 5, 0, 0) == REFRESH_WATCH_WAIT,
          "within the window, not minimized: wait (frames or not)");
    CHECK(refresh_match_watch(REFRESH_MATCH_WATCH_S, 1, 0, 0) == REFRESH_WATCH_OK && refresh_match_watch(10, 300, 0, 1) == REFRESH_WATCH_OK,
          "a presented frame by the end of the window: ok");
    CHECK(refresh_match_watch(REFRESH_MATCH_WATCH_S, 0, 0, 0) == REFRESH_WATCH_NO_FRAME &&
              refresh_match_watch(4, 0, 1, 1) == REFRESH_WATCH_NO_FRAME,
          "no frame by the end of the window: no frame (even minimized after a focus loss)");
    CHECK(refresh_match_watch(0.1, 0, 1, 0) == REFRESH_WATCH_MINIMIZED && refresh_match_watch(2, 40, 1, 0) == REFRESH_WATCH_MINIMIZED,
          "minimized without a focus loss: minimized");
    CHECK(refresh_match_watch(1, 0, 1, 1) == REFRESH_WATCH_WAIT, "minimized after a focus loss (Win+D): wait");
    CHECK(!strcmp(refresh_match_watch_reason(REFRESH_WATCH_NO_FRAME), "no frame presented within 3 s of the switch") &&
              !strcmp(refresh_match_watch_reason(REFRESH_WATCH_MINIMIZED), "the window was minimized without a focus change") &&
              !strcmp(refresh_match_watch_reason(REFRESH_WATCH_OK), "ok"),
          "reasons: %s / %s", refresh_match_watch_reason(REFRESH_WATCH_NO_FRAME), refresh_match_watch_reason(REFRESH_WATCH_MINIMIZED));

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
