/* Game-independent --bench test (src/bench.c): frame statistics (average, 1 % low, worst, GPU),
 * the settings without paths, and the report text. */
#include "../src/bench.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_summary(void) {
    static float ms[1000], gpu[1000];
    for (int i = 0; i < 1000; i++) {
        ms[i] = 16.0f;
        gpu[i] = i % 2 ? 10.0f : -1.0f;
    }
    for (int i = 0; i < 10; i++) ms[i * 97] = 40.0f;   /* 10 slow frames = the slowest 1 % */
    BenchSummary s;
    bench_summarize(ms, gpu, 1000, &s);
    const double total = 990 * 16.0 + 10 * 40.0;
    CHECK(s.frames == 1000 && fabs(s.seconds - total / 1000) < 1e-6, "1000 frames, %.3f s", s.seconds);
    CHECK(fabs(s.avg_fps - 1000 / (total / 1000)) < 1e-6, "average fps %.2f", s.avg_fps);
    CHECK(fabs(s.low1_fps - 25.0) < 1e-6, "1 %% low = 1000 / 40 ms = 25 fps (%.2f)", s.low1_fps);
    CHECK(s.worst_ms == 40.0 && fabs(s.gpu_ms - 10.0) < 1e-9, "worst 40 ms, GPU 10 ms (frames without a timestamp skipped)");
    CHECK(ms[97] == 40.0f && ms[0] == 40.0f && ms[1] == 16.0f, "the input is not reordered");
    float few[3] = {10, 20, 0};
    bench_summarize(few, NULL, 3, &s);
    CHECK(s.frames == 2 && fabs(s.low1_fps - 50.0) < 1e-9 && s.gpu_ms < 0, "few frames: the slowest one is the 1 %% low (%.1f), no GPU", s.low1_fps);
    bench_summarize(few, NULL, 0, &s);
    CHECK(s.frames == 0 && s.avg_fps == 0 && s.low1_fps == 0, "no frames: zeros");
}

static void test_settings(void) {
    char *argv[] = {"xbrun", "/home/u/disc/default.xbe", "/home/u/disc", "--resolution", "1920x1080", "--bench", "--taa-upscale", "ultra",
                    "--texture-pack", "C:\\Games\\W\\texpack", "--bench-out", "/tmp/r.txt", "--dynres", "auto", "--msaa", "2"};
    char buf[512];
    bench_settings(16, argv, buf, sizeof buf);
    CHECK(!strcmp(buf, "--resolution 1920x1080 --taa-upscale ultra --texture-pack <path> --dynres auto --msaa 2"), "settings: \"%s\"", buf);
    char small[16];
    bench_settings(16, argv, small, sizeof small);
    CHECK(strlen(small) == 15, "truncated to the buffer (%zu)", strlen(small));
}

static void test_report(void) {
    BenchSummary s = {3600, 60.5, 59.5, 48.2, 31.0, 11.25};
    BenchReport r = {"abc123", "NVIDIA GeForce RTX 2050", "--taa-upscale ultra", "total 11.25 ms: scene draws 5.00", "level80", 1920, 1080,
                     1100, 1477, 619, 831, 1300.4, 1, &s, NULL};
    char buf[2048];
    const size_t n = bench_report(buf, sizeof buf, &r);
    CHECK(n == strlen(buf) && n > 0 && buf[n - 1] == '\n', "report ends with a newline (%zu chars)", n);
    CHECK(strstr(buf, "fps: average 59.5, 1% low 48.2, worst frame 31.0 ms") != NULL, "fps line");
    CHECK(strstr(buf, "gpu: NVIDIA GeForce RTX 2050, 11.25 ms per frame") != NULL, "gpu line");
    CHECK(strstr(buf, "render: 1100x619 to 1477x831 (mean width 1300), output 1920x1080, dynamic resolution on") != NULL, "render line");
    CHECK(strstr(buf, "passes: total 11.25 ms: scene draws 5.00") && strstr(buf, "settings: --taa-upscale ultra") &&
          strstr(buf, "version: abc123") && strstr(buf, "level: level80"), "passes, settings, version, level lines");
    BenchReport f = {"abc123", "Intel(R) Iris(R) Xe Graphics", "", "", "", 1920, 1080, 0, 0, 0, 0, 0, 0, NULL, "the level did not start"};
    bench_report(buf, sizeof buf, &f);
    CHECK(strstr(buf, "result: failed: the level did not start") != NULL, "failure line: %s", buf);
    char tiny[20];
    const size_t m = bench_report(tiny, sizeof tiny, &r);
    CHECK(m == 19 && strlen(tiny) == 19, "truncated report (%zu)", m);
}

int main(void) {
    test_summary();
    test_settings();
    test_report();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
