/* Game-independent test of --frame-gen's pure part (src/framegen.c): the options, the display rates
 * that pace 120 frames a second, the presentation decision with its [frame-gen] line, and the
 * slow-source guard (off after 2 s under 60 fps, back after 2 s at 60, pauses ignored, the resume hold
 * doubling with every pause) and the display clock measured from present waits. */
#include "../src/framegen.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    FrameGenOptions o = {0, 0};
    CHECK(framegen_parse_arg("--frame-gen", &o) && o.on == 1 && o.of == 0, "--frame-gen turns it on");
    CHECK(framegen_parse_arg("--frame-gen-of", &o) && o.on == 1 && o.of == 1, "--frame-gen-of turns it on with optical flow");
    CHECK(framegen_parse_arg("--no-frame-gen", &o) && o.on == 0 && o.of == 0, "--no-frame-gen turns both off");
    CHECK(!framegen_parse_arg("--frame-gen=1", &o) && !framegen_parse_arg("--frame", &o) && o.on == 0, "other arguments are not taken");

    CHECK(framegen_rate_ok(120) && framegen_rate_ok(119) && framegen_rate_ok(121) && framegen_rate_ok(240) && framegen_rate_ok(239) &&
              framegen_rate_ok(360),
          "120, 119 (119.88), 121, 240, 239, 360 pace 120 fps");
    CHECK(!framegen_rate_ok(60) && !framegen_rate_ok(144) && !framegen_rate_ok(165) && !framegen_rate_ok(180) && !framegen_rate_ok(100) &&
              !framegen_rate_ok(0) && !framegen_rate_ok(-120) && !framegen_rate_ok(118) && !framegen_rate_ok(122),
          "60, 144, 165, 180, 100, 118, 122 and unknown do not");

    char line[200];
    FrameGenOptions on = {1, 0}, off = {0, 0}, of = {1, 1};
    CHECK(framegen_decide(&off, 1, 1, 1, 120, line, sizeof line) == FRAMEGEN_OFF && !strcmp(line, "[frame-gen] off"), "off: %s", line);
    CHECK(framegen_decide(&on, 0, 1, 1, 120, line, sizeof line) == FRAMEGEN_UNAVAILABLE && strstr(line, "unavailable"), "unsupported: %s", line);
    CHECK(framegen_decide(&on, 1, 1, 0, 120, line, sizeof line) == FRAMEGEN_NO_SIM60 && strstr(line, "--sim60"), "no --sim60: %s", line);
    CHECK(framegen_decide(&on, 1, 0, 1, 0, line, sizeof line) == FRAMEGEN_READBACK && strstr(line, "built"), "readback: %s", line);
    CHECK(framegen_decide(&on, 1, 1, 1, 144, line, sizeof line) == FRAMEGEN_RATE && strstr(line, "144 Hz"), "144 Hz: %s", line);
    CHECK(framegen_decide(&on, 1, 1, 1, 60, line, sizeof line) == FRAMEGEN_RATE && strstr(line, "60 Hz"), "60 Hz: %s", line);
    CHECK(framegen_decide(&on, 1, 1, 1, 0, line, sizeof line) == FRAMEGEN_RATE && strstr(line, "no refresh rate"), "unknown rate: %s", line);
    CHECK(framegen_decide(&on, 1, 1, 1, 120, line, sizeof line) == FRAMEGEN_SHOW && !strcmp(line, "[frame-gen] on: 120 frames a second on the 120 Hz display"),
          "120 Hz: %s", line);
    CHECK(framegen_decide(&of, 1, 1, 1, 240, line, sizeof line) == FRAMEGEN_SHOW && strstr(line, "with optical flow") && strstr(line, "240 Hz"),
          "240 Hz with optical flow: %s", line);
    CHECK(framegen_decide(&on, 1, 1, 1, 120, NULL, 0) == FRAMEGEN_SHOW, "no line buffer");

    /* Full-screen draws (the title's tint and feedback quads) vs HUD pieces. */
    typedef struct { float x, y, z, w; unsigned color; } V;
    const V full[4] = {{-1, 1, 0, 1, 0}, {1, 1, 0, 1, 0}, {1, -1, 0, 1, 0}, {-1, -1, 0, 1, 0}};
    const V full_w2[4] = {{-2, 2, 0, 2, 0}, {2, 2, 0, 2, 0}, {2, -2, 0, 2, 0}, {-2, -2, 0, 2, 0}};
    const V tri[3] = {{-1, -1, 0, 1, 0}, {3, -1, 0, 1, 0}, {-1, 3, 0, 1, 0}};
    const V bar[4] = {{-1, 1, 0, 1, 0}, {1, 1, 0, 1, 0}, {1, 0.75f, 0, 1, 0}, {-1, 0.75f, 0, 1, 0}};
    const V hud[4] = {{-0.9f, 0.9f, 0, 1, 0}, {0.9f, 0.9f, 0, 1, 0}, {0.9f, -0.9f, 0, 1, 0}, {-0.9f, -0.9f, 0, 1, 0}};
    const V behind[4] = {{-1, 1, 0, 1, 0}, {1, 1, 0, 1, 0}, {1, -1, 0, -1, 0}, {-1, -1, 0, 1, 0}};
    CHECK(framegen_full_screen(full, sizeof(V), 4) && framegen_full_screen(full_w2, sizeof(V), 4) && framegen_full_screen(tri, sizeof(V), 3),
          "full-screen quads (any w) and an oversized triangle cover the screen");
    CHECK(!framegen_full_screen(bar, sizeof(V), 4) && !framegen_full_screen(hud, sizeof(V), 4) && !framegen_full_screen(behind, sizeof(V), 4) &&
              !framegen_full_screen(full, sizeof(V), 2) && !framegen_full_screen(NULL, sizeof(V), 4),
          "a bar, a 90%% HUD panel, a vertex behind the camera, two vertices and none do not");

    /* Slow-source guard. */
    FrameGenPace p = {0};
    int changed = 0, flips = 0, shown = 1;
    double t = 0;
    for (; t < 10000; t += 16.667) shown &= framegen_pace(&p, 16.667, &changed), flips += changed;
    CHECK(shown && flips == 0, "60 fps for 10 s: always shown");
    double off_at = -1;
    for (t = 0; t < 4000; t += 20) {
        if (!framegen_pace(&p, 20, &changed) && off_at < 0) off_at = t;
        flips += changed;
    }
    CHECK(off_at >= 1900 && off_at <= 2600 && flips == 1, "50 fps: off after about 2 s (%.0f ms)", off_at);
    double on_at = -1;
    for (t = 0; t < 4000; t += 16.667)
        if (framegen_pace(&p, 16.667, &changed) && on_at < 0) on_at = t;
    CHECK(on_at >= 1900 && on_at <= 3000, "back at 60 fps: on again after about 2 s (%.0f ms)", on_at);
    FrameGenPace q = {0};
    int all = 1;
    for (int i = 0; i < 600; i++) all &= framegen_pace(&q, i % 60 == 59 ? 500.0 : 16.667, &changed);   /* a 0.5 s hitch every second */
    CHECK(all, "pauses (a load, a hitch) do not count as a slow source");
    FrameGenPace r = {0};
    int mixed = 1;
    for (int i = 0; i < 600; i++) mixed &= framegen_pace(&r, i % 2 ? 16.0 : 17.5, &changed);   /* 60 fps with jitter */
    CHECK(mixed, "60 fps with timer jitter (16-17.5 ms) stays on");
    CHECK(framegen_pace(&r, 0, &changed) && !changed && framegen_pace(&r, -3, NULL), "zero and negative intervals are ignored");

    /* The guard's resume hold doubles with every pause: a scene that holds 60 fps only without the
     * generated frames' cost (50 fps with them, 60 without: the user's level95, 13 flips) settles. */
    CHECK(framegen_resume_hold(0) == 2000 && framegen_resume_hold(1) == 2000 && framegen_resume_hold(2) == 4000 &&
              framegen_resume_hold(3) == 8000 && framegen_resume_hold(4) == 16000 && framegen_resume_hold(5) == 32000 &&
              framegen_resume_hold(40) == 32000,
          "resume hold: 2 s after the first pause, then 4, 8, 16, at most 32 s");
    {
        FrameGenPace g = {0};
        int fl = 0, on = 1, ch = 0;
        double last_flip = 0, gaps[8] = {0};
        int ng = 0;
        for (double tt = 0; tt < 90000;) {
            const double iv = on ? 20.0 : 16.667;   /* slow while generated frames are shown */
            on = framegen_pace(&g, iv, &ch);
            tt += iv;
            if (ch) {
                if (on && ng < 8) gaps[ng++] = tt - last_flip;   /* paused for this long */
                fl++;
                last_flip = tt;
            }
        }
        int longer = ng >= 4;
        for (int i = 1; i < ng; i++) longer &= gaps[i] >= 1.8 * gaps[i - 1];
        CHECK(fl <= 12 && longer, "a source slow only with generated frames: %d flips in 90 s (a flip every 2 s before), pauses of %.0f, %.0f, %.0f, %.0f ms",
              fl, gaps[0], gaps[1], gaps[2], gaps[3]);
        FrameGenPace k = {0};
        for (double tt = 0; tt < 3000; tt += 20) framegen_pace(&k, 20, NULL);
        for (double tt = 0; tt < 2500; tt += 16.667) framegen_pace(&k, 16.667, NULL);
        for (double tt = 0; tt < 3000; tt += 20) framegen_pace(&k, 20, NULL);
        const int two = k.pauses, paused = !framegen_pace(&k, 20, NULL);
        for (double tt = 0; tt < 70000; tt += 16.667) framegen_pace(&k, 16.667, NULL);
        CHECK(two == 2 && paused && k.pauses == 0 && !k.slow, "a minute shown in a row forgets the pauses (%d before, %d after)", two, k.pauses);
    }

    /* Display clock from present-wait returns: a 119.9 Hz display (two refreshes 16.681 ms), the wake-up
     * 0-0.6 ms late, every fifth wait already done (no sample), a frame missed now and then. */
    {
        FrameGenClock c = {0};
        const double pair = 2000.0 / 119.9;
        unsigned seed = 12345;
        double locked_at = -1, t_disp = 1000;
        int samples = 0;
        for (int i = 0; i < 2000; i++) {
            t_disp += pair * (i % 97 == 96 ? 2 : 1);
            seed = seed * 1103515245u + 12345u;
            const double jit = (double)(seed >> 16 & 0x7fff) / 32768.0 * 0.6;
            framegen_clock_add(&c, t_disp + jit, i % 5 != 4);
            samples++;
            if (locked_at < 0 && framegen_clock_period(&c) > 0) locked_at = samples;
        }
        const double got = framegen_clock_period(&c);
        CHECK(got > pair - 0.02 && got < pair + 0.02 && locked_at >= FRAMEGEN_CLOCK_LOCK && locked_at < 120,
              "display clock: 119.9 Hz measured as %.4f ms between real frames (truth %.4f), reported after %.0f samples", got, pair, locked_at);
        FrameGenClock d = {0};
        for (int i = 0; i < 500; i++) framegen_clock_add(&d, 1000 + i * 2000.0 / 144.0, 1);
        CHECK(framegen_clock_period(&d) == 0, "a 144 Hz pair (13.9 ms) is not taken for a 120 Hz display");
        FrameGenClock e = {0};
        for (int i = 0; i < 500; i++) framegen_clock_add(&e, 1000 + i * 1000.0 / 60.0, 0);
        CHECK(framegen_clock_period(&e) == 0, "waits that never waited measure nothing");
    }

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
