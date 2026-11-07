/* Robustness diagnostics for the log (README "Log line prefixes"):
 *   [breadcrumb]    the last DLOG_CRUMBS notable events, dumped on every crash or fatal path
 *   [crash-context] level, mission, frame, uptime, guest arena use and threads at that moment
 *   [hle-missing]   the first call of each stubbed / unimplemented HLE function, and a summary
 *   [vk-error]      non-success VkResults the renderer would otherwise ignore (rate-limited)
 *   [audio]         audio device underruns (rate-limited) and their total
 *
 * Game-independent and dependency-free (libc and write()/the Windows log only), so the unit
 * tests and every test binary that links a caller can link it as is. The hot path only pays
 * for an atomic add and a short vsnprintf per breadcrumb (notable events are rare), and for a
 * cached slot load per call of a stubbed function. */
#ifndef DIAGLOG_H
#define DIAGLOG_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DLOG_CRUMBS 100      /* breadcrumbs kept */
#define DLOG_CRUMB_TEXT 96   /* bytes per breadcrumb text, NUL included (longer text is cut) */
#define DLOG_MISSING_MAX 192 /* distinct missing-function names tracked */

/* Process start (uptime origin) and the XBRUN_DEBUG_CRASH test hook (see dlog_debug_crash). */
void dlog_init(void);
uint64_t dlog_now_ns(void);   /* CLOCK_MONOTONIC */

/* Context sources, registered by main (all optional; unset ones read 0 / unknown). They are
 * called from crash handlers, so they must be lock-free reads. */
typedef struct {
    uint32_t (*frame)(void);            /* presented frames so far */
    uint64_t (*arena_used)(void);       /* guest arena bytes in use */
    uint64_t (*arena_total)(void);      /* guest arena size */
    uint32_t (*threads)(void);          /* live guest threads */
} DlogSources;
void dlog_set_sources(const DlogSources *s);
uint32_t dlog_frame(void);
/* Current level ("level81") and story mission (-1 = unknown); set by the level-load watch. */
void dlog_set_level(const char *level, int mission);

/* The file name of a Bink movie path ("D:\\bik\\l1_in.bik" -> "l1_in.bik"), NULL for other files:
 * the movie start / end breadcrumbs come from its open and close. */
const char *dlog_movie_name(const char *path);

/* ---- breadcrumbs: a lock-free ring of the last DLOG_CRUMBS events ---- */
void dlog_crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Formats the ring, oldest first, as "[breadcrumb] -12.345s frame 1234: <event>\n" lines (time
 * relative to now_ns) into buf, always NUL-terminated. No allocation, no stdio: safe in a crash
 * handler. Returns the length written. */
size_t dlog_crumbs_format(char *buf, size_t cap, uint64_t now_ns);
/* "[crash-context] level L, mission M, frame F, uptime S s, guest arena used U MiB of T MiB,
 * threads N\n" with the same guarantees. */
size_t dlog_context_format(char *buf, size_t cap, uint64_t now_ns);
/* The crash / fatal-path dump: a "[crash-context] reason: <why>" header line, the breadcrumbs
 * and the context line, written with write() (Windows: the console and the log) from a fixed
 * static buffer. Async-signal-safe apart from the registered sources. Callers flush stdio first. */
void dlog_crash_dump(const char *why);

/* ---- missing HLE functionality ---- */
/* Registers name (once per name) and logs "[hle-missing] <name> (first call from 0xADDR, frame F)".
 * ext_count, when given, is the caller's own call counter for that function (read for the
 * summary); otherwise dlog_missing_hit counts. Returns the slot (>= 0), or -1 when the table is full. */
int dlog_missing_first(const char *name, uint32_t caller, const uint32_t *ext_count);
void dlog_missing_hit(int slot);
/* A stubbed path at one call site: logs the first call, counts every call. One slot load and
 * one atomic add per call after the first. */
#define DLOG_MISSING(name, caller)                                                            \
    do {                                                                                      \
        static int dlog_slot_;   /* slot + 1; 0 = not registered yet */                       \
        int dlog_s_ = __atomic_load_n(&dlog_slot_, __ATOMIC_ACQUIRE);                         \
        if (!dlog_s_) {                                                                       \
            dlog_s_ = dlog_missing_first((name), (caller), NULL) + 1;                         \
            __atomic_store_n(&dlog_slot_, dlog_s_, __ATOMIC_RELEASE);                         \
        }                                                                                     \
        if (dlog_s_ > 0) dlog_missing_hit(dlog_s_ - 1);                                       \
    } while (0)
/* "[hle-missing] summary: <name>xN, ..." (most calls first; "none" when nothing was missing). */
size_t dlog_missing_summary_format(char *buf, size_t cap);

/* ---- rate limiting ---- */
/* At most burst events per window_ns. dlog_rate_allow returns 1 when this event may be logged
 * (then *suppressed = the events dropped since the last logged one) and 0 when it is dropped.
 * Not thread-safe by itself; the dlog_* reporters below serialize it. */
typedef struct { uint64_t window_start; uint32_t in_window, suppressed; int started; } DlogRate;
int dlog_rate_allow(DlogRate *r, uint64_t now_ns, uint32_t burst, uint64_t window_ns, uint32_t *suppressed);

/* ---- video and audio errors ---- */
/* A non-success VkResult from call (result_name e.g. "VK_ERROR_DEVICE_LOST"). Logs
 * "[vk-error] <call>: <name> (frame F)" rate-limited per call and result; swapchain is nonzero
 * for VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR, which are logged once and then only counted.
 * device_lost dumps the crash breadcrumbs (once). Every result is counted for the exit summary. */
void dlog_vk_error(const char *call, int result, const char *result_name, int swapchain, int device_lost);
/* An audio device underrun: "[audio] underrun N (frame F)", rate-limited. */
void dlog_audio_underrun(void);
uint32_t dlog_audio_underruns(void);

/* At exit (once): the [hle-missing] summary, "[audio] underruns: N total" and, when any
 * VkResult was reported, "[vk-error] summary: <call> <name> xN, ...". */
void dlog_exit_summary(void);

/* XBRUN_DEBUG_CRASH=FRAME[:abort]: a hidden test hook that crashes on purpose when
 * frame FRAME is presented (a host write fault, or abort()), so the crash dump can be
 * checked. 0 = off. */
extern uint32_t dlog_debug_crash_frame;
void dlog_debug_crash(void);   /* performs the configured crash (does not return) */

/* Tests: forget every breadcrumb, missing name, counter and the level. */
void dlog_reset(void);

#ifdef __cplusplus
}
#endif
#endif
