/* End-to-end test of --render-process helper-io: this i386 program is
 * the game side (video.c's external backend over src/r64_input.c, the real input_map.c and hle_input.c)
 * and starts the x86_64 helper (build64/xbrun-render64) on SDL's dummy video driver with --test-inject:
 * the helper pushes N key events with SDL_PushEvent at irregular intervals, plugs a virtual controller,
 * presses and releases A and closes its window. Checked here: every key reaches input_key_event in
 * order, the timing between keys is kept, the added latency (SDL_PushEvent in the helper -> applied in
 * the game) p95 < 0.5 ms, the controller press reaches port 0, the game's rumble reaches the virtual
 * controller, the close ends video_pump and the helper exits 0; a killed helper gives the FAULT line and
 * video_pump 0; a game that dies takes its helper with it.
 * Prints SKIP and exits 0 when the helper cannot open a window (no x86_64 SDL2). */
#define _GNU_SOURCE
#include "../src/hle_input.h"
#include "../src/input_map.h"
#include "../src/r64_input.h"
#include "../src/video.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define NKEYS 600
static const char *helper = "build64/xbrun-render64";
static struct { int key, down; uint64_t t, o, now; } keys[NKEYS + 8];
static int nkeys, hook_keys[NKEYS + 8][2], nhook, rumble_seen, rumble_lo, rumble_hi, no_virtual, quit_seen, pads;
static void on_record(const R64InEvent *e, uint64_t now) {
    if (e->type == R64_IN_KEY && nkeys < NKEYS + 8) {
        keys[nkeys].key = e->v[0];
        keys[nkeys].down = e->arg;
        keys[nkeys].t = (uint64_t)e->t_hi << 32 | e->t_lo;
        keys[nkeys].o = (uint64_t)e->o_hi << 32 | e->o_lo;
        keys[nkeys].now = now;
        nkeys++;
    }
    if (e->type == R64_IN_TEST && e->arg == 1) rumble_seen = 1, rumble_lo = e->v[1], rumble_hi = e->v[2];
    if (e->type == R64_IN_TEST && e->arg == 2) no_virtual = 1;
    if (e->type == R64_IN_QUIT) quit_seen = 1;
    if (e->type == R64_IN_PAD) pads++;
}
static void on_key(int key, int down) {
    if (nhook < NKEYS + 8) hook_keys[nhook][0] = key, hook_keys[nhook][1] = down, nhook++;
}
static int cmp_d(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int test_inject(void) {
    char *argv[] = {"x", "--render-process", "helper-io", "--render64-helper", (char *)helper, "--render64-inject", "600", NULL};
    for (int i = 1; argv[i]; i++) r64_input_option(7, argv, &i);
    input_key_hook = on_key;
    r64_input_test_hook = on_record;
    const char *b = video_open("null", 640, 360, "r64_input_test");
    if (strcmp(b, "helper")) {
        printf("SKIP: the helper opened no window (backend %s)\n", b);
        return 1;
    }
    int a_down = 0, a_up = 0, connected = 0;
    const double t0 = sec();
    int pumped = 1;
    while ((pumped = video_pump()) && sec() - t0 < 30) {   /* busy: the read time is the arrival time */
        XPad p;
        uint32_t pk;
        input_get_port(0, &p, &pk);
        if (input_controller_connected()) connected = 1;
        if (!a_down && connected && p.analog[IN_A - IN_A] == 0xFF) {   /* the controller's A (keys may be bound to A too) */
            a_down = 1;
            input_rumble_hook(0, 0x1234, 0x5678);   /* as XInputSetState does */
        }
        if (a_down && !a_up && p.analog[IN_A - IN_A] == 0) a_up = 1;
    }
    R64InputStats s;
    r64_input_stats(&s);
    video_close();
    CHECK(!pumped, "the window close ended video_pump (after %.1f s)", sec() - t0);
    CHECK(quit_seen, "the close arrived as a QUIT record");
    CHECK(r64_input_last_exit == 0, "the helper exited 0 when the game quit (%d)", r64_input_last_exit);

    int order = nkeys == NKEYS, hook_order = nhook == NKEYS;
    for (int i = 0; i < nkeys && i < NKEYS; i++) order &= keys[i].key == 'a' + (i / 2) % 26 && keys[i].down == !(i & 1) && keys[i].o;
    for (int i = 0; i < nhook && i < NKEYS; i++) hook_order &= hook_keys[i][0] == 'a' + (i / 2) % 26 && hook_keys[i][1] == !(i & 1);
    CHECK(order, "%d of %d key records, in the injected order, each with its injection time", nkeys, NKEYS);
    CHECK(hook_order, "%d key changes reached input_key_event in order (input_key_hook)", nhook);
    static double lat[NKEYS], helper_part[NKEYS], skew[NKEYS];
    int n = 0;
    for (int i = 0; i < nkeys && i < NKEYS; i++, n++) {
        lat[n] = (double)(keys[i].now - keys[i].o) / 1e6;
        helper_part[n] = (double)(keys[i].t - keys[i].o) / 1e6;
        const double di = i ? (double)(keys[i].o - keys[i - 1].o) / 1e6 : 0, dr = i ? (double)(keys[i].now - keys[i - 1].now) / 1e6 : 0;
        skew[n] = dr > di ? dr - di : di - dr;
    }
    if (n) {
        qsort(lat, n, sizeof *lat, cmp_d);
        qsort(helper_part, n, sizeof *helper_part, cmp_d);
        qsort(skew, n, sizeof *skew, cmp_d);
        printf("     added latency (SDL_PushEvent -> applied in the game) ms: p50 %.3f p95 %.3f p99 %.3f max %.3f\n", lat[n / 2], lat[n * 95 / 100],
               lat[n * 99 / 100], lat[n - 1]);
        printf("     of which in the helper (push -> ring) ms: p50 %.3f p95 %.3f max %.3f; game read after the ring write p50 %.3f ms\n",
               helper_part[n / 2], helper_part[n * 95 / 100], helper_part[n - 1], s.wait_p50);
        printf("     interval skew between consecutive keys ms: p50 %.3f p95 %.3f max %.3f\n", skew[n / 2], skew[n * 95 / 100], skew[n - 1]);
        CHECK(lat[n * 95 / 100] < 0.5, "added latency p95 %.3f ms < 0.5 ms", lat[n * 95 / 100]);
        CHECK(skew[n * 95 / 100] < 0.5, "timing kept: the interval between keys changes by %.3f ms at p95", skew[n * 95 / 100]);
    }
    if (no_virtual) {
        printf("SKIP the controller part: the helper's SDL2 has no virtual joysticks\n");
    } else {
        CHECK(connected && a_down && a_up, "virtual controller: connected %d, A pressed %d and released %d on port 0 (%d pad records)", connected, a_down,
              a_up, pads);
        CHECK(rumble_seen && rumble_lo == 0x1234 && rumble_hi == 0x5678, "the game's rumble reached the controller in the helper (%04x %04x)", rumble_lo,
              rumble_hi);
    }
    CHECK(s.dropped == 0 && s.resyncs == 0, "no records dropped (%u)", s.dropped);
    return 0;
}

/* The helper killed while the game runs: FAULT line, video_pump 0, and it stays 0. */
static void test_helper_killed(void) {
    nkeys = nhook = 0;
    r64_input_inject = NULL;
    const char *b = video_open("null", 320, 240, "r64_input_test");
    if (strcmp(b, "helper")) { CHECK(0, "helper for the kill test (%s)", b); return; }
    const long pid = r64_input_helper_pid();
    const double t0 = sec();
    kill((pid_t)pid, SIGKILL);
    int r = 1;
    while ((r = video_pump()) && sec() - t0 < 2) {}
    const double ms = (sec() - t0) * 1e3;
    CHECK(!r && !video_pump(), "helper killed: video_pump 0 after %.2f ms (FAULT line above)", ms);
    CHECK(ms < 100, "detected in under 100 ms");
    video_close();
}

/* The helper idle (no input, no controller, the game pumping every 4 ms as hle_d3d does): how often its pump thread
 * (the main thread) wakes, and the CPU of that thread (/proc/PID/schedstat, ns) and of the whole process (/proc/PID/stat)
 * over 2 s. The pump waits for window input instead of polling every 250 us: about
 * 250 wakes/s instead of 3000+. */
static double task_cpu_s(long pid) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%ld/schedstat", pid);
    FILE *f = fopen(p, "r");
    unsigned long long ns = 0;
    if (f) {
        if (fscanf(f, "%llu", &ns) != 1) ns = 0;
        fclose(f);
    }
    return ns / 1e9;
}
static long task_wakes(long pid) {   /* voluntary context switches of the pump thread: its sleeps */
    char p[80], line[256];
    snprintf(p, sizeof p, "/proc/%ld/task/%ld/status", pid, pid);
    FILE *f = fopen(p, "r");
    long n = -1;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "voluntary_ctxt_switches:", 24)) n = strtol(line + 24, NULL, 10);
    if (f) fclose(f);
    return n;
}
static double proc_cpu_s(long pid) {
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%ld/stat", pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *s = strrchr(buf, ')');   /* fields after the command name: 3 state ... 14 utime 15 stime */
    unsigned long long ut = 0, st = 0;
    if (!s || sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return 0;
    return (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
}
static void test_idle_cpu(void) {
    r64_input_inject = NULL;
    r64_input_test_hook = NULL;
    const char *b = video_open("null", 320, 240, "r64_input_test");
    if (strcmp(b, "helper")) { CHECK(0, "helper for the idle test (%s)", b); return; }
    const long pid = r64_input_helper_pid();
    const struct timespec frame = {0, 4000000};
    for (double t = sec(); sec() - t < 0.3;) { video_pump(); nanosleep(&frame, NULL); }   /* settle after the window opened */
    const long w0 = task_wakes(pid);
    const double t0 = sec(), th0 = task_cpu_s(pid), pr0 = proc_cpu_s(pid);
    int up = 1;
    while ((up = video_pump()) && sec() - t0 < 2.0) nanosleep(&frame, NULL);
    const double secs = sec() - t0, th = (task_cpu_s(pid) - th0) / secs * 100, pr = (proc_cpu_s(pid) - pr0) / secs * 100;
    const double wakes = (double)(task_wakes(pid) - w0) / secs;
    printf("     helper idle over %.1f s: pump thread %.0f wakes/s, %.2f%% of one core, whole process %.1f%%\n", secs, wakes, th, pr);
    CHECK(up, "the helper stayed up while idle");
    CHECK(w0 >= 0 && wakes < 500, "idle pump thread wakes %.0f/s < 500/s (it waits for input)", wakes);
    CHECK(th < 2.0, "idle pump thread CPU %.2f%% < 2%% of one core", th);
    video_close();
}

/* The game dies: its helper goes too (PR_SET_PDEATHSIG; this process is the subreaper, so the
 * orphaned helper would be our child). */
static void test_game_killed(void) {
    fflush(stdout);
    const pid_t child = fork();
    if (!child) {
        r64_input_test_hook = NULL;
        const char *b = video_open("null", 320, 240, "r64_input_test");
        printf("     child: backend %s, helper pid %ld; SIGKILL to itself\n", b, r64_input_helper_pid());
        fflush(stdout);
        raise(SIGKILL);
        _exit(1);
    }
    int st;
    waitpid(child, &st, 0);
    const double t0 = sec();
    int left = 1;
    while (sec() - t0 < 2) {
        const pid_t w = waitpid(-1, &st, WNOHANG);
        if (w < 0) { left = 0; break; }   /* no children left */
        if (w == 0) nanosleep(&(struct timespec){0, 1000000}, NULL);
    }
    CHECK(WIFSIGNALED(st) || !left, "game killed: its helper is gone after %.1f ms", (sec() - t0) * 1e3);
    CHECK(!left, "no leftover processes");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--helper")) helper = argv[++i];
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    input_map_defaults();
    printf("r64_input_test: helper %s\n", helper);
    if (test_inject()) return 0;
    test_helper_killed();
    test_idle_cpu();
    test_game_killed();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
