/* render_ipc: see render_ipc.h. */
#define _GNU_SOURCE
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#endif
#include "render_ipc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434   /* the same number on i386 and x86_64 */
#endif
extern char **environ;
#endif

/* The control block has the same layout in both builds (i386 parent, x86_64 helper). */
_Static_assert(sizeof(R64Control) == R64_CONTROL_BYTES, "control block size");
_Static_assert(sizeof(R64ControlFields) <= R64_CONTROL_BYTES, "control block fields");
_Static_assert(offsetof(R64ControlFields, region) == 48, "header layout");
_Static_assert(offsetof(R64ControlFields, hello) == 128, "hello offset is frozen");
_Static_assert(offsetof(R64ControlFields, h_device) == 192, "hello layout");
_Static_assert(offsetof(R64ControlFields, refuse) == 256, "refuse offset is frozen");
_Static_assert(offsetof(R64ControlFields, state) == 512, "state offset");
_Static_assert(offsetof(R64ControlFields, ring) == 704, "ring offset");
_Static_assert(sizeof(R64EchoFrame) == 16 && sizeof(R64EchoAck) == 32, "echo records");

#define SLICE_MS 10   /* a sleeping side checks the peer at least this often */

struct R64Ipc {
    int role;
    int loopback;          /* r64_loopback: no peer process */
    struct R64Ipc *twin;   /* r64_loopback: the other view of the same mapping */
    R64Control *ctl;
    uint8_t *base;
    size_t total;
    uint32_t spin_us;
    struct {
        uint8_t *data;
        uint32_t size, mask, pos, pub;   /* pos: writer's written / reader's consumed position */
    } ring[R64_RING_COUNT];
    int peer_dead;
    int exit_kind, exit_val;   /* 0 running, 1 exit code, 2 signal, 3 gone (unknown status) */
#ifdef _WIN32
    HANDLE map, ev[R64_EV_COUNT], peer, job;
    HANDLE log_rd, log_thread;   /* parent: the helper's stdout/stderr pipe and the thread printing it */
#else
    long pid;   /* parent: the helper; helper: the parent */
    int pidfd;
#endif
};

static const char *g_prefix = "";

/* ---- small helpers ---- */

uint64_t r64_now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

R64Ipc *r64_shared_channel;
int r64_helper_render, r64_helper_hash, r64_fault_printed;
uint32_t r64_helper_schema, r64_helper_frames = 2;

void r64_log(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* every line gets the prefix; the whole message goes out in one write */
    char out[1400];
    size_t o = 0;
    const char *p = msg;
    while (*p && o + 1 < sizeof out) {
        const char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const int w = snprintf(out + o, sizeof out - o, "%s%.*s\n", g_prefix, (int)len, p);
        if (w < 0) break;
        o += (size_t)w < sizeof out - o ? (size_t)w : sizeof out - o - 1;
        p += len + (nl ? 1 : 0);
    }
    printf("%s", out);
    fflush(stdout);
}

static void set_err(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---- the helper's output (render_ipc.h r64_lines_*) ---- */
static void lines_emit(R64Lines *s, R64LineFn fn, void *ctx) {
    while (s->n && s->buf[s->n - 1] == '\r') s->n--;
    s->buf[s->n] = 0;
    char line[sizeof s->buf + 8];
    snprintf(line, sizeof line, "%s%s", strncmp(s->buf, "[r64]", 5) ? "[r64] " : "", s->buf);
    fn(ctx, line);
    s->n = 0;
}
void r64_lines_feed(R64Lines *s, const char *p, size_t n, R64LineFn fn, void *ctx) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {
            lines_emit(s, fn, ctx);
            continue;
        }
        if (s->n + 1 >= sizeof s->buf) lines_emit(s, fn, ctx);
        s->buf[s->n++] = p[i];
    }
}
void r64_lines_flush(R64Lines *s, R64LineFn fn, void *ctx) {
    if (s->n) lines_emit(s, fn, ctx);
}

static uint32_t layout_hash(void) {
    /* sizes and offsets of the fields both sides touch: a change in either build is a mismatch */
    const uint32_t v[] = {(uint32_t)sizeof(R64Control),
                          (uint32_t)offsetof(R64ControlFields, state),
                          (uint32_t)offsetof(R64ControlFields, seq),
                          (uint32_t)offsetof(R64ControlFields, waiting),
                          (uint32_t)offsetof(R64ControlFields, ring),
                          (uint32_t)sizeof(((R64ControlFields *)0)->ring),
                          (uint32_t)offsetof(R64ControlFields, frames_sent),
                          (uint32_t)sizeof(R64EchoFrame),
                          (uint32_t)sizeof(R64EchoAck),
                          R64_REG_COUNT,
                          R64_EV_COUNT,
                          R64_RING_COUNT};
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) h = (h ^ v[i]) * 16777619u;
    return h;
}

static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* ---- the checksum ---- */

void r64_sum_init(R64Sum *s) {
    for (int i = 0; i < 4; i++) s->h[i] = 2166136261u + (uint32_t)i * 0x9e3779b9u;
    s->words = 0;
}
void r64_sum_update(R64Sum *s, const void *p, uint32_t n) {
    const uint8_t *b = p;
    uint32_t w = s->words, k = n / 4;
    uint32_t h0 = s->h[0], h1 = s->h[1], h2 = s->h[2], h3 = s->h[3];
    uint32_t v;
    /* until lane 0 lines up, one word at a time */
    while (k && (w & 3)) {
        memcpy(&v, b, 4);
        switch (w & 3) {
        case 1: h1 = (h1 ^ v) * 16777619u; break;
        case 2: h2 = (h2 ^ v) * 16777619u; break;
        default: h3 = (h3 ^ v) * 16777619u; break;
        }
        b += 4, w++, k--;
    }
    for (; k >= 4; k -= 4, b += 16, w += 4) {
        uint32_t a[4];
        memcpy(a, b, 16);
        h0 = (h0 ^ a[0]) * 16777619u;
        h1 = (h1 ^ a[1]) * 16777619u;
        h2 = (h2 ^ a[2]) * 16777619u;
        h3 = (h3 ^ a[3]) * 16777619u;
    }
    for (; k; k--, b += 4, w++) {
        memcpy(&v, b, 4);
        switch (w & 3) {
        case 0: h0 = (h0 ^ v) * 16777619u; break;
        case 1: h1 = (h1 ^ v) * 16777619u; break;
        default: h2 = (h2 ^ v) * 16777619u; break;   /* at most 3 words remain: lanes 0-2 */
        }
    }
    s->h[0] = h0, s->h[1] = h1, s->h[2] = h2, s->h[3] = h3;
    s->words = w;
}
uint32_t r64_sum_final(const R64Sum *s) {
    uint32_t h = s->words * 0x85ebca6bu;
    for (int i = 0; i < 4; i++) h = (h ^ s->h[i]) * 0xc2b2ae35u, h ^= h >> 15;
    return h;
}

/* ---- events and the peer ---- */

static R64ControlFields *F(R64Ipc *x) { return &x->ctl->f; }
static int other(R64Ipc *x) { return x->role == R64_PARENT ? R64_HELPER : R64_PARENT; }

static void record_status(R64Ipc *x, int kind, int val) {
    x->peer_dead = 1;
    if (!x->exit_kind) x->exit_kind = kind, x->exit_val = val;
}

#ifdef _WIN32
static void os_wake(R64Ipc *x, int e) { SetEvent(x->ev[e]); }
/* Sleeps on event e unless its word moved; returns 1 if the peer's process handle signalled. */
static int os_wait(R64Ipc *x, int e, uint32_t seen, uint32_t ms) {
    if (F(x)->seq[e] != seen) return 0;
    HANDLE h[2] = {x->ev[e], x->peer};
    const DWORD r = WaitForMultipleObjects(x->peer ? 2 : 1, h, FALSE, ms);
    return r == WAIT_OBJECT_0 + 1;
}
static void reap(R64Ipc *x) {
    DWORD code = 0;
    if (x->peer && GetExitCodeProcess(x->peer, &code)) record_status(x, 1, (int)code);
    else record_status(x, 3, 0);
}
static int peer_gone(R64Ipc *x) {
    if (x->peer_dead) return 1;
    if (x->loopback) return 0;
    if (F(x)->state[other(x)] == R64_STATE_DEAD) return record_status(x, 3, 0), 1;
    if (x->peer && WaitForSingleObject(x->peer, 0) == WAIT_OBJECT_0) return reap(x), 1;
    return 0;
}
#else
static void os_wake(R64Ipc *x, int e) { syscall(SYS_futex, &F(x)->seq[e], FUTEX_WAKE, 1, NULL, NULL, 0); }
static int os_wait(R64Ipc *x, int e, uint32_t seen, uint32_t ms) {
    struct timespec to = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    syscall(SYS_futex, &F(x)->seq[e], FUTEX_WAIT, seen, &to, NULL, 0);
    return 0;
}
static void reap(R64Ipc *x) {
    if (x->role != R64_PARENT) { record_status(x, 3, 0); return; }
    int st = 0;
    pid_t r;
    do r = waitpid((pid_t)x->pid, &st, 0);
    while (r < 0 && errno == EINTR);
    if (r == (pid_t)x->pid && WIFEXITED(st)) record_status(x, 1, WEXITSTATUS(st));
    else if (r == (pid_t)x->pid && WIFSIGNALED(st)) record_status(x, 2, WTERMSIG(st));
    else record_status(x, 3, 0);
}
static int peer_gone(R64Ipc *x) {
    if (x->peer_dead) return 1;
    if (x->loopback) return 0;
    if (x->role == R64_PARENT) {
        if (x->pidfd >= 0) {
            struct pollfd p = {x->pidfd, POLLIN, 0};
            if (poll(&p, 1, 0) > 0) return reap(x), 1;
        } else {
            int st = 0;
            const pid_t r = waitpid((pid_t)x->pid, &st, WNOHANG);
            if (r == (pid_t)x->pid) {
                if (WIFSIGNALED(st)) record_status(x, 2, WTERMSIG(st));
                else record_status(x, 1, WEXITSTATUS(st));
                return 1;
            }
        }
        /* a DEAD flag without an exit yet: the helper is on its way out; the pidfd tells the status */
        return 0;
    }
    if ((long)getppid() != x->pid || F(x)->state[other(x)] == R64_STATE_DEAD) return record_status(x, 3, 0), 1;
    return 0;
}
#endif

static void signal_ev(R64Ipc *x, int e) {
    __atomic_add_fetch(&F(x)->seq[e], 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&F(x)->waiting[e], __ATOMIC_SEQ_CST)) os_wake(x, e);
}

typedef int (*Pred)(R64Ipc *x, uint32_t arg);

/* Waits until ok(x, arg): spins spin_us, then sleeps on event e in SLICE_MS slices, checking the
 * peer after each. timeout_ms 0 = no limit. The helper also stops when the parent quit. */
static int wait_for(R64Ipc *x, int e, Pred ok, uint32_t arg, uint32_t timeout_ms) {
    if (ok(x, arg)) return R64_OK;
    if (x->peer_dead) return R64_ERR_PEER_DEAD;
    const uint64_t t0 = r64_now_ns(), spin_end = t0 + (uint64_t)x->spin_us * 1000u;
    while (r64_now_ns() < spin_end)
        if (ok(x, arg)) return R64_OK;
    const uint64_t deadline = timeout_ms ? t0 + (uint64_t)timeout_ms * 1000000u : 0;
    int rc = R64_OK;
    for (;;) {
        const uint32_t seen = __atomic_load_n(&F(x)->seq[e], __ATOMIC_SEQ_CST);
        __atomic_store_n(&F(x)->waiting[e], 1, __ATOMIC_SEQ_CST);
        if (ok(x, arg)) break;
        /* checked after `seen`: a QUIT stored later also moves the word, so the sleep cannot miss it */
        if (x->role == R64_HELPER && F(x)->state[R64_PARENT] == R64_STATE_QUIT) { rc = R64_ERR_QUIT; break; }
        uint32_t ms = SLICE_MS;
        if (deadline) {
            const uint64_t now = r64_now_ns();
            if (now >= deadline) { rc = R64_ERR_TIMEOUT; break; }
            if ((deadline - now) / 1000000u + 1 < ms) ms = (uint32_t)((deadline - now) / 1000000u + 1);
        }
        const int died = os_wait(x, e, seen, ms);
        if (ok(x, arg)) break;
        if (died || peer_gone(x)) {
            if (died) reap(x);
            rc = R64_ERR_PEER_DEAD;
            break;
        }
        if (x->role == R64_HELPER && F(x)->state[R64_PARENT] == R64_STATE_QUIT) { rc = R64_ERR_QUIT; break; }
    }
    __atomic_store_n(&F(x)->waiting[e], 0, __ATOMIC_SEQ_CST);
    return rc;
}

/* ---- rings ---- */

static int ring_writer(int ring) { return ring == R64_RING_CMD ? R64_PARENT : R64_HELPER; }
static int ev_data(int ring) { return ring == R64_RING_CMD ? R64_EV_CMD_DATA : R64_EV_BACK_DATA; }
static int ev_space(int ring) { return ring == R64_RING_CMD ? R64_EV_CMD_SPACE : R64_EV_BACK_SPACE; }

static uint32_t space_of(R64Ipc *x, int r) {
    return x->ring[r].size - (x->ring[r].pos - __atomic_load_n(&F(x)->ring[r].tail, __ATOMIC_ACQUIRE));
}
static int has_space(R64Ipc *x, uint32_t arg) { return space_of(x, (int)(arg & 1)) >= (arg >> 1); }
static uint32_t avail_of(R64Ipc *x, int r) { return __atomic_load_n(&F(x)->ring[r].head, __ATOMIC_ACQUIRE) - x->ring[r].pos; }
static int has_data(R64Ipc *x, uint32_t arg) { return avail_of(x, (int)(arg & 1)) >= (arg >> 1); }

static void publish(R64Ipc *x, int r) {
    if (x->ring[r].pub == x->ring[r].pos) return;
    __atomic_store_n(&F(x)->ring[r].head, x->ring[r].pos, __ATOMIC_RELEASE);
    x->ring[r].pub = x->ring[r].pos;
    signal_ev(x, ev_data(r));
}

int r64_write(R64Ipc *x, int r, const void *p, uint32_t n) {
    if (!x || r < 0 || r >= R64_RING_COUNT || ring_writer(r) != x->role) return R64_ERR_ARGS;
    if (x->peer_dead) return R64_ERR_PEER_DEAD;
    const uint8_t *src = p;
    const uint32_t chunk = F(x)->chunk;
    while (n) {
        uint32_t space = space_of(x, r);
        if (!space || (space < n && space < chunk)) {
            /* wait for a whole chunk (or all of n) rather than trickling */
            publish(x, r);
            const uint32_t want = n < chunk ? n : chunk;
            const int rc = wait_for(x, ev_space(r), has_space, (uint32_t)r | want << 1, 0);
            if (rc) return rc;
            space = space_of(x, r);
        }
        const uint32_t at = x->ring[r].pos & x->ring[r].mask;
        uint32_t m = n < space ? n : space;
        if (m > x->ring[r].size - at) m = x->ring[r].size - at;
        const uint32_t to_chunk = chunk - (x->ring[r].pos - x->ring[r].pub);
        if (m > to_chunk) m = to_chunk;
        memcpy(x->ring[r].data + at, src, m);
        x->ring[r].pos += m, src += m, n -= m;
        if (x->ring[r].pos - x->ring[r].pub >= chunk) publish(x, r);
    }
    return R64_OK;
}

int r64_flush(R64Ipc *x, int r) {
    if (!x || r < 0 || r >= R64_RING_COUNT || ring_writer(r) != x->role) return R64_ERR_ARGS;
    publish(x, r);
    return x->peer_dead ? R64_ERR_PEER_DEAD : R64_OK;
}

uint32_t r64_writable(R64Ipc *x, int r) {
    if (!x || r < 0 || r >= R64_RING_COUNT || ring_writer(r) != x->role) return 0;
    return space_of(x, r);
}
uint32_t r64_readable(R64Ipc *x, int r) {
    if (!x || r < 0 || r >= R64_RING_COUNT || ring_writer(r) == x->role) return 0;
    return avail_of(x, r);
}

int r64_peek(R64Ipc *x, int r, const void **p, uint32_t *n) {
    if (!x || r < 0 || r >= R64_RING_COUNT || ring_writer(r) == x->role) return R64_ERR_ARGS;
    const int rc = wait_for(x, ev_data(r), has_data, (uint32_t)r | 1u << 1, 0);
    if (rc) return rc;
    const uint32_t at = x->ring[r].pos & x->ring[r].mask, avail = avail_of(x, r);
    *p = x->ring[r].data + at;
    *n = avail < x->ring[r].size - at ? avail : x->ring[r].size - at;
    return R64_OK;
}

void r64_consume(R64Ipc *x, int r, uint32_t n) {
    x->ring[r].pos += n;
    __atomic_store_n(&F(x)->ring[r].tail, x->ring[r].pos, __ATOMIC_RELEASE);
    signal_ev(x, ev_space(r));
}

int r64_read(R64Ipc *x, int r, void *dst, uint32_t n) {
    uint8_t *d = dst;
    while (n) {
        const void *p;
        uint32_t m;
        const int rc = r64_peek(x, r, &p, &m);
        if (rc) return rc;
        if (m > n) m = n;
        memcpy(d, p, m);
        r64_consume(x, r, m);
        d += m, n -= m;
    }
    return R64_OK;
}

/* ---- regions and lifecycle ---- */

void *r64_region(R64Ipc *x, int region, uint32_t *size) {
    if (!x || region < 0 || region >= R64_REG_COUNT) return NULL;
    if (size) *size = F(x)->region[region].size;
    return x->base + F(x)->region[region].offset;
}
R64Control *r64_control(R64Ipc *x) { return x ? x->ctl : NULL; }
long r64_peer_pid(R64Ipc *x) {
#ifdef _WIN32
    return x && x->peer ? (long)GetProcessId(x->peer) : 0;
#else
    return x ? x->pid : 0;
#endif
}

int r64_poll(R64Ipc *x) { return peer_gone(x) ? R64_ERR_PEER_DEAD : R64_OK; }
int r64_quit_requested(R64Ipc *x) { return F(x)->state[R64_PARENT] == R64_STATE_QUIT; }

void r64_mark_dead(R64Ipc *x) {
    __atomic_store_n(&F(x)->state[x->role], R64_STATE_DEAD, __ATOMIC_SEQ_CST);
    for (int e = 0; e < R64_EV_COUNT; e++) signal_ev(x, e);
}

const char *r64_peer_exit(R64Ipc *x, char *buf, size_t n) {
    switch (x ? x->exit_kind : 0) {
    case 1:
#ifdef _WIN32
        if ((uint32_t)x->exit_val >= 0xC0000000u) {   /* an exception code (0xC0000005 access violation, ...) */
            snprintf(buf, n, "code 0x%08x", (unsigned)x->exit_val);
            break;
        }
#endif
        snprintf(buf, n, "code %d", x->exit_val);
        break;
    case 2: snprintf(buf, n, "signal %d", x->exit_val); break;
    case 3: snprintf(buf, n, "gone"); break;
    default: snprintf(buf, n, "running"); break;
    }
    return buf;
}

static void map_rings(R64Ipc *x) {
    static const int reg[R64_RING_COUNT] = {R64_REG_CMD, R64_REG_BACK};
    for (int r = 0; r < R64_RING_COUNT; r++) {
        x->ring[r].data = x->base + F(x)->region[reg[r]].offset;
        x->ring[r].size = F(x)->region[reg[r]].size;
        x->ring[r].mask = x->ring[r].size - 1;
        /* the writer's position is the published head, the reader's the tail */
        x->ring[r].pos = ring_writer(r) == x->role ? F(x)->ring[r].head : F(x)->ring[r].tail;
        x->ring[r].pub = x->ring[r].pos;
    }
}

static void unmap(R64Ipc *x) {
#ifdef _WIN32
    if (x->base) UnmapViewOfFile(x->base);
    if (x->map) CloseHandle(x->map);
    for (int e = 0; e < R64_EV_COUNT; e++)
        if (x->ev[e]) CloseHandle(x->ev[e]);
    if (x->peer) CloseHandle(x->peer);
    if (x->job) CloseHandle(x->job);   /* kill-on-close: a helper still running goes with it */
    if (x->log_thread) {   /* the helper is gone, so its end of the pipe is closed: the reader sees the end */
        if (WaitForSingleObject(x->log_thread, 1000) != WAIT_OBJECT_0) {
            CancelSynchronousIo(x->log_thread);
            WaitForSingleObject(x->log_thread, 1000);
        }
        CloseHandle(x->log_thread);
    }
    if (x->log_rd) CloseHandle(x->log_rd);
#else
    if (x->base) munmap(x->base, x->total);
    if (x->pidfd >= 0) close(x->pidfd);
#endif
    free(x);
}

/* Parent: wait <= ms for the helper to exit (reaping it); 1 if it did. */
static int wait_exit(R64Ipc *x, uint32_t ms) {
    if (x->peer_dead) return 1;
#ifdef _WIN32
    if (WaitForSingleObject(x->peer, ms) != WAIT_OBJECT_0) return 0;
    reap(x);
    return 1;
#else
    if (x->pidfd >= 0) {
        struct pollfd p = {x->pidfd, POLLIN, 0};
        int r;
        do r = poll(&p, 1, (int)ms);
        while (r < 0 && errno == EINTR);
        if (r <= 0) return 0;
        reap(x);
        return 1;
    }
    const uint64_t end = r64_now_ns() + (uint64_t)ms * 1000000u;
    for (;;) {
        int st = 0;
        if (waitpid((pid_t)x->pid, &st, WNOHANG) == (pid_t)x->pid) {
            if (WIFSIGNALED(st)) record_status(x, 2, WTERMSIG(st));
            else record_status(x, 1, WEXITSTATUS(st));
            return 1;
        }
        if (r64_now_ns() >= end) return 0;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
#endif
}

static void kill_helper(R64Ipc *x) {
    if (x->peer_dead) return;
#ifdef _WIN32
    TerminateProcess(x->peer, 1);
    WaitForSingleObject(x->peer, 5000);
#else
    kill((pid_t)x->pid, SIGKILL);
#endif
    reap(x);
}

int r64_shutdown(R64Ipc *x, uint32_t timeout_ms, int *exit_code) {
    if (!x) return R64_ERR_ARGS;
    if (x->loopback) {   /* one mapping, two views */
        if (exit_code) *exit_code = 0;
        if (x->twin) {
            x->twin->base = NULL;   /* unmapped once, below */
#ifdef _WIN32
            x->twin->map = NULL;
            for (int e = 0; e < R64_EV_COUNT; e++) x->twin->ev[e] = NULL;
#endif
            free(x->twin);
        }
        unmap(x);
        return R64_OK;
    }
    int rc = R64_OK;
    if (x->role == R64_PARENT) {
        if (!x->peer_dead) {
            __atomic_store_n(&F(x)->state[R64_PARENT], R64_STATE_QUIT, __ATOMIC_SEQ_CST);
            for (int e = 0; e < R64_EV_COUNT; e++) signal_ev(x, e);
            if (!wait_exit(x, timeout_ms)) {
                r64_log("helper did not exit within %u ms: killed", timeout_ms);
                kill_helper(x);
                rc = R64_ERR_TIMEOUT;
            }
        }
        if (exit_code) *exit_code = x->exit_kind == 1 ? x->exit_val : x->exit_kind == 2 ? 128 + x->exit_val : -1;
    } else {
        if (F(x)->state[R64_HELPER] != R64_STATE_DEAD) __atomic_store_n(&F(x)->state[R64_HELPER], R64_STATE_QUIT, __ATOMIC_SEQ_CST);
        for (int e = 0; e < R64_EV_COUNT; e++) signal_ev(x, e);
        if (exit_code) *exit_code = 0;
    }
    unmap(x);
    return rc;
}

/* ---- the handshake ---- */

static int hello_set(R64Ipc *x, uint32_t arg) { return F(x)->hello != R64_HELLO_NONE; }
static int hello_answered(R64Ipc *x, uint32_t arg) {
    const uint32_t h = F(x)->hello;
    return h == R64_HELLO_ACCEPTED || h == R64_HELLO_REFUSED;
}

/* The first difference between the parent's header and the helper's hello, or "" if none. */
static void mismatch(const R64ControlFields *c, char *why, size_t n) {
    why[0] = 0;
    if (c->h_magic != c->magic) snprintf(why, n, "magic %08x, game %08x", c->h_magic, c->magic);
    else if (c->h_version != c->version) snprintf(why, n, "version %u, game version %u", c->h_version, c->version);
    else if (c->h_layout != c->layout || c->h_ctl_bytes != c->ctl_bytes)
        snprintf(why, n, "control layout %08x/%u, game %08x/%u", c->h_layout, c->h_ctl_bytes, c->layout, c->ctl_bytes);
    else if (c->h_schema != c->schema) snprintf(why, n, "wire schema %08x, game schema %08x", c->h_schema, c->schema);
}

static void fill_header(R64ControlFields *c, const R64SpawnOpts *o, const uint32_t *sizes, uint32_t total) {
    c->magic = R64_MAGIC;
    c->version = o->test_version ? o->test_version : R64_VERSION;
    c->layout = layout_hash();
    c->ctl_bytes = R64_CONTROL_BYTES;
    c->total = total;
    c->chunk = o->chunk_bytes ? o->chunk_bytes : R64_DEFAULT_CHUNK;
    c->spin_us = o->spin_us ? o->spin_us : 50;
    c->schema = o->schema;
    c->ptr_bits = (uint32_t)(sizeof(void *) * 8);
    uint32_t off = 0;
    for (int i = 0; i < R64_REG_COUNT; i++) {
        c->region[i].offset = off;
        c->region[i].size = sizes[i];
        off += sizes[i];
    }
    r64_in_init((uint8_t *)c + c->region[R64_REG_INPUT].offset, sizes[R64_REG_INPUT], o->input);
    c->state[R64_PARENT] = R64_STATE_ALIVE;
}

/* After the helper answered: compare, accept or refuse. */
static int finish_handshake(R64Ipc *x, const char *path, char *err, size_t errlen) {
    R64ControlFields *c = F(x);
    char why[200];
    mismatch(c, why, sizeof why);
    if (c->hello == R64_HELLO_REFUSED || why[0]) {
        if (!why[0]) snprintf(why, sizeof why, "%.*s", (int)sizeof c->refuse, c->refuse);
        if (c->hello != R64_HELLO_REFUSED) {
            snprintf(c->refuse, sizeof c->refuse, "the game refused the helper: %s", why);
            __atomic_store_n(&c->hello, R64_HELLO_REFUSED, __ATOMIC_SEQ_CST);
            signal_ev(x, R64_EV_HELPER);
        }
        set_err(err, errlen, "helper %s refused (%s)", path, why);
        r64_log("handshake refused: helper %s: %s", path, why);
        if (!wait_exit(x, 1000)) kill_helper(x);
        return R64_ERR_VERSION;
    }
    __atomic_store_n(&c->hello, R64_HELLO_ACCEPTED, __ATOMIC_SEQ_CST);
    signal_ev(x, R64_EV_HELPER);
    return R64_OK;
}

static int spawn_fail(R64Ipc *x, int rc) {
    if (x) unmap(x);
    return rc;
}

int r64_loopback(const R64SpawnOpts *o, R64Ipc **parent, R64Ipc **helper, char *err, size_t errlen) {
    *parent = *helper = NULL;
    g_prefix = "[render64] ";
    if (!o) return set_err(err, errlen, "no options"), R64_ERR_ARGS;
    const uint32_t sizes[R64_REG_COUNT] = {R64_CONTROL_BYTES,
                                           o->cmd_bytes ? o->cmd_bytes : R64_DEFAULT_CMD_BYTES,
                                           o->content_bytes ? o->content_bytes : R64_DEFAULT_CONTENT_BYTES,
                                           o->back_bytes ? o->back_bytes : R64_DEFAULT_BACK_BYTES,
                                           o->stats_bytes ? o->stats_bytes : R64_DEFAULT_STATS_BYTES,
                                           o->input_bytes ? o->input_bytes : R64_DEFAULT_INPUT_BYTES};
    uint64_t total64 = 0;
    for (int i = 0; i < R64_REG_COUNT; i++) {
        if (sizes[i] & 4095) return set_err(err, errlen, "region %d size %u is not page-aligned", i, sizes[i]), R64_ERR_ARGS;
        total64 += sizes[i];
    }
    const uint32_t chunk = o->chunk_bytes ? o->chunk_bytes : R64_DEFAULT_CHUNK;
    if (!is_pow2(sizes[R64_REG_CMD]) || !is_pow2(sizes[R64_REG_BACK]) || chunk & 3 || chunk > sizes[R64_REG_BACK] ||
        total64 > 0x7fffffffu)
        return set_err(err, errlen, "bad ring sizes (powers of two, chunk <= back ring)"), R64_ERR_ARGS;
    const uint32_t total = (uint32_t)total64;
    R64Ipc *x = calloc(1, sizeof *x), *y = calloc(1, sizeof *y);
    if (!x || !y) {
        free(x);
        free(y);
        return set_err(err, errlen, "out of memory"), R64_ERR_SYS;
    }
    x->role = R64_PARENT;
    x->total = total;
    x->loopback = 1;
#ifdef _WIN32
    x->map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, total, NULL);
    x->base = x->map ? MapViewOfFile(x->map, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    if (!x->base) {
        free(y);
        return set_err(err, errlen, "no shared section (%lu)", GetLastError()), spawn_fail(x, R64_ERR_SYS);
    }
    for (int e = 0; e < R64_EV_COUNT; e++)
        if (!(x->ev[e] = CreateEventW(NULL, FALSE, FALSE, NULL))) {
            free(y);
            return set_err(err, errlen, "no event (%lu)", GetLastError()), spawn_fail(x, R64_ERR_SYS);
        }
#else
    x->pidfd = -1;
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        free(y);
        return set_err(err, errlen, "mmap %u bytes: %s", total, strerror(errno)), spawn_fail(x, R64_ERR_SYS);
    }
    x->base = base;
#endif
    x->ctl = (R64Control *)x->base;
    fill_header(F(x), o, sizes, total);
    x->spin_us = F(x)->spin_us;
    F(x)->state[R64_HELPER] = R64_STATE_ALIVE;
    F(x)->hello = R64_HELLO_ACCEPTED;
    *y = *x;
    y->role = R64_HELPER;
    map_rings(x);
    map_rings(y);
    x->twin = y;
    y->twin = x;
    *parent = x;
    *helper = y;
    return R64_OK;
}

#ifdef _WIN32
/* The helper's stdout and stderr, printed by this process (the game's printf is the outlog: console and log file). */
static void log_line(void *ctx, const char *line) {
    (void)ctx;
    printf("%s\n", line);
}
static DWORD WINAPI log_reader(void *arg) {
    R64Ipc *x = arg;
    R64Lines lines;
    lines.n = 0;
    char buf[4096];
    DWORD got;
    while (ReadFile(x->log_rd, buf, sizeof buf, &got, NULL) && got) r64_lines_feed(&lines, buf, got, log_line, NULL);
    r64_lines_flush(&lines, log_line, NULL);
    fflush(stdout);
    return 0;
}
#endif

int r64_spawn(const R64SpawnOpts *o, R64Ipc **out, char *err, size_t errlen) {
    *out = NULL;
    g_prefix = "[render64] ";
    if (!o || !o->helper_path) return set_err(err, errlen, "no helper path"), R64_ERR_ARGS;
    const uint32_t sizes[R64_REG_COUNT] = {R64_CONTROL_BYTES,
                                           o->cmd_bytes ? o->cmd_bytes : R64_DEFAULT_CMD_BYTES,
                                           o->content_bytes ? o->content_bytes : R64_DEFAULT_CONTENT_BYTES,
                                           o->back_bytes ? o->back_bytes : R64_DEFAULT_BACK_BYTES,
                                           o->stats_bytes ? o->stats_bytes : R64_DEFAULT_STATS_BYTES,
                                           o->input_bytes ? o->input_bytes : R64_DEFAULT_INPUT_BYTES};
    uint64_t total64 = 0;
    for (int i = 0; i < R64_REG_COUNT; i++) {
        if (sizes[i] & 4095) return set_err(err, errlen, "region %d size %u is not page-aligned", i, sizes[i]), R64_ERR_ARGS;
        total64 += sizes[i];
    }
    const uint32_t chunk = o->chunk_bytes ? o->chunk_bytes : R64_DEFAULT_CHUNK;
    if (!is_pow2(sizes[R64_REG_CMD]) || !is_pow2(sizes[R64_REG_BACK]) || chunk & 3 || chunk > sizes[R64_REG_BACK] ||
        total64 > 0x7fffffffu)
        return set_err(err, errlen, "bad ring sizes (powers of two, chunk <= back ring)"), R64_ERR_ARGS;
    const uint32_t total = (uint32_t)total64;
    R64Ipc *x = calloc(1, sizeof *x);
    if (!x) return set_err(err, errlen, "out of memory"), R64_ERR_SYS;
    x->role = R64_PARENT;
    x->total = total;
    char cmd_args[1200];
    size_t ca = 0;
    cmd_args[0] = 0;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    x->map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, total, NULL);
    x->base = x->map ? MapViewOfFile(x->map, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    if (!x->base) return set_err(err, errlen, "no shared section (%lu)", GetLastError()), spawn_fail(x, R64_ERR_SYS);
    for (int e = 0; e < R64_EV_COUNT; e++)
        if (!(x->ev[e] = CreateEventW(&sa, FALSE, FALSE, NULL)))
            return set_err(err, errlen, "no event (%lu)", GetLastError()), spawn_fail(x, R64_ERR_SYS);
#else
    x->pidfd = -1;
    /* not close-on-exec until the helper started: it inherits the descriptor */
    const int fd = (int)syscall(SYS_memfd_create, "render64", 0);
    if (fd < 0 || ftruncate(fd, (off_t)total)) {
        set_err(err, errlen, "memfd: %s", strerror(errno));
        if (fd >= 0) close(fd);
        return spawn_fail(x, R64_ERR_SYS);
    }
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        set_err(err, errlen, "mmap %u bytes: %s", total, strerror(errno));
        close(fd);
        return spawn_fail(x, R64_ERR_SYS);
    }
    x->base = base;
#endif
    x->ctl = (R64Control *)x->base;
    fill_header(F(x), o, sizes, total);
    x->spin_us = F(x)->spin_us;
    map_rings(x);
#ifdef _WIN32
    F(x)->parent_pid = (uint32_t)GetCurrentProcessId();
    /* Inherited by the helper, and nothing else (PROC_THREAD_ATTRIBUTE_HANDLE_LIST: every entry inheritable, no
     * pseudo handle, no duplicate): the section, the events, a waitable handle of this process (the helper's death
     * watch: SYNCHRONIZE, and QUERY_LIMITED for its pid) and the write end of the log pipe as its stdout and stderr
     * (the game's console handles are not passed: the outlog's log file only sees what this process prints). */
    HANDLE self = NULL, log_wr = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                    TRUE, 0);
    if (CreatePipe(&x->log_rd, &log_wr, &sa, 0)) SetHandleInformation(x->log_rd, HANDLE_FLAG_INHERIT, 0);
    else x->log_rd = log_wr = NULL;
    HANDLE list[R64_EV_COUNT + 3];
    int nl = 0;
    list[nl++] = x->map;
    for (int e = 0; e < R64_EV_COUNT; e++) list[nl++] = x->ev[e];
    if (self) list[nl++] = self;
    if (log_wr) list[nl++] = log_wr;
    ca += (size_t)snprintf(cmd_args + ca, sizeof cmd_args - ca, "\"%s\" --ipc-handles %llx", o->helper_path,
                           (unsigned long long)(uintptr_t)x->map);
    for (int e = 0; e < R64_EV_COUNT; e++)
        ca += (size_t)snprintf(cmd_args + ca, sizeof cmd_args - ca, ":%llx", (unsigned long long)(uintptr_t)x->ev[e]);
    ca += (size_t)snprintf(cmd_args + ca, sizeof cmd_args - ca, ":%llx", (unsigned long long)(uintptr_t)self);
    for (const char *const *a = o->extra_args; a && *a && ca < sizeof cmd_args; a++)
        ca += (size_t)snprintf(cmd_args + ca, sizeof cmd_args - ca, strchr(*a, ' ') ? " \"%s\"" : " %s", *a);
    x->job = CreateJobObjectW(NULL, NULL);
    if (x->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(x->job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = HeapAlloc(GetProcessHeap(), 0, asz);
    BOOL ok = al && InitializeProcThreadAttributeList(al, 1, 0, &asz) &&
              UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, (SIZE_T)nl * sizeof(HANDLE), NULL, NULL);
    STARTUPINFOEXA si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = NULL;
    si.StartupInfo.hStdOutput = log_wr;
    si.StartupInfo.hStdError = log_wr;
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (ok)
        ok = CreateProcessA(NULL, cmd_args, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL,
                            NULL, &si.StartupInfo, &pi);   /* CREATE_NO_WINDOW: a console program, no console of its own */
    const DWORD werr = GetLastError();
    if (al) {
        DeleteProcThreadAttributeList(al);
        HeapFree(GetProcessHeap(), 0, al);
    }
    if (self) CloseHandle(self);
    if (log_wr) CloseHandle(log_wr);   /* the helper's copy is the only writer left: its exit ends the reader */
    /* inheritance was taken at CreateProcess: another program this process starts later must not get the channel */
    SetHandleInformation(x->map, HANDLE_FLAG_INHERIT, 0);
    for (int e = 0; e < R64_EV_COUNT; e++) SetHandleInformation(x->ev[e], HANDLE_FLAG_INHERIT, 0);
    if (!ok) return set_err(err, errlen, "cannot start %s (%lu)", o->helper_path, werr), spawn_fail(x, R64_ERR_SYS);
    /* in the job before it runs a single instruction: the game's exit kills it */
    if (x->job && !AssignProcessToJobObject(x->job, pi.hProcess)) r64_log("helper not in a job object (%lu)", GetLastError());
    x->peer = pi.hProcess;
    if (x->log_rd && !(x->log_thread = CreateThread(NULL, 64 << 10, log_reader, x, 0, NULL)))
        r64_log("no thread for the helper's output (%lu): it is lost", GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
#else
    F(x)->parent_pid = (uint32_t)getpid();
    char fdstr[16];
    snprintf(fdstr, sizeof fdstr, "%d", fd);
    const char *argv[40];
    int na = 0;
    argv[na++] = o->helper_path;
    argv[na++] = "--ipc-fd";
    argv[na++] = fdstr;
    for (const char *const *a = o->extra_args; a && *a && na < 38; a++) argv[na++] = *a;
    argv[na] = NULL;
    (void)cmd_args, (void)ca;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (o->log_fd >= 0 && o->log_fd != 1) posix_spawn_file_actions_adddup2(&fa, o->log_fd, 1);
    if (o->log_fd >= 0 && o->log_fd != 2) posix_spawn_file_actions_adddup2(&fa, o->log_fd, 2);
    pid_t pid = 0;
    const int se = posix_spawn(&pid, o->helper_path, &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd);   /* the mapping stays; the helper has its own descriptor */
    if (se) return set_err(err, errlen, "cannot start %s: %s", o->helper_path, strerror(se)), spawn_fail(x, R64_ERR_SYS);
    x->pid = pid;
    x->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);   /* -1 on kernels < 5.3: waitpid(WNOHANG) instead */
#endif
    const int rc = wait_for(x, R64_EV_PARENT, hello_set, 0, o->handshake_ms ? o->handshake_ms : 2000);
    if (rc) {
        char st[32];
        if (rc == R64_ERR_TIMEOUT) {
            kill_helper(x);
            set_err(err, errlen, "helper %s: no handshake within %u ms", o->helper_path, o->handshake_ms ? o->handshake_ms : 2000);
        } else
            set_err(err, errlen, "helper %s exited before the handshake (%s)", o->helper_path, r64_peer_exit(x, st, sizeof st));
        r64_log("%s", err ? err : "handshake failed");
        return spawn_fail(x, rc);
    }
    const int hr = finish_handshake(x, o->helper_path, err, errlen);
    if (hr) return spawn_fail(x, hr);
    *out = x;
    return R64_OK;
}

static int parse_handles(int argc, char **argv, R64Ipc *x) {
    for (int i = 1; i + 1 < argc; i++) {
#ifdef _WIN32
        if (strcmp(argv[i], "--ipc-handles")) continue;
        unsigned long long v[R64_EV_COUNT + 2];
        const char *p = argv[i + 1];
        int k = 0;
        for (; k < R64_EV_COUNT + 2 && *p; k++) {
            char *end;
            v[k] = strtoull(p, &end, 16);
            if (end == p) break;
            p = *end == ':' ? end + 1 : end;
        }
        if (k != R64_EV_COUNT + 2) return -1;
        x->map = (HANDLE)(uintptr_t)v[0];
        for (int e = 0; e < R64_EV_COUNT; e++) x->ev[e] = (HANDLE)(uintptr_t)v[1 + e];
        x->peer = (HANDLE)(uintptr_t)v[R64_EV_COUNT + 1];
        x->base = MapViewOfFile(x->map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!x->base) return -1;
        MEMORY_BASIC_INFORMATION mi;
        x->total = VirtualQuery(x->base, &mi, sizeof mi) ? mi.RegionSize : 0;
        return 0;
#else
        if (strcmp(argv[i], "--ipc-fd")) continue;
        const int fd = atoi(argv[i + 1]);
        struct stat st;
        if (fstat(fd, &st) || st.st_size < (off_t)R64_CONTROL_BYTES) return -1;
        void *b = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (b == MAP_FAILED) return -1;
        x->base = b;
        x->total = (size_t)st.st_size;
        return 0;
#endif
    }
    return -1;
}

int r64_attach(int argc, char **argv, const R64AttachOpts *o, R64Ipc **out) {
    *out = NULL;
    g_prefix = "[r64] ";
    R64Ipc *x = calloc(1, sizeof *x);
    if (!x) return R64_ERR_SYS;
    x->role = R64_HELPER;
#ifndef _WIN32
    x->pidfd = -1;
    /* die with the game; getppid() closes the race of a parent that died before the prctl */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    if (parse_handles(argc, argv, x)) {
        r64_log("no shared channel on the command line (--ipc-fd / --ipc-handles): this program is started by the game");
        free(x);
        return R64_ERR_ARGS;
    }
    x->ctl = (R64Control *)x->base;
    R64ControlFields *c = F(x);
#ifndef _WIN32
    x->pid = (long)c->parent_pid;
    if ((long)getppid() != x->pid) {
        r64_log("the game (pid %u) is already gone", c->parent_pid);
        x->ctl = NULL;
        unmap(x);
        return R64_ERR_PEER_DEAD;
    }
#endif
    if (c->magic != R64_MAGIC || c->total > x->total) {
        r64_log("handshake refused: not a render64 channel (magic %08x, %u of %u bytes)", c->magic, c->total, (unsigned)x->total);
        unmap(x);
        return R64_ERR_VERSION;
    }
    x->total = c->total;
    /* the hello, then this side's own check of the game's header */
    c->h_magic = R64_MAGIC;
    c->h_version = o && o->test_version ? o->test_version : R64_VERSION;
    c->h_layout = layout_hash();
    c->h_ctl_bytes = R64_CONTROL_BYTES;
    c->h_schema = o ? o->schema : 0;
    c->h_ptr_bits = (uint32_t)(sizeof(void *) * 8);
#ifdef _WIN32
    c->h_pid = (uint32_t)GetCurrentProcessId();
#else
    c->h_pid = (uint32_t)getpid();
#endif
    c->h_caps = o ? o->caps : 0;
    snprintf(c->h_device, sizeof c->h_device, "%s", o && o->device ? o->device : "");
    c->state[R64_HELPER] = R64_STATE_ALIVE;
    char why[200];
    mismatch(c, why, sizeof why);
    if (why[0]) {
        /* the helper's terms first: "helper version 7, game version 1" */
        snprintf(c->refuse, sizeof c->refuse, "the helper refused the game: helper %s", why);
        __atomic_store_n(&c->hello, R64_HELLO_REFUSED, __ATOMIC_SEQ_CST);
        r64_log("handshake refused: helper %s", why);
        c->state[R64_HELPER] = R64_STATE_DEAD;
        signal_ev(x, R64_EV_PARENT);
        unmap(x);
        return R64_ERR_VERSION;
    }
    x->spin_us = c->spin_us;
    map_rings(x);
    __atomic_store_n(&c->hello, R64_HELLO_SENT, __ATOMIC_SEQ_CST);
    signal_ev(x, R64_EV_PARENT);
    const int rc = wait_for(x, R64_EV_HELPER, hello_answered, 0, 5000);
    if (rc || c->hello != R64_HELLO_ACCEPTED) {
        if (rc == R64_ERR_PEER_DEAD) r64_log("the game exited during the handshake");
        else if (rc) r64_log("no answer to the handshake (%d)", rc);
        else r64_log("handshake refused: %.*s", (int)sizeof c->refuse, c->refuse);
        unmap(x);
        return rc ? rc : R64_ERR_VERSION;
    }
    *out = x;
    return R64_OK;
}

/* ---- the input region (render_ipc.h) ---- */
_Static_assert(sizeof(R64InEvent) == R64_IN_RECORD_BYTES, "input records are 64 bytes");
_Static_assert(sizeof(R64InHeader) <= R64_IN_HEADER_BYTES, "input header fits its kilobyte");
_Static_assert(sizeof(R64InConfig) == 80, "the window config has the same layout in both ABIs");
_Static_assert(offsetof(R64InHeader, ring[0].head) == 128 && offsetof(R64InHeader, ring[1].head) == 320 &&
               offsetof(R64InHeader, quit) == 512 && offsetof(R64InEvent, v) == 24, "input layout: the same bytes in i386 and x86_64");

static uint32_t floor_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p <= v / 2) p *= 2;
    return v ? p : 0;
}
int r64_in_init(void *region, uint32_t size, const R64InConfig *cfg) {
    if (!region || size < 4096) return R64_ERR_ARGS;
    memset(region, 0, R64_IN_HEADER_BYTES);
    R64InHeader *h = region;
    const uint32_t avail = (size - R64_IN_HEADER_BYTES) / R64_IN_RECORD_BYTES;
    h->slots[R64_IN_EVENTS] = floor_pow2(avail);                          /* 64 KB: 512 events */
    if (h->slots[R64_IN_EVENTS] == avail) h->slots[R64_IN_EVENTS] /= 2;
    h->slots[R64_IN_CMDS] = floor_pow2(avail - h->slots[R64_IN_EVENTS]);  /* 64 KB: 256 commands */
    h->offset[R64_IN_EVENTS] = R64_IN_HEADER_BYTES;
    h->offset[R64_IN_CMDS] = R64_IN_HEADER_BYTES + h->slots[R64_IN_EVENTS] * R64_IN_RECORD_BYTES;
    if (cfg) h->cfg = *cfg;
    h->cfg.title[sizeof h->cfg.title - 1] = 0;
    h->bytes = size;
    h->record_bytes = R64_IN_RECORD_BYTES;
    h->version = R64_IN_VERSION;
    __atomic_store_n(&h->magic, R64_IN_MAGIC, __ATOMIC_RELEASE);
    return R64_OK;
}
int r64_in_check(const void *region, uint32_t size, char *why, size_t n) {
    const R64InHeader *h = region;
    if (!h || size < 4096) return set_err(why, n, "no input region"), R64_ERR_VERSION;
    if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) != R64_IN_MAGIC)
        return set_err(why, n, "input region magic %08x, expected %08x", h->magic, R64_IN_MAGIC), R64_ERR_VERSION;
    if (h->version != R64_IN_VERSION) return set_err(why, n, "input protocol version %u, this side %u", h->version, R64_IN_VERSION), R64_ERR_VERSION;
    if (h->record_bytes != R64_IN_RECORD_BYTES) return set_err(why, n, "input records of %u bytes, this side %u", h->record_bytes, R64_IN_RECORD_BYTES), R64_ERR_VERSION;
    for (int r = 0; r < R64_IN_RING_COUNT; r++)
        if (!is_pow2(h->slots[r]) || h->offset[r] < R64_IN_HEADER_BYTES ||
            (uint64_t)h->offset[r] + (uint64_t)h->slots[r] * R64_IN_RECORD_BYTES > size || h->bytes != size)
            return set_err(why, n, "input ring %d: %u slots at %u in %u bytes", r, h->slots[r], h->offset[r], size), R64_ERR_VERSION;
    return R64_OK;
}
static R64InEvent *in_slot(R64InHeader *h, int ring, uint32_t pos) {
    return (R64InEvent *)((uint8_t *)h + h->offset[ring]) + (pos & (h->slots[ring] - 1));
}
static void in_write(R64InHeader *h, int ring, uint32_t head, uint16_t type, uint16_t arg, const void *p, uint32_t n, uint64_t origin) {
    R64InEvent *e = in_slot(h, ring, head);
    memset(e, 0, sizeof *e);
    e->seq = h->ring[ring].seq++;
    e->type = type;
    e->arg = arg;
    const uint64_t t = r64_now_ns();
    e->t_lo = (uint32_t)t;
    e->t_hi = (uint32_t)(t >> 32);
    e->o_lo = (uint32_t)origin;
    e->o_hi = (uint32_t)(origin >> 32);
    if (p) memcpy(e->v, p, n < sizeof e->v ? n : sizeof e->v);
}
int r64_in_put(void *region, int ring, uint16_t type, uint16_t arg, const void *payload, uint32_t n, uint64_t origin_ns) {
    R64InHeader *h = region;
    if (!h || ring < 0 || ring >= R64_IN_RING_COUNT) return 0;
    uint32_t head = h->ring[ring].head;   /* only this side writes it */
    const uint32_t used = head - __atomic_load_n(&h->ring[ring].tail, __ATOMIC_ACQUIRE);
    const uint32_t need = h->ring[ring].resync ? 2u : 1u;
    if (used + need > h->slots[ring]) {
        __atomic_store_n(&h->ring[ring].dropped, h->ring[ring].dropped + 1, __ATOMIC_RELAXED);
        h->ring[ring].resync = 1;
        return 0;
    }
    if (h->ring[ring].resync) {
        const int32_t d = (int32_t)h->ring[ring].dropped;
        in_write(h, ring, head++, R64_IN_RESYNC, 0, &d, sizeof d, 0);
        h->ring[ring].resync = 0;
    }
    in_write(h, ring, head++, type, arg, payload, n, origin_ns);
    __atomic_store_n(&h->ring[ring].head, head, __ATOMIC_RELEASE);
    return 1;
}
int r64_in_get(void *region, int ring, R64InEvent *out) {
    R64InHeader *h = region;
    if (!h || ring < 0 || ring >= R64_IN_RING_COUNT) return 0;
    const uint32_t tail = h->ring[ring].tail;   /* only this side writes it */
    if (__atomic_load_n(&h->ring[ring].head, __ATOMIC_ACQUIRE) == tail) return 0;
    *out = *in_slot(h, ring, tail);
    __atomic_store_n(&h->ring[ring].tail, tail + 1, __ATOMIC_RELEASE);
    return 1;
}
uint32_t r64_in_dropped(const void *region, int ring) {
    const R64InHeader *h = region;
    return h && ring >= 0 && ring < R64_IN_RING_COUNT ? __atomic_load_n(&h->ring[ring].dropped, __ATOMIC_RELAXED) : 0;
}
