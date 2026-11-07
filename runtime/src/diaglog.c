/* Robustness diagnostics for the log: crash breadcrumbs, crash context, missing HLE functions,
 * rate-limited Vulkan and audio errors (see diaglog.h). */
#define _GNU_SOURCE
#include "diaglog.h"
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

uint64_t dlog_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t start_ns;
static DlogSources src;
static char level_name[24];
static int level_mission = -1;
uint32_t dlog_debug_crash_frame;
static int debug_crash_kind;   /* 0 segv, 1 abort */

void dlog_init(void) {
    if (!start_ns) start_ns = dlog_now_ns();
    const char *e = getenv("XBRUN_DEBUG_CRASH");
    if (e && *e) {
        dlog_debug_crash_frame = (uint32_t)strtoul(e, NULL, 10);
        debug_crash_kind = strstr(e, ":abort") != NULL;
        if (dlog_debug_crash_frame)
            printf("[diag] XBRUN_DEBUG_CRASH: a deliberate %s at frame %u\n", debug_crash_kind ? "abort()" : "segfault",
                   dlog_debug_crash_frame);
    }
}

void dlog_set_sources(const DlogSources *s) { src = *s; }
uint32_t dlog_frame(void) { return src.frame ? src.frame() : 0; }

void dlog_set_level(const char *level, int mission) {
    /* Racy by design (a crash handler may read a half-written name): the buffer always holds a
     * NUL within its size, so a torn read only shows a mixed name. */
    size_t n = level ? strlen(level) : 0;
    if (n > sizeof level_name - 1) n = sizeof level_name - 1;
    memcpy(level_name, level ? level : "", n);
    level_name[n] = 0;
    __atomic_store_n(&level_mission, mission, __ATOMIC_RELEASE);
}

const char *dlog_movie_name(const char *path) {
    size_t n = path ? strlen(path) : 0;
    if (n < 4 || strcasecmp(path + n - 4, ".bik")) return NULL;
    const char *b = path + n;
    while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
    return b;
}

/* ---- allocation-free formatting (crash handlers) ---- */
typedef struct { char *p; size_t n, cap; } Buf;
static void put_str(Buf *b, const char *s) {
    while (s && *s && b->n + 1 < b->cap) b->p[b->n++] = *s++;
    if (b->cap) b->p[b->n] = 0;
}
static void put_u64(Buf *b, uint64_t v) {
    char t[24];
    int i = (int)sizeof t - 1;
    t[i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
    put_str(b, t + i);
}
/* ns as seconds with `decimals` decimals (truncated). */
static void put_secs(Buf *b, uint64_t ns, int decimals) {
    put_u64(b, ns / 1000000000ull);
    if (!decimals) return;
    uint64_t frac = ns % 1000000000ull, div = 1000000000ull;
    put_str(b, ".");
    for (int i = 0; i < decimals; i++) {
        div /= 10;
        char d[2] = {(char)('0' + (frac / div) % 10), 0};
        put_str(b, d);
    }
}

/* ---- breadcrumbs ---- */
typedef struct {
    uint32_t seq;     /* index + 1 once written; 0 while a writer fills it */
    uint32_t frame;
    uint64_t t;
    char text[DLOG_CRUMB_TEXT];
} Crumb;
static Crumb ring[DLOG_CRUMBS];
static uint32_t ring_head;   /* breadcrumbs added so far */

void dlog_crumb(const char *fmt, ...) {
    uint32_t n = __atomic_fetch_add(&ring_head, 1, __ATOMIC_ACQ_REL);
    Crumb *c = &ring[n % DLOG_CRUMBS];
    __atomic_store_n(&c->seq, 0, __ATOMIC_RELEASE);
    c->t = dlog_now_ns();
    c->frame = dlog_frame();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->text, sizeof c->text, fmt, ap);
    va_end(ap);
    __atomic_store_n(&c->seq, n + 1, __ATOMIC_RELEASE);
}

size_t dlog_crumbs_format(char *buf, size_t cap, uint64_t now_ns) {
    Buf b = {buf, 0, cap};
    if (cap) buf[0] = 0;
    uint32_t end = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
    uint32_t first = end > DLOG_CRUMBS ? end - DLOG_CRUMBS : 0;
    for (uint32_t i = first; i != end; i++) {
        const Crumb *c = &ring[i % DLOG_CRUMBS];
        if (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) != i + 1) continue;   /* being written or reused */
        char text[DLOG_CRUMB_TEXT];
        memcpy(text, c->text, sizeof text);
        text[sizeof text - 1] = 0;
        uint64_t t = c->t;
        uint32_t frame = c->frame;
        if (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) != i + 1) continue;   /* overwritten meanwhile */
        put_str(&b, "[breadcrumb] -");
        put_secs(&b, now_ns > t ? now_ns - t : 0, 3);
        put_str(&b, "s frame ");
        put_u64(&b, frame);
        put_str(&b, ": ");
        put_str(&b, text);
        put_str(&b, "\n");
    }
    return b.n;
}

size_t dlog_context_format(char *buf, size_t cap, uint64_t now_ns) {
    Buf b = {buf, 0, cap};
    if (cap) buf[0] = 0;
    char lv[sizeof level_name];
    memcpy(lv, level_name, sizeof lv);
    lv[sizeof lv - 1] = 0;
    int mission = __atomic_load_n(&level_mission, __ATOMIC_ACQUIRE);
    put_str(&b, "[crash-context] level ");
    put_str(&b, lv[0] ? lv : "-");
    put_str(&b, ", mission ");
    if (mission >= 0) put_u64(&b, (uint64_t)mission);
    else put_str(&b, "-");
    put_str(&b, ", frame ");
    put_u64(&b, dlog_frame());
    put_str(&b, ", uptime ");
    put_secs(&b, start_ns && now_ns > start_ns ? now_ns - start_ns : 0, 1);
    put_str(&b, " s, guest arena used ");
    put_u64(&b, (src.arena_used ? src.arena_used() : 0) >> 20);
    put_str(&b, " MiB of ");
    put_u64(&b, (src.arena_total ? src.arena_total() : 0) >> 20);
    put_str(&b, " MiB, threads ");
    put_u64(&b, src.threads ? src.threads() : 0);
    put_str(&b, "\n");
    return b.n;
}

#ifdef _WIN32
#include "platform/outlog.h"
static void out_write(const char *p, size_t n) { outlog_write(0, p, n); }
#else
static void out_write(const char *p, size_t n) {
    while (n) {
        ssize_t w = write(1, p, n);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}
#endif

/* One static buffer: a second crash on another thread at the same time takes the spin flag's
 * loser path and writes only its header. */
static char dump_buf[DLOG_CRUMBS * (DLOG_CRUMB_TEXT + 48) + 512];
static int dump_busy;

void dlog_crash_dump(const char *why) {
    uint64_t now = dlog_now_ns();
    char head[160];
    Buf h = {head, 0, sizeof head};
    put_str(&h, "[crash-context] reason: ");
    put_str(&h, why ? why : "?");
    put_str(&h, " (last ");
    uint32_t end = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
    put_u64(&h, end < DLOG_CRUMBS ? end : DLOG_CRUMBS);
    put_str(&h, " breadcrumbs, oldest first)\n");
    out_write(head, h.n);
    if (__atomic_exchange_n(&dump_busy, 1, __ATOMIC_ACQ_REL)) return;
    size_t n = dlog_crumbs_format(dump_buf, sizeof dump_buf, now);
    n += dlog_context_format(dump_buf + n, sizeof dump_buf - n, now);
    out_write(dump_buf, n);
    __atomic_store_n(&dump_busy, 0, __ATOMIC_RELEASE);
}

/* ---- missing HLE functionality ---- */
typedef struct {
    char name[64];
    uint32_t count;
    const uint32_t *ext;
} Missing;
static Missing missing[DLOG_MISSING_MAX];
static int nmissing;
static pthread_mutex_t missing_lock = PTHREAD_MUTEX_INITIALIZER;

int dlog_missing_first(const char *name, uint32_t caller, const uint32_t *ext_count) {
    if (!name) name = "?";
    pthread_mutex_lock(&missing_lock);
    for (int i = 0; i < nmissing; i++)
        if (!strncmp(missing[i].name, name, sizeof missing[i].name - 1)) {
            pthread_mutex_unlock(&missing_lock);
            return i;
        }
    if (nmissing == DLOG_MISSING_MAX) {
        pthread_mutex_unlock(&missing_lock);
        return -1;
    }
    int i = nmissing;
    snprintf(missing[i].name, sizeof missing[i].name, "%s", name);
    missing[i].count = 0;
    missing[i].ext = ext_count;
    __atomic_store_n(&nmissing, i + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&missing_lock);
    printf("[hle-missing] %s (first call from 0x%08x, frame %u)\n", name, caller, dlog_frame());
    fflush(stdout);
    return i;
}

void dlog_missing_hit(int slot) {
    if (slot >= 0 && slot < DLOG_MISSING_MAX) __atomic_add_fetch(&missing[slot].count, 1, __ATOMIC_RELAXED);
}

static uint32_t missing_count(int i) {
    return missing[i].ext ? __atomic_load_n(missing[i].ext, __ATOMIC_RELAXED) : __atomic_load_n(&missing[i].count, __ATOMIC_RELAXED);
}

size_t dlog_missing_summary_format(char *buf, size_t cap) {
    Buf b = {buf, 0, cap};
    if (cap) buf[0] = 0;
    int n = __atomic_load_n(&nmissing, __ATOMIC_ACQUIRE), order[DLOG_MISSING_MAX];
    uint32_t cnt[DLOG_MISSING_MAX];
    for (int i = 0; i < n; i++) {   /* insertion sort: most calls first, then first seen */
        uint32_t c = missing_count(i);
        int j = i;
        while (j > 0 && cnt[j - 1] < c) { order[j] = order[j - 1]; cnt[j] = cnt[j - 1]; j--; }
        order[j] = i;
        cnt[j] = c;
    }
    put_str(&b, "[hle-missing] summary: ");
    if (!n) put_str(&b, "none");
    for (int k = 0; k < n; k++) {
        if (k) put_str(&b, ", ");
        put_str(&b, missing[order[k]].name);
        put_str(&b, "x");
        put_u64(&b, cnt[k]);
    }
    put_str(&b, "\n");
    return b.n;
}

/* ---- rate limiting ---- */
int dlog_rate_allow(DlogRate *r, uint64_t now_ns, uint32_t burst, uint64_t window_ns, uint32_t *suppressed) {
    if (!r->started || now_ns - r->window_start >= window_ns) {
        r->started = 1;
        r->window_start = now_ns;
        r->in_window = 0;
    }
    if (r->in_window < burst) {
        r->in_window++;
        if (suppressed) *suppressed = r->suppressed;
        r->suppressed = 0;
        return 1;
    }
    r->suppressed++;
    return 0;
}

/* ---- Vulkan errors ---- */
#define VK_KINDS 64
#define VK_BURST 5
#define VK_WINDOW_NS 10000000000ull   /* 5 lines per call and result every 10 s */
typedef struct {
    const char *call, *name;
    int result, swapchain;
    uint32_t count;
    DlogRate rate;
} VkKind;
static VkKind vk_kinds[VK_KINDS];
static int nvk;
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static int device_lost_dumped;

void dlog_vk_error(const char *call, int result, const char *result_name, int swapchain, int device_lost) {
    uint64_t now = dlog_now_ns();
    pthread_mutex_lock(&report_lock);
    VkKind *k = NULL;
    for (int i = 0; i < nvk; i++)
        if (vk_kinds[i].result == result && !strcmp(vk_kinds[i].call, call)) { k = &vk_kinds[i]; break; }
    if (!k && nvk < VK_KINDS) {
        k = &vk_kinds[nvk++];
        *k = (VkKind){call, result_name, result, swapchain, 0, {0, 0, 0, 0}};
    }
    uint32_t dropped = 0;
    int show = 0;
    if (k) {
        k->count++;
        show = swapchain ? k->count == 1 : dlog_rate_allow(&k->rate, now, VK_BURST, VK_WINDOW_NS, &dropped);
    }
    pthread_mutex_unlock(&report_lock);
    if (show) {
        if (dropped) printf("[vk-error] %s: %s (frame %u) (%u more not logged)\n", call, result_name, dlog_frame(), dropped);
        else printf("[vk-error] %s: %s (frame %u)%s\n", call, result_name, dlog_frame(),
                    swapchain ? " (further ones only counted)" : "");
        fflush(stdout);
    }
    if (device_lost && !__atomic_exchange_n(&device_lost_dumped, 1, __ATOMIC_ACQ_REL)) {
        dlog_crumb("Vulkan device lost in %s", call);
        fflush(stdout);
        dlog_crash_dump("Vulkan device lost");
    }
}

/* ---- audio underruns ---- */
static uint32_t underruns;
static DlogRate underrun_rate;
#define UNDERRUN_BURST 3
#define UNDERRUN_WINDOW_NS 10000000000ull

void dlog_audio_underrun(void) {
    uint32_t n = __atomic_add_fetch(&underruns, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&report_lock);
    int show = dlog_rate_allow(&underrun_rate, dlog_now_ns(), UNDERRUN_BURST, UNDERRUN_WINDOW_NS, NULL);
    pthread_mutex_unlock(&report_lock);
    if (show) {
        printf("[audio] underrun %u (frame %u)\n", n, dlog_frame());
        fflush(stdout);
    }
}
uint32_t dlog_audio_underruns(void) { return __atomic_load_n(&underruns, __ATOMIC_RELAXED); }

/* ---- exit ---- */
void dlog_exit_summary(void) {
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    static char line[8192];
    dlog_missing_summary_format(line, sizeof line);
    printf("%s", line);
    printf("[audio] underruns: %u total\n", dlog_audio_underruns());
    pthread_mutex_lock(&report_lock);
    Buf b = {line, 0, sizeof line};
    put_str(&b, "[vk-error] summary: ");
    for (int i = 0; i < nvk; i++) {
        if (i) put_str(&b, ", ");
        put_str(&b, vk_kinds[i].call);
        put_str(&b, " ");
        put_str(&b, vk_kinds[i].name);
        put_str(&b, " x");
        put_u64(&b, vk_kinds[i].count);
    }
    int any = nvk;
    pthread_mutex_unlock(&report_lock);
    if (any) printf("%s\n", line);
    fflush(stdout);
}

/* ---- XBRUN_DEBUG_CRASH ---- */
void dlog_debug_crash(void) {
    dlog_crumb("XBRUN_DEBUG_CRASH: deliberate %s", debug_crash_kind ? "abort()" : "segfault");
    printf("[diag] XBRUN_DEBUG_CRASH: crashing on purpose at frame %u\n", dlog_frame());
    fflush(stdout);
    if (debug_crash_kind) abort();
    *(volatile uint32_t *)(uintptr_t)8 = 0;   /* a write fault in host code on the calling thread */
    abort();
}

void dlog_reset(void) {
    pthread_mutex_lock(&missing_lock);
    memset(missing, 0, sizeof missing);
    nmissing = 0;
    pthread_mutex_unlock(&missing_lock);
    pthread_mutex_lock(&report_lock);
    memset(vk_kinds, 0, sizeof vk_kinds);
    nvk = 0;
    memset(&underrun_rate, 0, sizeof underrun_rate);
    pthread_mutex_unlock(&report_lock);
    underruns = 0;
    memset(ring, 0, sizeof ring);
    ring_head = 0;
    level_name[0] = 0;
    level_mission = -1;
    device_lost_dumped = 0;
}
