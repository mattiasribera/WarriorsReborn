/* Game-independent test of the FPS overlay's pure parts (src/fps_overlay.c): the meter's window
 * averages, the text, the corner layout, glyph mapping and the font's ink. */
#include "../src/fps_overlay.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_meter(void) {
    FpsMeter m = {0};
    CHECK(!fps_meter_frame(&m, 1000.0), "first frame only starts the window");
    int published = 0;
    double t = 1000.0;
    for (int i = 0; i < 29; i++) published += fps_meter_frame(&m, t += 1000.0 / 60);
    CHECK(!published, "no value before 500 ms (%d)", published);
    published = fps_meter_frame(&m, t += 1000.0 / 60);   /* 30 frames in 500 ms */
    CHECK(published && m.fps > 59.9f && m.fps < 60.1f && m.ms > 16.6f && m.ms < 16.7f, "60 Hz: %.2f fps, %.2f ms", m.fps, m.ms);
    /* 20 frames of 33.3 ms: published once the window passes 500 ms (15 frames) */
    int at = 0;
    for (int i = 1; i <= 20 && !at; i++)
        if (fps_meter_frame(&m, t += 1000.0 / 30)) at = i;
    CHECK(at == 15 && m.fps > 29.9f && m.fps < 30.1f && m.ms > 33.2f && m.ms < 33.4f, "30 Hz: after %d frames %.2f fps, %.2f ms", at, m.fps,
          m.ms);
    /* a hitch: one 400 ms frame in the window lowers the average */
    fps_meter_frame(&m, t += 400);
    int p = 0;
    for (int i = 0; i < 10 && !p; i++) p = fps_meter_frame(&m, t += 1000.0 / 60);
    CHECK(p && m.fps < 30 && m.ms > 33, "a 400 ms hitch shows: %.1f fps, %.1f ms", m.fps, m.ms);
}

static void test_format(void) {
    char b[32];
    int n = fps_overlay_format(b, sizeof b, 59.96f, 16.68f);
    CHECK(!strcmp(b, "60 FPS  16.7 ms") && n == 15, "format: \"%s\" (%d)", b, n);
    fps_overlay_format(b, sizeof b, 144.2f, 6.93f);
    CHECK(!strcmp(b, "144 FPS  6.9 ms"), "format: \"%s\"", b);
    n = fps_overlay_format(b, sizeof b, 5000.0f, 5000.0f);
    CHECK(!strcmp(b, "999 FPS  999.9 ms") && n <= FPS_MAX_CHARS, "format clamps: \"%s\"", b);
    fps_overlay_format(b, sizeof b, -3.0f, -1.0f);
    CHECK(!strcmp(b, "0 FPS  0.0 ms"), "format clamps below: \"%s\"", b);
    fps_overlay_format(b, 6, 60.0f, 16.6f);
    CHECK(strlen(b) == 5, "format truncates to the buffer: \"%s\"", b);
}

static void test_glyphs(void) {
    uint8_t g[FPS_MAX_CHARS];
    int n = fps_overlay_glyphs("60 FPS 1.5ms", g, FPS_MAX_CHARS);
    const uint8_t want[] = {6, 0, 16, 11, 12, 13, 16, 1, 10, 5, 14, 15};
    CHECK(n == 12 && !memcmp(g, want, sizeof want), "glyph indices (%d)", n);
    n = fps_overlay_glyphs("Xz", g, FPS_MAX_CHARS);
    CHECK(n == 2 && g[0] == 16 && g[1] == 16, "unknown characters draw as spaces");
    n = fps_overlay_glyphs("012345678901234567890123456789012345678901234", g, FPS_MAX_CHARS);
    CHECK(n == FPS_MAX_CHARS, "glyphs capped at %d (%d)", FPS_MAX_CHARS, n);
}

static void test_layout(void) {
    CHECK(fps_overlay_scale(1920, 1080) == 2 && fps_overlay_scale(640, 480) == 1 && fps_overlay_scale(2560, 1440) == 3 &&
              fps_overlay_scale(320, 100) == 1,
          "scale: 1080p 2, 480 1, 1440p 3, tiny 1");
    FpsBox b = fps_overlay_box(1920, 1080, FPS_CORNER_TL, 15);
    int w = (15 * FPS_ADVANCE - 1 + 2 * FPS_PAD) * 2, h = (FPS_FONT_H + 2 * FPS_PAD) * 2, m = FPS_MARGIN * 2;
    CHECK(b.scale == 2 && b.x == m && b.y == m && b.w == w && b.h == h, "tl box %d,%d %dx%d", b.x, b.y, b.w, b.h);
    b = fps_overlay_box(1920, 1080, FPS_CORNER_BR, 15);
    CHECK(b.x == 1920 - m - w && b.y == 1080 - m - h, "br box %d,%d", b.x, b.y);
    b = fps_overlay_box(1920, 1080, FPS_CORNER_TR, 15);
    CHECK(b.x == 1920 - m - w && b.y == m, "tr box %d,%d", b.x, b.y);
    b = fps_overlay_box(1920, 1080, FPS_CORNER_BL, 15);
    CHECK(b.x == m && b.y == 1080 - m - h, "bl box %d,%d", b.x, b.y);
    CHECK(w < 1920 / 8 && h < 1080 / 30, "small: %dx%d at 1080p", w, h);
    CHECK(fps_overlay_corner("tl") == FPS_CORNER_TL && fps_overlay_corner("br") == FPS_CORNER_BR && fps_overlay_corner("tr") == FPS_CORNER_TR &&
              fps_overlay_corner("bl") == FPS_CORNER_BL && fps_overlay_corner("x") == -1 && fps_overlay_corner("") == -1,
          "corner names");
}

static void test_ink(void) {
    uint8_t g[4];
    int n = fps_overlay_glyphs("1 F", g, 4);
    /* '1': 00100 / 01100 / 00100 x4 / 01110 */
    CHECK(fps_overlay_ink(g, n, 2, 0) && !fps_overlay_ink(g, n, 0, 0) && fps_overlay_ink(g, n, 1, 1) && fps_overlay_ink(g, n, 3, 6) &&
              !fps_overlay_ink(g, n, 4, 6),
          "'1' rows");
    CHECK(!fps_overlay_ink(g, n, 5, 0) && !fps_overlay_ink(g, n, 5, 6), "gap column is empty");
    int sp = 0;
    for (int y = 0; y < FPS_FONT_H; y++)
        for (int x = 6; x < 12; x++) sp += fps_overlay_ink(g, n, x, y);
    CHECK(!sp, "space is empty");
    CHECK(fps_overlay_ink(g, n, 12, 0) && fps_overlay_ink(g, n, 16, 0) && fps_overlay_ink(g, n, 12, 6) && !fps_overlay_ink(g, n, 13, 6),
          "'F' top bar and stem");
    CHECK(!fps_overlay_ink(g, n, -1, 0) && !fps_overlay_ink(g, n, 0, -1) && !fps_overlay_ink(g, n, 2, 7) && !fps_overlay_ink(g, n, 18, 0),
          "outside the text is empty");
    /* every glyph but the space has ink, and no two glyphs are the same */
    uint8_t all[FPS_FONT_GLYPHS];
    fps_overlay_glyphs(FPS_FONT_CHARS, all, FPS_FONT_GLYPHS);
    int empty = 0, same = 0;
    for (int a = 0; a < FPS_FONT_GLYPHS; a++) {
        if (a == FPS_GLYPH_SPACE) continue;
        int ink = 0;
        for (int y = 0; y < FPS_FONT_H; y++)
            for (int x = 0; x < FPS_FONT_W; x++) ink += fps_overlay_ink(&all[a], 1, x, y);
        empty += !ink;
        for (int b = a + 1; b < FPS_FONT_GLYPHS; b++) {
            if (b == FPS_GLYPH_SPACE) continue;
            int diff = 0;
            for (int y = 0; y < FPS_FONT_H; y++)
                for (int x = 0; x < FPS_FONT_W; x++) diff += fps_overlay_ink(&all[a], 1, x, y) != fps_overlay_ink(&all[b], 1, x, y);
            same += !diff;
        }
    }
    CHECK(!empty && !same, "font: %d empty glyphs, %d duplicates", empty, same);
}

/* --fps-overlay-items: any subset of fps,ms,cpu,gpu (in any order, shown in that fixed order). */
static void test_items(void) {
    CHECK(fps_overlay_items("fps") == FPS_ITEM_FPS && fps_overlay_items("ms") == FPS_ITEM_MS && fps_overlay_items("cpu") == FPS_ITEM_CPU &&
              fps_overlay_items("gpu") == FPS_ITEM_GPU,
          "items: one each");
    CHECK(fps_overlay_items("fps,ms,cpu,gpu") == 15 && fps_overlay_items("gpu,fps") == (FPS_ITEM_FPS | FPS_ITEM_GPU) &&
              fps_overlay_items("cpu,cpu") == FPS_ITEM_CPU,
          "items: subsets, any order, repeats");
    CHECK(fps_overlay_items("") == -1 && fps_overlay_items("fps,") == -1 && fps_overlay_items(",ms") == -1 && fps_overlay_items("fps,vram") == -1 &&
              fps_overlay_items("FPS") == -1 && fps_overlay_items("fps ms") == -1,
          "items: empty, unknown or malformed lists are rejected");
    CHECK(FPS_ITEMS_DEFAULT == (FPS_ITEM_FPS | FPS_ITEM_MS), "--fps-overlay alone: fps and ms");
}

static void test_format_items(void) {
    char b[64];
    int n = fps_overlay_format_items(b, sizeof b, 15, 60.2f, 16.61f, 34.4f, 70.6f);
    CHECK(!strcmp(b, "60 FPS  16.6 ms  CPU 34%  GPU 71%") && n == (int)strlen(b), "all four: \"%s\"", b);
    fps_overlay_format_items(b, sizeof b, FPS_ITEM_FPS, 60.2f, 16.61f, 34.4f, 70.6f);
    CHECK(!strcmp(b, "60 FPS"), "fps only: \"%s\"", b);
    fps_overlay_format_items(b, sizeof b, FPS_ITEM_MS | FPS_ITEM_GPU, 60.2f, 16.61f, 34.4f, 70.6f);
    CHECK(!strcmp(b, "16.6 ms  GPU 71%"), "ms + gpu: \"%s\"", b);
    fps_overlay_format_items(b, sizeof b, FPS_ITEM_CPU, 60, 16.6f, 0.2f, 0);
    CHECK(!strcmp(b, "CPU 0%"), "cpu only: \"%s\"", b);
    fps_overlay_format_items(b, sizeof b, FPS_ITEM_FPS | FPS_ITEM_CPU | FPS_ITEM_GPU, 60, 16.6f, -1, -1);
    CHECK(!strcmp(b, "60 FPS  CPU --  GPU --"), "unknown values show --: \"%s\"", b);
    fps_overlay_format_items(b, sizeof b, FPS_ITEM_CPU | FPS_ITEM_GPU, 60, 16.6f, 250, 180);
    CHECK(!strcmp(b, "CPU 100%  GPU 100%"), "percentages clamp to 100: \"%s\"", b);
    n = fps_overlay_format_items(b, sizeof b, 15, 5000, 5000, 100, 100);
    CHECK(!strcmp(b, "999 FPS  999.9 ms  CPU 100%  GPU 100%") && n <= FPS_MAX_CHARS, "longest text fits (%d <= %d)", n, FPS_MAX_CHARS);
    char legacy[32];
    fps_overlay_format(legacy, sizeof legacy, 59.96f, 16.68f);
    fps_overlay_format_items(b, sizeof b, FPS_ITEMS_DEFAULT, 59.96f, 16.68f, -1, -1);
    CHECK(!strcmp(b, legacy) && !strcmp(b, "60 FPS  16.7 ms"), "default items = the old text: \"%s\"", b);
    fps_overlay_format_items(b, 8, 15, 60, 16.6f, 34, 71);
    CHECK(strlen(b) == 7, "truncates to the buffer: \"%s\"", b);
    fps_overlay_format_items(b, sizeof b, 0, 60, 16.6f, 34, 71);
    CHECK(!strcmp(b, ""), "no items: empty text");
}

/* CPU %: process CPU time over wall time, 100 = every logical core busy. */
static void test_cpu(void) {
    CHECK(fps_cpu_percent(250, 500, 1) > 49.9f && fps_cpu_percent(250, 500, 1) < 50.1f, "one core, half busy: 50%%");
    CHECK(fps_cpu_percent(1000, 500, 4) > 49.9f && fps_cpu_percent(1000, 500, 4) < 50.1f, "4 cores, 2 busy: 50%% (%.2f)",
          fps_cpu_percent(1000, 500, 4));
    CHECK(fps_cpu_percent(500, 500, 12) > 8.3f && fps_cpu_percent(500, 500, 12) < 8.4f, "12 threads, 1 busy: 8.3%%");
    CHECK(fps_cpu_percent(5000, 500, 4) == 100.0f && fps_cpu_percent(-5, 500, 4) == 0.0f, "clamped to 0-100");
    CHECK(fps_cpu_percent(100, 0, 4) < 0 && fps_cpu_percent(100, 500, 0) < 0, "no window or no cores: unknown (-1)");
}

/* GPU %: timestamp ticks (masked to the valid bits) times the period, over the frame period. */
static void test_gpu(void) {
    CHECK(fps_gpu_busy_ms(1000, 1000 + 8000000, 64, 1.0f) > 7.999 && fps_gpu_busy_ms(1000, 1000 + 8000000, 64, 1.0f) < 8.001, "8e6 ticks of 1 ns: 8 ms");
    double ms = fps_gpu_busy_ms(0, 1000000, 64, 2.5f);
    CHECK(ms > 2.499 && ms < 2.501, "period 2.5 ns: %.3f ms", ms);
    ms = fps_gpu_busy_ms(0xFFFFFF00ull, 0x00000100ull, 32, 1.0f);   /* a 32-bit counter wrapped */
    CHECK(ms > 0.000511 && ms < 0.000513, "32 valid bits: the wrap is masked (%.6f ms)", ms);
    CHECK(fps_gpu_busy_ms(0, 100, 0, 1.0f) < 0, "no valid bits: unknown (-1)");
    CHECK(fps_gpu_percent(8.3, 16.6) > 49.9f && fps_gpu_percent(8.3, 16.6) < 50.1f, "8.3 of 16.6 ms: 50%%");
    CHECK(fps_gpu_percent(40, 16.6) == 100.0f && fps_gpu_percent(-1, 16.6) < 0 && fps_gpu_percent(5, 0) < 0, "clamped; unknown stays -1");
}

static void test_new_glyphs(void) {
    uint8_t g[8];
    int n = fps_overlay_glyphs("CUG%- ", g, 8);
    CHECK(n == 6 && g[0] == 17 && g[1] == 18 && g[2] == 19 && g[3] == 20 && g[4] == 21 && g[5] == FPS_GLYPH_SPACE, "C U G %% - glyphs");
    n = fps_overlay_glyphs("x", g, 8);
    CHECK(n == 1 && g[0] == FPS_GLYPH_SPACE, "unknown still draws as a space");
    /* '-': only the middle row */
    n = fps_overlay_glyphs("-", g, 8);
    int ink = 0, mid = 0;
    for (int y = 0; y < FPS_FONT_H; y++)
        for (int x = 0; x < FPS_FONT_W; x++) ink += fps_overlay_ink(g, n, x, y), mid += y == 3 && fps_overlay_ink(g, n, x, y);
    CHECK(ink == 5 && mid == 5, "'-' is the middle row (%d ink)", ink);
    n = fps_overlay_glyphs("999 FPS  999.9 ms  CPU 100%  GPU 100%", g, 8);
    CHECK(n == 8, "glyphs still capped by max");
}

int main(void) {
    test_meter();
    test_format();
    test_glyphs();
    test_layout();
    test_ink();
    test_items();
    test_format_items();
    test_cpu();
    test_gpu();
    test_new_glyphs();
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
