/* Game-independent test of the robustness diagnostics (src/diaglog.c): the breadcrumb ring's
 * wraparound and line format, the crash-context line, the crash dump through write(), the
 * once-per-name [hle-missing] dedupe and its summary, the rate limiter, the [vk-error] and
 * [audio] underrun reporters, the exit summary and the movie file-name rule. Output is captured through a pipe put on
 * stdout. */
#include "../src/diaglog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static uint32_t frame = 42;
static uint32_t src_frame(void) { return frame; }
static uint64_t src_used(void) { return 13ull << 20; }
static uint64_t src_total(void) { return 1536ull << 20; }
static uint32_t src_threads(void) { return 9; }

/* stdout capture: everything printed (stdio and write(1)) between begin and end goes into a
 * pipe (each capture stays well below the 64 KiB pipe buffer). */
static int cap_pipe[2], saved_fd;
static void cap_begin(void) {
    fflush(stdout);
    if (pipe(cap_pipe)) { perror("pipe"); exit(2); }
    saved_fd = dup(1);
    dup2(cap_pipe[1], 1);
    close(cap_pipe[1]);
}
static char cap_text[65536];
static const char *cap_end(void) {
    fflush(stdout);
    dup2(saved_fd, 1);   /* closes the pipe's last write end */
    close(saved_fd);
    size_t n = 0;
    ssize_t r;
    while (n < sizeof cap_text - 1 && (r = read(cap_pipe[0], cap_text + n, sizeof cap_text - 1 - n)) > 0) n += (size_t)r;
    cap_text[n] = 0;
    close(cap_pipe[0]);
    return cap_text;
}
static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

static char buf[32768];

static void test_crumbs(void) {
    dlog_reset();
    /* Fewer than the ring holds: all of them, oldest first, in the documented format. */
    frame = 7;
    dlog_crumb("level load %s (mission %d)", "level81", 8);
    uint64_t now = dlog_now_ns() + 2000000000ull;   /* 2 s after the event */
    size_t n = dlog_crumbs_format(buf, sizeof buf, now);
    CHECK(n == strlen(buf), "the returned length is the text length (%zu)", n);
    CHECK(!strncmp(buf, "[breadcrumb] -2.0", 17) && strstr(buf, "s frame 7: level load level81 (mission 8)\n") &&
              count_of(buf, "\n") == 1,
          "one breadcrumb line \"[breadcrumb] -2.0..s frame 7: level load level81 (mission 8)\": %s", buf);
    const char *dot = strchr(buf, '.');
    CHECK(dot && dot[4] == 's', "the age has 3 decimals (%.24s)", buf);

    /* Wraparound: 105 events keep the last 100, oldest first. */
    dlog_reset();
    for (int i = 0; i < 105; i++) {
        frame = 1000 + i;
        dlog_crumb("event %d", i);
    }
    n = dlog_crumbs_format(buf, sizeof buf, dlog_now_ns());
    CHECK(count_of(buf, "[breadcrumb] ") == DLOG_CRUMBS, "%d breadcrumb lines after 105 events", count_of(buf, "[breadcrumb] "));
    CHECK(!strstr(buf, ": event 4\n") && strstr(buf, "frame 1005: event 5\n") == strstr(buf, "frame "),
          "the oldest kept is event 5 (events 0-4 were overwritten)");
    const char *last = strstr(buf, "frame 1104: event 104\n");
    CHECK(last && last[strlen("frame 1104: event 104\n")] == 0, "the newest, event 104, is last");
    int ordered = 1;
    const char *prev = buf;
    for (int i = 6; i < 105 && ordered; i++) {
        char want[32];
        snprintf(want, sizeof want, ": event %d\n", i);
        const char *at = strstr(buf, want);
        ordered = at && at > prev;
        prev = at;
    }
    CHECK(ordered, "breadcrumbs stay in event order across the wrap");

    /* A long event is cut to DLOG_CRUMB_TEXT - 1 bytes; a small buffer is never overrun. */
    dlog_reset();
    char longtext[300];
    memset(longtext, 'x', sizeof longtext - 1);
    longtext[sizeof longtext - 1] = 0;
    dlog_crumb("%s", longtext);
    dlog_crumbs_format(buf, sizeof buf, dlog_now_ns());
    const char *x = strchr(buf, 'x');
    CHECK(x && strspn(x, "x") == DLOG_CRUMB_TEXT - 1 && x[DLOG_CRUMB_TEXT - 1] == '\n',
          "a %zu-byte event keeps %d bytes", strlen(longtext), DLOG_CRUMB_TEXT - 1);
    char small[40];
    memset(small, '#', sizeof small);
    n = dlog_crumbs_format(small, 24, dlog_now_ns());
    CHECK(n == 23 && small[23] == 0 && small[24] == '#', "a 24-byte buffer gets 23 bytes and a NUL, nothing past it");
}

static void test_context(void) {
    dlog_reset();
    frame = 3712;
    uint64_t now = dlog_now_ns();
    dlog_context_format(buf, sizeof buf, now);
    CHECK(!strncmp(buf, "[crash-context] level -, mission -, frame 3712, uptime ", 55),
          "unknown level and mission print as -: %s", buf);
    dlog_set_level("level51", 14);
    dlog_context_format(buf, sizeof buf, now);
    const char *tail = " s, guest arena used 13 MiB of 1536 MiB, threads 9\n";
    CHECK(!strncmp(buf, "[crash-context] level level51, mission 14, frame 3712, uptime ", 62) &&
              strlen(buf) > strlen(tail) && !strcmp(buf + strlen(buf) - strlen(tail), tail),
          "context line: %s", buf);
    dlog_set_level("level95", 0);
    dlog_context_format(buf, sizeof buf, now);
    CHECK(strstr(buf, "level level95, mission 0,") != NULL, "mission 0 (the hub) prints as 0");

    /* The crash dump: header, breadcrumbs, context, through write(1). */
    dlog_reset();
    dlog_set_level("level51", 14);
    dlog_crumb("thread t9 create");
    dlog_crumb("large alloc 64 MiB");
    cap_begin();
    dlog_crash_dump("test fault");
    const char *out = cap_end();
    const char *h = strstr(out, "[crash-context] reason: test fault (last 2 breadcrumbs, oldest first)\n");
    const char *c1 = strstr(out, ": thread t9 create\n"), *c2 = strstr(out, ": large alloc 64 MiB\n");
    const char *ctx = strstr(out, "[crash-context] level level51, mission 14,");
    CHECK(h && c1 && c2 && ctx && h < c1 && c1 < c2 && c2 < ctx, "crash dump order: header, breadcrumbs, context:\n%s", out);
}

static void test_missing(void) {
    dlog_reset();
    frame = 5;
    cap_begin();
    int a = dlog_missing_first("KeSaveFloatingPointState", 0x1234, NULL);
    int b = dlog_missing_first("KeSaveFloatingPointState", 0x5678, NULL);   /* same name: no new line */
    dlog_missing_hit(a);
    dlog_missing_hit(b);
    for (int i = 0; i < 5; i++) DLOG_MISSING("D3DDevice_SetTile", 0xabcd0000u + i);   /* one call site */
    static uint32_t ext = 0;
    dlog_missing_first("AvSendTVEncoderOption", 0x99, &ext);
    ext = 7;   /* the caller's own counter is read at summary time */
    dlog_missing_first("Unused", 0x1, NULL);
    const char *out = cap_end();
    CHECK(a == b && a >= 0, "the same name gets the same slot (%d, %d)", a, b);
    CHECK(count_of(out, "[hle-missing] KeSaveFloatingPointState (first call from 0x00001234, frame 5)\n") == 1 &&
              count_of(out, "KeSaveFloatingPointState") == 1,
          "one line for KeSaveFloatingPointState, from its first caller");
    CHECK(count_of(out, "[hle-missing] D3DDevice_SetTile (first call from 0xabcd0000, frame 5)\n") == 1 &&
              count_of(out, "D3DDevice_SetTile") == 1,
          "DLOG_MISSING logs its first call only");
    CHECK(count_of(out, "[hle-missing] ") == 4, "4 first-call lines for 4 names:\n%s", out);
    dlog_missing_summary_format(buf, sizeof buf);
    CHECK(!strcmp(buf, "[hle-missing] summary: AvSendTVEncoderOptionx7, D3DDevice_SetTilex5, KeSaveFloatingPointStatex2, Unusedx0\n"),
          "summary, most calls first: %s", buf);
    dlog_reset();
    dlog_missing_summary_format(buf, sizeof buf);
    CHECK(!strcmp(buf, "[hle-missing] summary: none\n"), "empty summary: %s", buf);

    /* A full table refuses new names (-1) without logging or crashing. */
    cap_begin();
    char name[32];
    int last = 0;
    for (int i = 0; i <= DLOG_MISSING_MAX; i++) {
        snprintf(name, sizeof name, "fn%d", i);
        last = dlog_missing_first(name, 0, NULL);
    }
    DLOG_MISSING("overflow", 0);
    out = cap_end();
    CHECK(last == -1 && count_of(out, "[hle-missing] ") == DLOG_MISSING_MAX, "%d names fit, the next one is refused",
          DLOG_MISSING_MAX);
    dlog_reset();
}

static void test_rate(void) {
    const uint64_t S = 1000000000ull;
    DlogRate r = {0};
    uint32_t sup = 99;
    int allowed = 0;
    for (int i = 0; i < 5; i++) allowed += dlog_rate_allow(&r, 100 * S + i * S, 3, 10 * S, &sup);
    CHECK(allowed == 3 && sup == 0, "burst 3 in a 10 s window: 3 of 5 allowed (suppressed before them %u)", sup);
    CHECK(!dlog_rate_allow(&r, 109 * S, 3, 10 * S, &sup), "still dropped 9 s into the window");
    CHECK(dlog_rate_allow(&r, 110 * S, 3, 10 * S, &sup) && sup == 3, "a new window allows again and reports 3 dropped (%u)", sup);
    CHECK(dlog_rate_allow(&r, 111 * S, 3, 10 * S, &sup) && sup == 0, "the dropped count restarts at 0 (%u)", sup);
    DlogRate z = {0};
    CHECK(dlog_rate_allow(&z, 0, 1, 10 * S, NULL) && !dlog_rate_allow(&z, 1, 1, 10 * S, NULL), "a window starting at time 0 works");

    /* [vk-error]: other results rate-limited per call and result, swapchain ones once. */
    dlog_reset();
    frame = 77;
    cap_begin();
    for (int i = 0; i < 20; i++) dlog_vk_error("vkQueueSubmit", -2, "VK_ERROR_OUT_OF_DEVICE_MEMORY", 0, 0);
    for (int i = 0; i < 12; i++) dlog_vk_error("vkQueuePresentKHR", -1000001004, "VK_ERROR_OUT_OF_DATE_KHR", 1, 0);
    for (int i = 0; i < 3; i++) dlog_vk_error("vkAcquireNextImageKHR", 1000001003, "VK_SUBOPTIMAL_KHR", 1, 0);
    const char *out = cap_end();
    CHECK(count_of(out, "[vk-error] vkQueueSubmit: VK_ERROR_OUT_OF_DEVICE_MEMORY (frame 77)\n") == 5,
          "20 identical errors in one window: 5 lines");
    CHECK(count_of(out, "[vk-error] vkQueuePresentKHR: VK_ERROR_OUT_OF_DATE_KHR (frame 77) (further ones only counted)\n") == 1 &&
              count_of(out, "vkAcquireNextImageKHR: VK_SUBOPTIMAL_KHR") == 1,
          "swapchain results: one line each:\n%s", out);
    /* [audio] underruns: rate-limited lines with the running count. */
    cap_begin();
    for (int i = 0; i < 10; i++) dlog_audio_underrun();
    out = cap_end();
    CHECK(count_of(out, "[audio] underrun ") == 3 && strstr(out, "[audio] underrun 1 (frame 77)\n") &&
              strstr(out, "[audio] underrun 3 (frame 77)\n") && dlog_audio_underruns() == 10,
          "10 underruns: 3 lines (1-3), total %u", dlog_audio_underruns());
    cap_begin();
    dlog_exit_summary();
    dlog_exit_summary();   /* once */
    out = cap_end();
    CHECK(count_of(out, "[hle-missing] summary: none\n") == 1 && count_of(out, "[audio] underruns: 10 total\n") == 1 &&
              count_of(out, "[vk-error] summary: vkQueueSubmit VK_ERROR_OUT_OF_DEVICE_MEMORY x20, vkQueuePresentKHR "
                            "VK_ERROR_OUT_OF_DATE_KHR x12, vkAcquireNextImageKHR VK_SUBOPTIMAL_KHR x3\n") == 1,
          "exit summary, printed once:\n%s", out);

    /* Device lost dumps the breadcrumbs once. */
    dlog_reset();
    dlog_crumb("swapchain recreate 1280x720");
    cap_begin();
    dlog_vk_error("vkQueueSubmit", -4, "VK_ERROR_DEVICE_LOST", 0, 1);
    dlog_vk_error("vkQueueSubmit", -4, "VK_ERROR_DEVICE_LOST", 0, 1);
    out = cap_end();
    CHECK(count_of(out, "[crash-context] reason: Vulkan device lost") == 1 && strstr(out, ": swapchain recreate 1280x720\n") &&
              strstr(out, ": Vulkan device lost in vkQueueSubmit\n"),
          "device lost: one crash dump with the breadcrumbs:\n%s", out);
}

static void test_movie_name(void) {
    const char *m = dlog_movie_name("\\Device\\CdRom0\\bik\\l1_in.BIK");
    CHECK(m && !strcmp(m, "l1_in.BIK"), "movie name of a Xbox path: %s", m ? m : "(null)");
    m = dlog_movie_name("/data/disc/bik_pal/logo_w.bik");
    CHECK(m && !strcmp(m, "logo_w.bik"), "movie name of a host path: %s", m ? m : "(null)");
    CHECK(!dlog_movie_name("D:\\Main.wad0") && !dlog_movie_name("bik") && !dlog_movie_name(NULL), "other files are not movies");
}

int main(void) {
    dlog_init();
    DlogSources s = {src_frame, src_used, src_total, src_threads};
    dlog_set_sources(&s);
    test_crumbs();
    test_context();
    test_missing();
    test_rate();
    test_movie_name();
    printf("%s: %d failure%s\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
