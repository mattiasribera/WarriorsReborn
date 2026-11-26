/* render_ipc_test: the render_ipc channel between this program (the
 * parent; built i386 in build/ and x86_64 in build64/) and the helper skeleton (build64/xbrun-render64,
 * x86_64). Checks: spawn failures, handshake mismatches refused on both sides, N frames of 2, 4 and
 * 16 MB echoed with checksums (throughput and latency printed), and on Linux the crash paths: the
 * helper killed mid-frame is detected in < 100 ms and the parent exits 0 with the FAULT line; the
 * parent killed takes the helper with it in < 1 s; no process is left behind.
 *
 * usage: render_ipc_test [--helper PATH] [--frames N] [--paced N] [--no-kill]
 *   --frames  frames per size at full speed (default 10000), --paced frames per size at 60 Hz (120) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/render_ipc.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
extern char **environ;
#endif

static int checks, failures;
#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        checks++;                                                                                                       \
        if (!(cond)) {                                                                                                  \
            failures++;                                                                                                 \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                      \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

static const char *helper;

static void sleep_ms(double ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = {(time_t)(ms / 1000), (long)((ms - (double)(long)(ms / 1000) * 1000) * 1e6)};
    nanosleep(&ts, NULL);
#endif
}
static double ms_since(uint64_t t0) { return (double)(r64_now_ns() - t0) / 1e6; }

static int cmp_d(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double pct(double *v, uint32_t n, int p) {
    size_t i = (size_t)n * (size_t)p / 100;
    return v[i < n ? i : n - 1];
}

static R64SpawnOpts opts(void) {
    R64SpawnOpts o;
    memset(&o, 0, sizeof o);
    o.helper_path = helper;
    o.schema = R64_SKELETON_SCHEMA;
    o.log_fd = -1;
    return o;
}

/* ---- source frames ---- */

#define SRC_SLACK (64u << 10)
static uint8_t *src;
static void make_source(uint32_t bytes) {
    src = malloc((size_t)bytes + SRC_SLACK);
    uint32_t v = 0x12345678u;
    for (uint32_t i = 0; i < (bytes + SRC_SLACK) / 4; i++) {
        v ^= v << 13, v ^= v >> 17, v ^= v << 5;
        memcpy(src + (size_t)i * 4, &v, 4);
    }
}
/* Frame seq: `bytes` from a per-frame offset into the source (so every frame differs). */
static int send_frame(R64Ipc *x, uint32_t seq, uint32_t bytes, uint32_t *sum_out) {
    const R64EchoFrame h = {R64_ECHO_FRAME, seq, bytes, 0};
    int rc = r64_write(x, R64_RING_CMD, &h, sizeof h);
    const uint8_t *p = src + (seq * 4096u) % SRC_SLACK;
    R64Sum s;
    r64_sum_init(&s);
    for (uint32_t put = 0; !rc && put < bytes;) {
        const uint32_t n = bytes - put < R64_DEFAULT_CHUNK ? bytes - put : R64_DEFAULT_CHUNK;
        r64_sum_update(&s, p + put, n);   /* hashed while the piece is in cache */
        rc = r64_write(x, R64_RING_CMD, p + put, n);
        put += n;
    }
    const uint32_t sum = r64_sum_final(&s);
    if (!rc) rc = r64_write(x, R64_RING_CMD, &sum, 4);
    if (!rc) rc = r64_flush(x, R64_RING_CMD);
    if (sum_out) *sum_out = sum;
    return rc;
}

/* ---- tests ---- */

static void test_sum(void) {
    /* the checksum does not depend on how the stream is split */
    uint8_t buf[4096];
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (uint8_t)(i * 31 + 7);
    R64Sum a, b;
    r64_sum_init(&a);
    r64_sum_update(&a, buf, sizeof buf);
    r64_sum_init(&b);
    uint32_t at = 0;
    for (uint32_t step = 4; at < sizeof buf; step = step % 60 + 8) {
        const uint32_t n = sizeof buf - at < step ? sizeof buf - at : step;
        r64_sum_update(&b, buf + at, n);
        at += n;
    }
    CHECK(r64_sum_final(&a) == r64_sum_final(&b), "split checksum %08x != %08x", r64_sum_final(&b), r64_sum_final(&a));
    buf[1000] ^= 1;
    r64_sum_init(&b);
    r64_sum_update(&b, buf, sizeof buf);
    CHECK(r64_sum_final(&a) != r64_sum_final(&b), "one flipped bit not detected");
}

static void test_spawn_failures(void) {
    R64SpawnOpts o = opts();
    R64Ipc *x;
    char err[300] = "";
    o.helper_path = "/nonexistent/xbrun-render64";
    int rc = r64_spawn(&o, &x, err, sizeof err);
    CHECK(rc == R64_ERR_SYS && !x, "missing helper: rc %d", rc);
    printf("missing helper: %s\n", err);
#ifndef _WIN32
    /* a program that exits without a handshake: detected as an exit, well before the timeout */
    o.helper_path = "/bin/true";
    const uint64_t t0 = r64_now_ns();
    rc = r64_spawn(&o, &x, err, sizeof err);
    const double ms = ms_since(t0);
    CHECK(rc == R64_ERR_PEER_DEAD && strstr(err, "code 0"), "no handshake: rc %d (%s)", rc, err);
    CHECK(ms < 500, "no handshake took %.0f ms", ms);
    printf("no handshake (%.1f ms): %s\n", ms, err);
#endif
}

/* A mismatched handshake: both sides refuse with a clear line, the helper exits 3. */
static void mismatch_case(const char *what, uint32_t parent_version, const char *const *args, const char *expect) {
    R64SpawnOpts o = opts();
    o.test_version = parent_version;
    o.extra_args = args;
#ifndef _WIN32
    const int log = (int)syscall(SYS_memfd_create, "helper-log", 0);
    o.log_fd = log;
#endif
    R64Ipc *x;
    char err[300] = "";
    const int rc = r64_spawn(&o, &x, err, sizeof err);
    CHECK(rc == R64_ERR_VERSION && !x, "%s: rc %d", what, rc);
    CHECK(strstr(err, expect) != NULL, "%s: game error lacks \"%s\": %s", what, expect, err);
    printf("%s, game side: %s\n", what, err);
#ifndef _WIN32
    char buf[2048] = "";
    const ssize_t n = pread(log, buf, sizeof buf - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    close(log);
    CHECK(strstr(buf, "[r64] handshake refused") && strstr(buf, expect), "%s: helper log: %s", what, buf);
    printf("%s, helper side: %s", what, buf);
#endif
}
static void test_mismatch(void) {
    mismatch_case("game version 99", 99, NULL, "version");
    static const char *const v7[] = {"--test-version", "7", NULL};
    mismatch_case("helper version 7", 0, v7, "version 7");
    static const char *const sc[] = {"--test-schema", "1234abcd", NULL};
    mismatch_case("helper schema", 0, sc, "schema 1234abcd");
}

static void test_handshake_and_quit(void) {
    R64SpawnOpts o = opts();
    R64Ipc *x;
    char err[300] = "";
    const uint64_t t0 = r64_now_ns();
    int rc = r64_spawn(&o, &x, err, sizeof err);
    const double ms = ms_since(t0);
    CHECK(rc == R64_OK && x, "spawn: %d %s", rc, err);
    if (!x) return;
    const R64ControlFields *c = &r64_control(x)->f;
    CHECK(c->h_ptr_bits == 64, "helper is %u-bit", c->h_ptr_bits);
    CHECK(c->h_version == R64_VERSION && c->h_schema == R64_SKELETON_SCHEMA, "hello %u %08x", c->h_version, c->h_schema);
    uint32_t sz = 0;
    CHECK(r64_region(x, R64_REG_CONTENT, &sz) && sz == R64_DEFAULT_CONTENT_BYTES, "content arena %u", sz);
    CHECK(c->total == R64_CONTROL_BYTES + R64_DEFAULT_CMD_BYTES + R64_DEFAULT_CONTENT_BYTES + R64_DEFAULT_BACK_BYTES +
                          R64_DEFAULT_STATS_BYTES + R64_DEFAULT_INPUT_BYTES,
          "mapping %u bytes", c->total);
    CHECK(r64_poll(x) == R64_OK, "helper not alive");
    printf("handshake: %.1f ms, helper pid %ld, %u-bit, device \"%s\", mapping %.1f MB\n", ms, r64_peer_pid(x), c->h_ptr_bits,
           c->h_device, c->total / 1048576.0);
    int code = -1;
    const uint64_t t1 = r64_now_ns();
    rc = r64_shutdown(x, 1000, &code);
    CHECK(rc == R64_OK && code == 0, "quit: rc %d, exit code %d", rc, code);
    printf("quit: helper exited %d after %.2f ms\n", code, ms_since(t1));
}

static void test_frames(const uint32_t *sizes, int nsizes, uint32_t frames, uint32_t paced) {
    R64SpawnOpts o = opts();
    R64Ipc *x;
    char err[300] = "";
    if (r64_spawn(&o, &x, err, sizeof err)) {
        CHECK(0, "spawn: %s", err);
        return;
    }
    double *push = malloc(sizeof(double) * frames), *lat = malloc(sizeof(double) * frames);
    uint64_t *t_pushed = malloc(sizeof(uint64_t) * frames);
    uint32_t *sums = malloc(sizeof(uint32_t) * frames);
    uint32_t seq = 0;
    printf("frames: %s-bit game -> %u-bit helper, ring %u MB, chunks %u KB\n", sizeof(void *) == 4 ? "32" : "64",
           r64_control(x)->f.h_ptr_bits, R64_DEFAULT_CMD_BYTES >> 20, R64_DEFAULT_CHUNK >> 10);
    for (int si = 0; si < nsizes; si++) {
        const uint32_t bytes = sizes[si];
        /* full speed: frames back to back, acks drained as they come */
        uint32_t acked = 0, bad = 0;
        const uint32_t base = seq;
        const uint64_t t0 = r64_now_ns();
        int rc = 0;
        for (uint32_t f = 0; f < frames && !rc; f++) {
            const uint64_t ts = r64_now_ns();
            rc = send_frame(x, seq++, bytes, &sums[f]);
            t_pushed[f] = r64_now_ns();
            push[f] = (double)(t_pushed[f] - ts) / 1e6;
            while (!rc && (r64_readable(x, R64_RING_BACK) >= sizeof(R64EchoAck) || (f + 1 == frames && acked < frames))) {
                R64EchoAck a;
                if ((rc = r64_read(x, R64_RING_BACK, &a, sizeof a))) break;
                const uint32_t i = a.seq - base;
                if (a.tag != R64_ECHO_ACK || i != acked || !a.ok || a.sum != sums[i] || a.bytes != bytes) bad++;
                lat[acked] = (double)(int64_t)(((uint64_t)a.t_hi << 32 | a.t_lo) - t_pushed[i]) / 1e3;
                acked++;
            }
        }
        const double total_ms = ms_since(t0);
        CHECK(!rc && acked == frames && !bad, "%.0f MB: rc %d, %u/%u acked, %u bad", bytes / 1048576.0, rc, acked, frames, bad);
        if (acked < frames) break;
        qsort(push, frames, sizeof *push, cmp_d);
        qsort(lat, frames, sizeof *lat, cmp_d);
        printf("%5.1f MB x %u full speed: %.2f GB/s, %.0f frames/s; push p50 %.3f ms p99 %.3f; last byte to helper done "
               "p50 %.0f us p99 %.0f; checksums %u/%u\n",
               bytes / 1048576.0, frames, (double)bytes * frames / (total_ms / 1000) / 1e9, frames / (total_ms / 1000), pct(push, frames, 50),
               pct(push, frames, 99), pct(lat, frames, 50), pct(lat, frames, 99), acked - bad, frames);
        /* 60 Hz: each frame waits for its ack (the latency a frame adds) */
        uint32_t pbad = 0;
        uint64_t next = r64_now_ns() + 2000000;
        const uint32_t pn = paced < frames ? paced : frames;
        for (uint32_t f = 0; f < pn && !rc; f++) {
            for (;;) {
                const double d = (double)(int64_t)(next - r64_now_ns()) / 1e6;   /* signed: the clock may pass next */
                if (d <= 0) break;
                if (d > 0.3) sleep_ms(d - 0.2);
            }
            next += 1000000000u / 60;
            const uint64_t ts = r64_now_ns();
            uint32_t sum;
            rc = send_frame(x, seq++, bytes, &sum);
            const uint64_t tp = r64_now_ns();
            R64EchoAck a;
            if (!rc) rc = r64_read(x, R64_RING_BACK, &a, sizeof a);
            if (rc) break;
            if (a.tag != R64_ECHO_ACK || !a.ok || a.sum != sum) pbad++;
            push[f] = (double)(tp - ts) / 1e6;
            lat[f] = (double)(int64_t)(((uint64_t)a.t_hi << 32 | a.t_lo) - tp) / 1e3;
        }
        CHECK(!rc && !pbad, "%.0f MB paced: rc %d, %u bad", bytes / 1048576.0, rc, pbad);
        if (pn && !rc) {
            qsort(push, pn, sizeof *push, cmp_d);
            qsort(lat, pn, sizeof *lat, cmp_d);
            printf("%5.1f MB x %u at 60 Hz: push p50 %.3f ms p99 %.3f; last byte to helper done p50 %.0f us p99 %.0f\n",
                   bytes / 1048576.0, pn, pct(push, pn, 50), pct(push, pn, 99), pct(lat, pn, 50), pct(lat, pn, 99));
        }
    }
    int code = -1;
    const int rc = r64_shutdown(x, 1000, &code);
    CHECK(rc == R64_OK && code == 0, "quit after frames: rc %d code %d", rc, code);
    free(push), free(lat), free(t_pushed), free(sums);
}

#ifndef _WIN32
/* ---- crash paths: this program re-runs itself as the parent (the "inner" process) ---- */

/* Inner: spawn the helper, print its pid, stream until something fails. mode 'h': 16 MB frames at
 * full speed (the outer test kills the helper); 'p': 60 Hz frames (the outer test kills us). */
static int inner(char mode) {
    R64SpawnOpts o = opts();
    R64Ipc *x;
    char err[300];
    if (r64_spawn(&o, &x, err, sizeof err)) {
        printf("inner: spawn: %s\n", err);
        return 1;
    }
    printf("pid %ld\n", r64_peer_pid(x));
    fflush(stdout);
    const uint32_t bytes = mode == 'h' ? 16u << 20 : 2u << 20;
    const uint64_t t_end = r64_now_ns() + 20000000000ull;
    int rc = 0;
    for (uint32_t seq = 0; !rc && r64_now_ns() < t_end; seq++) {
        rc = send_frame(x, seq, bytes, NULL);
        while (!rc && r64_readable(x, R64_RING_BACK) >= sizeof(R64EchoAck)) {
            R64EchoAck a;
            rc = r64_read(x, R64_RING_BACK, &a, sizeof a);
        }
        if (mode == 'p') sleep_ms(16);
        if (!rc) rc = r64_poll(x);   /* the host loop's once-per-frame check */
    }
    if (rc == R64_ERR_PEER_DEAD) {
        const uint64_t t = r64_now_ns();
        char st[32];
        /* the game's FAULT line (plan 2.4); then a clean exit */
        printf("=== FAULT (render64 helper exited: %s)\n", r64_peer_exit(x, st, sizeof st));
        printf("detected_ns %llu\n", (unsigned long long)t);
        fflush(stdout);
        r64_shutdown(x, 1000, NULL);
        return 0;
    }
    printf("inner: stream ended with %d\n", rc);
    r64_shutdown(x, 1000, NULL);
    return 1;
}

/* Starts the inner parent with its stdout on a pipe; returns its pid, *out the pipe. */
static pid_t start_inner(const char *self, const char *mode, FILE **out) {
    int p[2];
    if (pipe(p)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    const char *argv[] = {self, mode, "--helper", helper, NULL};
    pid_t pid = -1;
    if (posix_spawn(&pid, self, &fa, NULL, (char *const *)argv, environ)) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    *out = fdopen(p[0], "r");
    return pid;
}
static long read_pid(FILE *f) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
        printf("  | %s", line);
        long pid;
        if (sscanf(line, "pid %ld", &pid) == 1) return pid;
    }
    return -1;
}

static void test_kill_helper(const char *self) {
    FILE *f;
    const pid_t in = start_inner(self, "--inner-kill-helper", &f);
    CHECK(in > 0 && f, "inner not started");
    if (in <= 0 || !f) return;
    const long hp = read_pid(f);
    CHECK(hp > 0, "no helper pid");
    sleep_ms(300);   /* mid-stream: 16 MB frames are written continuously */
    const uint64_t t0 = r64_now_ns();
    if (hp > 0) kill((pid_t)hp, SIGKILL);
    char line[512];
    int fault = 0;
    unsigned long long td = 0;
    while (fgets(line, sizeof line, f)) {
        printf("  | %s", line);
        if (strstr(line, "=== FAULT (render64 helper exited: signal 9)")) fault = 1;
        sscanf(line, "detected_ns %llu", &td);
    }
    fclose(f);
    int st = 0;
    waitpid(in, &st, 0);
    const double ms = td ? (double)(int64_t)(td - t0) / 1e6 : -1;
    CHECK(fault, "no FAULT line");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "inner parent status %d", st);
    CHECK(td && ms >= 0 && ms < 100, "detection took %.1f ms", ms);
    CHECK(hp <= 0 || (kill((pid_t)hp, 0) && errno == ESRCH), "helper %ld still exists", hp);
    printf("helper killed mid-frame: the game detected it after %.2f ms, FAULT line, exit %d\n", ms, WEXITSTATUS(st));
}

static void test_kill_parent(const char *self) {
    FILE *f;
    const pid_t in = start_inner(self, "--inner-kill-parent", &f);
    CHECK(in > 0 && f, "inner not started");
    if (in <= 0 || !f) return;
    const long hp = read_pid(f);
    CHECK(hp > 0, "no helper pid");
    if (hp <= 0) {
        kill(in, SIGKILL);
        waitpid(in, NULL, 0);
        fclose(f);
        return;
    }
    const int pfd = (int)syscall(SYS_pidfd_open, (pid_t)hp, 0);
    CHECK(pfd >= 0, "pidfd_open(helper): %s", strerror(errno));
    sleep_ms(300);
    const uint64_t t0 = r64_now_ns();
    kill(in, SIGKILL);
    struct pollfd p = {pfd, POLLIN, 0};
    const int r = pfd >= 0 ? poll(&p, 1, 2000) : 0;
    const double ms = ms_since(t0);
    waitpid(in, NULL, 0);
    char line[512];
    while (fgets(line, sizeof line, f)) printf("  | %s", line);
    fclose(f);
    /* this process is the subreaper: the orphaned helper's zombie is ours to reap */
    int st = 0;
    const pid_t w = waitpid((pid_t)hp, &st, 0);
    CHECK(r == 1 && ms < 1000, "helper still alive %.0f ms after the parent died", ms);
    CHECK(w != (pid_t)hp || (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL), "helper status %d", st);
    if (pfd >= 0) close(pfd);
    printf("game killed: the helper was gone after %.2f ms (%s)\n", ms,
           w == (pid_t)hp && WIFSIGNALED(st) ? "SIGKILL by PR_SET_PDEATHSIG" : "reaped elsewhere");
}

static void test_no_leftovers(void) {
    /* as the subreaper, every orphaned descendant would be our child */
    int n = 0;
    pid_t w;
    while ((w = waitpid(-1, NULL, WNOHANG)) > 0) n++;
    CHECK(n == 0 && w == -1 && errno == ECHILD, "%d leftover processes (waitpid %d)", n, (int)w);
    printf("leftover processes: %d\n", n);
}
#endif

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint32_t frames = 10000, paced = 120;
    int no_kill = 0;
    char inner_mode = 0;
    static char def[1200];
    const char *slash = strrchr(argv[0], '/');
#ifdef _WIN32
    const char *bs = strrchr(argv[0], '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
    snprintf(def, sizeof def, "%.*s%sx64\\WarriorsRender64.exe", slash ? (int)(slash - argv[0]) : 0, argv[0], slash ? "\\" : "");
#else
    /* build/render_ipc_test and build64/render_ipc_test both find build64/xbrun-render64 */
    snprintf(def, sizeof def, "%.*s%s../build64/xbrun-render64", slash ? (int)(slash - argv[0]) : 0, argv[0], slash ? "/" : "");
#endif
    helper = def;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--helper") && i + 1 < argc) helper = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--paced") && i + 1 < argc) paced = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-kill")) no_kill = 1;
        else if (!strcmp(argv[i], "--inner-kill-helper")) inner_mode = 'h';
        else if (!strcmp(argv[i], "--inner-kill-parent")) inner_mode = 'p';
        else {
            fprintf(stderr, "usage: render_ipc_test [--helper PATH] [--frames N] [--paced N] [--no-kill]\n");
            return 2;
        }
    }
    if (frames < 1) frames = 1;
    make_source(16u << 20);
#ifndef _WIN32
    if (inner_mode) return inner(inner_mode);
    /* orphans (a helper whose game died) become our children: reaped and counted */
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    char self[1024];
    const ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    self[sl > 0 ? sl : 0] = 0;
#else
    (void)inner_mode;
#endif
    printf("render_ipc_test: %d-bit, helper %s\n", (int)(sizeof(void *) * 8), helper);
    test_sum();
    test_spawn_failures();
    test_mismatch();
    test_handshake_and_quit();
    static const uint32_t sizes[] = {2u << 20, 4u << 20, 16u << 20};
    test_frames(sizes, 3, frames, paced);
#ifndef _WIN32
    if (!no_kill) {
        test_kill_helper(self);
        test_kill_parent(self);
    }
    test_no_leftovers();
#else
    (void)no_kill;   /* the crash paths on Windows run in W5 */
#endif
    printf("render_ipc_test: %d checks, %d failed\n", checks, failures);
    free(src);
    return failures ? 1 : 0;
}
