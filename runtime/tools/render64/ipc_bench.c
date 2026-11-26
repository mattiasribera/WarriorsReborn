/* Shared-memory ring between two processes (stage 2a): the producer (the
 * game, i386) streams frames of a given size into a ring that the consumer (the renderer
 * process, x86_64) reads, the way a renderer.h proxy would. Linux: memfd + futex on words of
 * the shared mapping; Windows: a pagefile-backed file mapping + auto-reset events (WaitOnAddress
 * only works inside one process). Each side spins briefly before it sleeps and only signals a
 * side that said it sleeps.
 *
 * usage: ipc_bench [--consumer PATH] [--mb LIST] [--frames N] [--rate HZ] [--ring-mb R] [--chunk-kb C] [--spin-us S]
 *   --consumer  the consumer binary (default: ipc_bench64[.exe] next to this one)
 *   --mb        frame sizes in MB, comma-separated (default 2,5,10)
 *   --rate      frames per second (default 60; 0 = back to back, the throughput)
 * Prints per size: the producer's time to push a frame, the time from its last byte to the
 * consumer having copied the whole frame out (the added latency), and the ping-pong round trip. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define MAX_FRAMES 4096
#define PINGS 20000
enum { EV_DATA, EV_SPACE, EV_PING, EV_PONG, EV_COUNT };

typedef struct {
    uint32_t magic, ring, chunk, frame_bytes, frames, spin_us, phase;   /* phase: 1 ping-pong, 2 frames, 3 quit */
    uint32_t pad0[9];
    volatile uint32_t head;          /* producer: bytes written (mod 2^32) */
    uint32_t pad1[15];
    volatile uint32_t tail;          /* consumer: bytes read */
    uint32_t pad2[15];
    volatile uint32_t seq[EV_COUNT];   /* futex words, one per event */
    uint32_t pad3[12];
    volatile uint32_t waiting[EV_COUNT];   /* the side waiting on that event */
    uint32_t pad4[12];
    volatile uint32_t ready, ping, pong;
    uint32_t pad5[13];
    double t_done[MAX_FRAMES];       /* consumer: frame copied out (ms, shared clock) */
    double consumer_ms;              /* consumer: time spent copying */
} Shared;
#define HDR_BYTES ((sizeof(Shared) + 4095) & ~(size_t)4095)

static Shared *S;
static uint8_t *ring;

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e3 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#endif
}
static void sleep_until(double t) {
    for (;;) {
        const double d = t - now_ms();
        if (d <= 0) return;
#ifdef _WIN32
        if (d > 2) Sleep((DWORD)(d - 1));
#else
        if (d > 0.2) {
            struct timespec ts = {0, (long)((d - 0.1) * 1e6)};
            nanosleep(&ts, NULL);
        }
#endif
    }
}

#ifdef _WIN32
static HANDLE ev[EV_COUNT];
static void ev_wait(int e, uint32_t seen) {
    if (S->seq[e] == seen) WaitForSingleObject(ev[e], 100);
}
static void ev_wake(int e) { SetEvent(ev[e]); }
#else
static void ev_wait(int e, uint32_t seen) {
    struct timespec to = {0, 100 * 1000000};
    syscall(SYS_futex, &S->seq[e], FUTEX_WAIT, seen, &to, NULL, 0);
}
static void ev_wake(int e) { syscall(SYS_futex, &S->seq[e], FUTEX_WAKE, 1, NULL, NULL, 0); }
#endif

/* Waits until ok() holds: spins spin_us, then sleeps on event e (the other side wakes it). */
static void wait_for(int e, int (*ok)(void)) {
    if (ok()) return;
    const double until = now_ms() + S->spin_us / 1000.0;
    while (now_ms() < until)
        if (ok()) return;
    for (;;) {
        const uint32_t seen = __atomic_load_n(&S->seq[e], __ATOMIC_SEQ_CST);
        __atomic_store_n(&S->waiting[e], 1, __ATOMIC_SEQ_CST);
        if (ok()) break;
        ev_wait(e, seen);
        if (ok()) break;
    }
    __atomic_store_n(&S->waiting[e], 0, __ATOMIC_SEQ_CST);
}
static void signal_ev(int e) {
    __atomic_add_fetch(&S->seq[e], 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&S->waiting[e], __ATOMIC_SEQ_CST)) ev_wake(e);
}

/* ---- consumer ---- */
static uint32_t want_bytes;
static int data_ready(void) { return __atomic_load_n(&S->head, __ATOMIC_ACQUIRE) - S->tail >= want_bytes; }
static uint32_t ping_want;
static int ping_ready(void) { return __atomic_load_n(&S->ping, __ATOMIC_ACQUIRE) == ping_want; }
static int phase_set(void) { return __atomic_load_n(&S->phase, __ATOMIC_ACQUIRE) != 0; }
static int consumer(void) {
    uint8_t *dst = NULL;
    size_t dst_cap = 0;
    __atomic_store_n(&S->ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        wait_for(EV_PING, phase_set);
        const uint32_t phase = S->phase;
        if (phase == 3) break;
        if (phase == 1) {
            for (uint32_t i = 1; i <= PINGS; i++) {
                ping_want = i;
                wait_for(EV_PING, ping_ready);
                __atomic_store_n(&S->pong, i, __ATOMIC_RELEASE);
                signal_ev(EV_PONG);
            }
        } else if (phase == 2) {
            if (S->frame_bytes > dst_cap) {
                free(dst);
                dst = malloc(S->frame_bytes);
                dst_cap = S->frame_bytes;
                memset(dst, 1, dst_cap);
            }
            double busy = 0;
            for (uint32_t f = 0; f < S->frames; f++) {
                for (uint32_t got = 0; got < S->frame_bytes;) {
                    const uint32_t n = S->frame_bytes - got < S->chunk ? S->frame_bytes - got : S->chunk;
                    want_bytes = S->chunk;   /* every chunk takes a whole slot: none straddles the ring's end */
                    wait_for(EV_DATA, data_ready);
                    const double t = now_ms();
                    memcpy(dst + got, ring + (S->tail % S->ring), n);
                    busy += now_ms() - t;
                    got += n;
                    __atomic_store_n(&S->tail, S->tail + S->chunk, __ATOMIC_RELEASE);
                    signal_ev(EV_SPACE);
                }
                S->t_done[f] = now_ms();
            }
            S->consumer_ms = busy;
        }
        __atomic_store_n(&S->phase, 0, __ATOMIC_RELEASE);
        signal_ev(EV_PONG);   /* phase finished */
    }
    free(dst);
    return 0;
}

/* ---- producer ---- */
static int space_ready(void) { return S->ring - (S->head - __atomic_load_n(&S->tail, __ATOMIC_ACQUIRE)) >= want_bytes; }
static uint32_t pong_want;
static int pong_ready(void) { return __atomic_load_n(&S->pong, __ATOMIC_ACQUIRE) == pong_want; }
static int phase_idle(void) { return __atomic_load_n(&S->phase, __ATOMIC_ACQUIRE) == 0; }
static int is_ready(void) { return __atomic_load_n(&S->ready, __ATOMIC_ACQUIRE) != 0; }
static void start_phase(uint32_t p) {
    __atomic_store_n(&S->phase, p, __ATOMIC_RELEASE);
    signal_ev(EV_PING);
}
static int cmp_d(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double pct(double *v, int n, int p) { return v[(size_t)n * p / 100 < (size_t)n ? (size_t)n * p / 100 : (size_t)n - 1]; }

static void run_pings(void) {
    static double rtt[PINGS];
    S->ping = S->pong = 0;
    start_phase(1);
    for (uint32_t i = 1; i <= PINGS; i++) {
        const double t = now_ms();
        __atomic_store_n(&S->ping, i, __ATOMIC_RELEASE);
        signal_ev(EV_PING);
        pong_want = i;
        wait_for(EV_PONG, pong_ready);
        rtt[i - 1] = (now_ms() - t) * 1000;
    }
    wait_for(EV_PONG, phase_idle);
    qsort(rtt, PINGS, sizeof *rtt, cmp_d);
    printf("ping-pong round trip (spin %u us): p50 %.1f us, p99 %.1f us, max %.1f us\n", S->spin_us, pct(rtt, PINGS, 50), pct(rtt, PINGS, 99),
           rtt[PINGS - 1]);
}
static void run_frames(uint32_t bytes, uint32_t frames, double rate) {
    static double t_start[MAX_FRAMES], t_pushed[MAX_FRAMES];
    uint8_t *src = malloc(bytes), *dst = malloc(bytes);
    for (uint32_t i = 0; i < bytes; i++) src[i] = (uint8_t)(i * 2654435761u >> 24);
    memset(dst, 0, bytes);
    /* the same copy inside one process, for scale */
    double local = 0;
    for (int k = 0; k < 8; k++) {
        const double t = now_ms();
        memcpy(dst, src, bytes);
        __asm__ volatile("" ::"r"(dst) : "memory");   /* the copy is used */
        local += now_ms() - t;
    }
    local /= 8;
    S->frame_bytes = bytes;
    S->frames = frames;
    S->head = S->tail = 0;
    start_phase(2);
    double next = now_ms() + 2;
    for (uint32_t f = 0; f < frames; f++) {
        if (rate > 0) { sleep_until(next); next += 1000.0 / rate; }
        t_start[f] = now_ms();
        for (uint32_t put = 0; put < bytes;) {
            const uint32_t n = bytes - put < S->chunk ? bytes - put : S->chunk;
            want_bytes = S->chunk;
            wait_for(EV_SPACE, space_ready);
            memcpy(ring + (S->head % S->ring), src + put, n);
            put += n;
            __atomic_store_n(&S->head, S->head + S->chunk, __ATOMIC_RELEASE);
            signal_ev(EV_DATA);
        }
        t_pushed[f] = now_ms();
    }
    wait_for(EV_PONG, phase_idle);
    const double total = t_pushed[frames - 1] - t_start[0];
    static double push[MAX_FRAMES], lag[MAX_FRAMES], e2e[MAX_FRAMES];
    const uint32_t from = frames > 10 ? 5 : 0, n = frames - from;   /* the first frames warm up */
    for (uint32_t f = from; f < frames; f++) {
        push[f - from] = t_pushed[f] - t_start[f];
        lag[f - from] = (S->t_done[f] - t_pushed[f]) * 1000;
        e2e[f - from] = S->t_done[f] - t_start[f];
    }
    qsort(push, n, sizeof *push, cmp_d);
    qsort(lag, n, sizeof *lag, cmp_d);
    qsort(e2e, n, sizeof *e2e, cmp_d);
    printf("%5.1f MB x %u at %s: push p50 %.3f ms p99 %.3f; consumer done after the last byte p50 %.0f us p99 %.0f; "
           "frame start to consumer done p50 %.3f ms p99 %.3f; in-process memcpy %.3f ms; ",
           bytes / 1048576.0, frames, rate > 0 ? "60 Hz" : "full speed", pct(push, n, 50), pct(push, n, 99), pct(lag, n, 50), pct(lag, n, 99),
           pct(e2e, n, 50), pct(e2e, n, 99), local);
    if (rate > 0) printf("consumer copy %.3f ms/frame\n", S->consumer_ms / frames);
    else printf("throughput %.2f GB/s\n", (double)bytes * frames / (total / 1000) / 1e9);
    free(src);
    free(dst);
}

int main(int argc, char **argv) {
    const char *consumer_path = NULL, *mb = "2,5,10";
    uint32_t frames = 600, ring_mb = 64, chunk_kb = 256, spin_us = 50;
    double rate = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--child") && i + 1 < argc) {
#ifdef _WIN32
            unsigned long long hm, e[EV_COUNT];
            if (sscanf(argv[i + 1], "%llx:%llx:%llx:%llx:%llx", &hm, &e[0], &e[1], &e[2], &e[3]) != 5) return 2;
            for (int k = 0; k < EV_COUNT; k++) ev[k] = (HANDLE)(uintptr_t)e[k];
            void *base = MapViewOfFile((HANDLE)(uintptr_t)hm, FILE_MAP_ALL_ACCESS, 0, 0, 0);
#else
            const int fd = atoi(argv[i + 1]);
            const off_t size = lseek(fd, 0, SEEK_END);
            void *base = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED) base = NULL;
#endif
            if (!base) { fprintf(stderr, "ipc_bench: child cannot map the ring\n"); return 2; }
            S = base;
            ring = (uint8_t *)base + HDR_BYTES;
            return consumer();
        }
        if (!strcmp(argv[i], "--consumer") && i + 1 < argc) consumer_path = argv[++i];
        else if (!strcmp(argv[i], "--mb") && i + 1 < argc) mb = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ring-mb") && i + 1 < argc) ring_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chunk-kb") && i + 1 < argc) chunk_kb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spin-us") && i + 1 < argc) spin_us = (uint32_t)atoi(argv[++i]);
        else { fprintf(stderr, "usage: ipc_bench [--consumer PATH] [--mb LIST] [--frames N] [--rate HZ] [--ring-mb R] [--chunk-kb C] [--spin-us S]\n"); return 2; }
    }
    if (frames < 1 || frames > MAX_FRAMES) frames = MAX_FRAMES;
    const size_t ring_bytes = (size_t)ring_mb << 20, total = HDR_BYTES + ring_bytes;
    char self_dir[1024] = ".", path[1100];
    const char *slash = strrchr(argv[0], '/');
#ifdef _WIN32
    const char *bs = strrchr(argv[0], '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (slash) snprintf(self_dir, sizeof self_dir, "%.*s", (int)(slash - argv[0]), argv[0]);
#ifdef _WIN32
    if (!consumer_path) { snprintf(path, sizeof path, "%s\\ipc_bench64.exe", self_dir); consumer_path = path; }
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)total, NULL);
    void *base = hm ? MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    for (int k = 0; k < EV_COUNT; k++) ev[k] = CreateEventA(&sa, FALSE, FALSE, NULL);
    if (!base) { fprintf(stderr, "ipc_bench: no file mapping\n"); return 2; }
    memset(base, 0, HDR_BYTES);
    S = base;
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "\"%s\" --child %llx:%llx:%llx:%llx:%llx", consumer_path, (unsigned long long)(uintptr_t)hm,
             (unsigned long long)(uintptr_t)ev[0], (unsigned long long)(uintptr_t)ev[1], (unsigned long long)(uintptr_t)ev[2],
             (unsigned long long)(uintptr_t)ev[3]);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "ipc_bench: cannot start %s (%lu)\n", consumer_path, GetLastError());
        return 2;
    }
#else
    if (!consumer_path) { snprintf(path, sizeof path, "%s/ipc_bench64", self_dir); consumer_path = path; }
    const int fd = (int)syscall(SYS_memfd_create, "ipc_bench", 0);
    if (fd < 0 || ftruncate(fd, (off_t)total)) { perror("memfd"); return 2; }
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 2; }
    S = base;
    const pid_t pid = fork();
    if (!pid) {
        char fds[16];
        snprintf(fds, sizeof fds, "%d", fd);
        execl(consumer_path, consumer_path, "--child", fds, (char *)NULL);
        perror(consumer_path);
        _exit(2);
    }
#endif
    ring = (uint8_t *)base + HDR_BYTES;
    S->magic = 0x52363442u;
    S->ring = (uint32_t)ring_bytes;
    S->chunk = chunk_kb << 10;
    S->spin_us = spin_us;
    memset(ring, 0, ring_bytes);   /* both sides' pages faulted in before timing */
    wait_for(EV_PONG, is_ready);
    printf("producer %d-bit, consumer %s; ring %u MB, chunks %u KB\n", (int)(sizeof(void *) * 8), consumer_path, ring_mb, chunk_kb);
    run_pings();
    const uint32_t spin = S->spin_us;
    S->spin_us = 0;
    run_pings();
    S->spin_us = spin;
    for (const char *p = mb; *p;) {
        const double m = atof(p);
        if (m > 0) {
            const uint32_t bytes = (uint32_t)(m * 1048576.0);
            run_frames(bytes, frames, rate);
            run_frames(bytes, frames < 300 ? frames : 300, 0);
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    start_phase(3);
#ifdef _WIN32
    WaitForSingleObject(pi.hProcess, 5000);
#else
    waitpid(pid, NULL, 0);
#endif
    return 0;
}
