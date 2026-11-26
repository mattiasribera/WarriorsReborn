/* the shared logic of the Windows helper, on Linux.
 *   1. r64_lines_* (render_ipc.c): the Windows game prints the helper's stdout pipe through it: lines in any split,
 *      "\r\n" endings, the "[r64] " prefix added once, a line longer than the buffer in pieces, the flush of a last
 *      line without a newline.
 *   2. --render64-selftest (src/r64_selftest.c) with a helper that does not exist: FAIL (exit 1) with the reason.
 *   3. --render64-selftest with the x86_64 helper: PASS (exit 0); 77 (skip) without a Vulkan device. */
#include "../src/r64_selftest.h"
#include "../src/render_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static char got[16][1200];
static int ngot;
static void take(void *ctx, const char *line) {
    (void)ctx;
    if (ngot < 16) snprintf(got[ngot], sizeof got[0], "%s", line);
    ngot++;
}

static void test_lines(void) {
    R64Lines s;
    memset(&s, 0, sizeof s);
    ngot = 0;
    const char *in = "[vk] device X\r\n[r64] window 640x480\nSDL: par";
    for (const char *p = in; *p; p++) r64_lines_feed(&s, p, 1, take, NULL);   /* one byte at a time */
    r64_lines_feed(&s, "tial\n\nlast", 10, take, NULL);
    CHECK(ngot == 4, "lines split across reads: %d lines before the flush", ngot);
    CHECK(!strcmp(got[0], "[r64] [vk] device X"), "a line without the prefix gets it, \\r dropped: '%s'", got[0]);
    CHECK(!strcmp(got[1], "[r64] window 640x480"), "a line with the prefix keeps one: '%s'", got[1]);
    CHECK(!strcmp(got[2], "[r64] SDL: partial"), "a line in two reads is one line: '%s'", got[2]);
    CHECK(!strcmp(got[3], "[r64] "), "an empty line stays a line: '%s'", got[3]);
    r64_lines_flush(&s, take, NULL);
    CHECK(ngot == 5 && !strcmp(got[4], "[r64] last"), "the flush passes the last line without a newline: '%s'", ngot >= 5 ? got[4] : "");
    r64_lines_flush(&s, take, NULL);
    CHECK(ngot == 5, "a second flush passes nothing");
    char big[2500];
    memset(big, 'x', sizeof big);
    big[sizeof big - 1] = '\n';
    ngot = 0;
    r64_lines_feed(&s, big, sizeof big, take, NULL);
    size_t total = 0;
    for (int i = 0; i < ngot && i < 16; i++) total += strlen(got[i]) - 6;
    CHECK(ngot == 3 && total == sizeof big - 1, "a %zu-byte line comes in %d pieces of the buffer's size (%zu bytes)", sizeof big - 1, ngot, total);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *helper = "build64/xbrun-render64";
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--helper")) helper = argv[++i];
    test_lines();
    const int missing = r64_selftest_run("/nonexistent/xbrun-render64");
    if (missing == 77) {
        printf("SKIP: no Vulkan device (the selftest itself skips)\n");
    } else {
        CHECK(missing == 1, "the selftest with a missing helper: FAIL, exit %d", missing);
        const int r = r64_selftest_run(helper);
        CHECK(r == 0, "the selftest with %s: PASS, exit %d", helper, r);
    }
    printf("%s: %d failure%s\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
