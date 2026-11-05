/* HLE of XAPI timing (see hle_xapi.h). */
#include "hle_xapi.h"
#include "pacing.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Expected first bytes of QueryPerformanceCounter: mov ecx,[esp+4]; rdtsc; mov [ecx],eax;
 * mov [ecx+4],edx; xor eax,eax; inc eax; ret 4. */
static const uint8_t qpc_sig[] = {0x8b, 0x4c, 0x24, 0x04, 0x0f, 0x31, 0x89, 0x01, 0x89, 0x51, 0x04,
                                  0x33, 0xc0, 0x40, 0xc2, 0x04, 0x00};

uint64_t xapi_tsc_from_time(uint64_t sec, uint32_t nsec) {
    return sec * XBOX_TSC_FREQUENCY + (uint64_t)nsec * XBOX_TSC_FREQUENCY / 1000000000u;
}

static struct timespec boot;   /* the counter starts near zero, as after an Xbox boot */

/* Frame lock (default; --no-frame-lock turns it off): the counter the title reads follows the
 * presented frames (src/pacing.h), one 60 Hz period per Swap while the host keeps up, whole
 * periods towards real time otherwise. The title's step loop (game clock [0x4aca1c], fixed 1/60 s steps in pairs)
 * then runs its pair on every other presented frame instead of slipping against the Swap
 * pacing. Only QueryPerformanceCounter is locked: the title's other time sources (KeTickCount,
 * KeQuerySystemTime, KeQueryInterruptTime) and the host audio mixer stay on real time, at most
 * about one period apart from it. */
int hle_xapi_frame_lock = 1;
static int installed;
static PacingClock pclock;
static pthread_mutex_t pclock_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t host_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((int64_t)t.tv_sec - boot.tv_sec) * 1000000000LL + (t.tv_nsec - boot.tv_nsec);
}

void hle_xapi_frame_presented(void) {
    if (!installed || !hle_xapi_frame_lock) return;
    int64_t now = host_ns();
    pthread_mutex_lock(&pclock_lock);
    pacing_clock_present(&pclock, now);
    pthread_mutex_unlock(&pclock_lock);
}

int hle_xapi_frame_lock_stats(uint64_t *presents, uint64_t *resyncs) {
    if (!installed || !hle_xapi_frame_lock) return 0;
    pthread_mutex_lock(&pclock_lock);
    *presents = pclock.presents;
    *resyncs = pclock.resyncs;
    pthread_mutex_unlock(&pclock_lock);
    return 1;
}

double hle_xapi_seconds(void) {
    int64_t t = host_ns();
    if (installed && hle_xapi_frame_lock) {
        pthread_mutex_lock(&pclock_lock);
        t = pacing_clock_now(&pclock, t);
        pthread_mutex_unlock(&pclock_lock);
    }
    return (double)t / 1e9;
}

/* BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *count): runs on guest threads, called
 * straight from the patched entry (stdcall, same callee-saved registers as the original). */
static uint32_t __attribute__((stdcall, used)) query_performance_counter(uint64_t *count) {
    int64_t t = host_ns();
    if (hle_xapi_frame_lock) {
        pthread_mutex_lock(&pclock_lock);
        t = pacing_clock_now(&pclock, t);
        pthread_mutex_unlock(&pclock_lock);
    }
    if (t < 0) t = 0;
    *count = xapi_tsc_from_time((uint64_t)(t / 1000000000LL), (uint32_t)(t % 1000000000LL));
    return 1;
}

int hle_xapi_install(HleLookup lookup) {
    clock_gettime(CLOCK_MONOTONIC, &boot);
    pacing_clock_init(&pclock, PACING_PERIOD_60HZ_NS);
    uint32_t s = lookup ? lookup("XAPILIB__QueryPerformanceCounter") : 0;
    if (s && s != XAPI_QPC_ADDR) {
        printf("[xapi] skip QueryPerformanceCounter: symbol at %08x, expected %08x\n", s, XAPI_QPC_ADDR);
        return 0;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)XAPI_QPC_ADDR;
    if (memcmp(p, qpc_sig, sizeof qpc_sig)) {
        printf("[xapi] skip QueryPerformanceCounter at %08x: unexpected entry bytes\n", XAPI_QPC_ADDR);
        return 0;
    }
    int32_t rel = (int32_t)((uintptr_t)query_performance_counter - (uintptr_t)(p + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    installed = 1;
    printf("[xapi] QueryPerformanceCounter replaced (%u Hz from the host monotonic clock; frame lock %s)\n", XBOX_TSC_FREQUENCY,
           hle_xapi_frame_lock ? "on: one 60 Hz period per presented frame, whole periods to real time when half a period off"
                               : "off (--no-frame-lock): real time");
    return 1;
}
