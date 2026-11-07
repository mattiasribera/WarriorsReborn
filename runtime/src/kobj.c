/* Kernel objects. One global lock keeps the object state consistent. A thread blocked in a
 * wait sleeps on its own condition variable and is listed with the objects it waits for; a
 * state change wakes only the threads waiting on that object, which then re-check. (One shared
 * condition variable woke every blocked thread on every change: a guest that polls a mutex
 * hundreds of thousands of times a second, as the Bink movie player does, then kept all the
 * other guest threads spinning through wake-ups.) The global condition variable remains for
 * the service loop (timers, DPCs) and for threads created suspended. */
#define _GNU_SOURCE
#include "kobj.h"
#include "diag.h"
#include "diaglog.h"
#include "kernel.h"
#include "mem.h"
#include "platform.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern uint32_t guest_call(uint32_t fn, uint32_t stack_top, int nargs, const uint32_t *args);

static pthread_mutex_t klock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kcond;
#ifdef KOBJ_TEST
static __thread unsigned kobj_test_wakeups;   /* times this thread returned from a blocking wait */
#define KOBJ_WOKE() (kobj_test_wakeups++)
#else
#define KOBJ_WOKE() ((void)0)
#endif
#define MAX_HANDLES 4096
static HObj handles[MAX_HANDLES];
static uint32_t tick_count;               /* KeTickCount (ms) */
static uint32_t time_increment = 10000;   /* KeTimeIncrement (100ns units) */
static struct timespec boot;

#define U8(p, o) (*(uint8_t *)((uintptr_t)(p) + (o)))
#define I32(p, o) (*(int32_t *)((uintptr_t)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uintptr_t)(p) + (o)))
#define U64(p, o) (*(uint64_t *)((uintptr_t)(p) + (o)))

enum { EVT_NOTIFY = 0, EVT_SYNC = 1, OBJ_MUTANT = 2, OBJ_SEMAPHORE = 5, OBJ_THREAD = 6, TMR_NOTIFY = 8, TMR_SYNC = 9 };

/* ---------- time ---------- */
uint64_t ktime_interrupt(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec - boot.tv_sec) * 10000000ull + (ts.tv_nsec - boot.tv_nsec) / 100;
}
uint64_t ktime_system(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + ts.tv_nsec / 100 + 116444736000000000ull;
}
/* Converts an NT timeout (negative = relative, positive = absolute system time) to an
 * absolute CLOCK_MONOTONIC deadline. Returns 0 for "no timeout". */
static int deadline_from(uint32_t ptimeout, struct timespec *out) {
    if (!ptimeout) return 0;
    int64_t t = *(int64_t *)G(ptimeout);
    int64_t rel = t < 0 ? -t : (int64_t)(t - ktime_system());
    if (rel < 0) rel = 0;
    clock_gettime(CLOCK_MONOTONIC, out);
    out->tv_sec += rel / 10000000;
    out->tv_nsec += (rel % 10000000) * 100;
    if (out->tv_nsec >= 1000000000) { out->tv_sec++; out->tv_nsec -= 1000000000; }
    return 1;
}

/* ---------- handles ---------- */
uint32_t handle_new(HType type, uint32_t body) {
    pthread_mutex_lock(&klock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (handles[i].type == H_FREE) {
            handles[i] = (HObj){.type = type, .body = body, .fd = -1, .refs = 1};
            pthread_mutex_unlock(&klock);
            return (uint32_t)i * 4;
        }
    pthread_mutex_unlock(&klock);
    return 0;
}
HObj *handle_get(uint32_t h) {
    if (h % 4 || h / 4 >= MAX_HANDLES || h == 0) return NULL;
    HObj *o = &handles[h / 4];
    return o->type == H_FREE ? NULL : o;
}
void handle_close(uint32_t h) {
    HObj *o = handle_get(h);
    if (!o) return;
    if (o->fd >= 0) close(o->fd);
    free(o->path);
    free(o->dir);
    o->type = H_FREE;
}

/* ---------- dispatcher objects ---------- */
static void init_header(uint32_t obj, uint8_t type, uint8_t size_dwords, int32_t signal) {
    U8(obj, 0) = type; U8(obj, 1) = 0; U8(obj, 2) = size_dwords; U8(obj, 3) = 0;
    I32(obj, 4) = signal;
    U32(obj, 8) = obj + 8; U32(obj, 12) = obj + 8;
}
static uint32_t cur_kthread(void) { XThread *t = xthread_current(); return t ? GA(t->ethread) : 0; }

/* Caller holds klock. */
static int signaled(uint32_t obj) {
    if (U8(obj, 0) == OBJ_MUTANT) return I32(obj, 4) > 0 || U32(obj, 0x18) == cur_kthread();
    return I32(obj, 4) > 0;
}
static void consume(uint32_t obj) {
    switch (U8(obj, 0)) {
    case EVT_SYNC: case TMR_SYNC: I32(obj, 4) = 0; break;
    case OBJ_SEMAPHORE: I32(obj, 4)--; break;
    case OBJ_MUTANT: I32(obj, 4)--; U32(obj, 0x18) = cur_kthread(); break;
    default: break;
    }
}

/* A thread blocked in a kernel wait (wait_objects or a critical section). Listed under klock
 * while it sleeps; wake_locked signals the waiters that list the changed object. */
typedef struct KWaiter {
    const uint32_t *objs;
    int n;
    pthread_cond_t cond;
    struct KWaiter *next;
} KWaiter;
static KWaiter *waiters;
static __thread KWaiter self_waiter;
static __thread int self_waiter_ready;

/* Longest sleep before a blocked thread re-checks its objects anyway: a safety net for a state
 * changed without a kernel call (reported once), cheap at 10 re-checks a second. */
#define WAIT_RECHECK_NS 100000000L
static int recheck_reported;   /* written under klock */

static KWaiter *waiter_add_locked(const uint32_t *objs, int n) {
    KWaiter *w = &self_waiter;
    if (!self_waiter_ready) { plat_cond_init_monotonic(&w->cond); self_waiter_ready = 1; }
    w->objs = objs;
    w->n = n;
    w->next = waiters;
    waiters = w;
    return w;
}
static void waiter_remove_locked(KWaiter *w) {
    for (KWaiter **p = &waiters; *p; p = &(*p)->next)
        if (*p == w) { *p = w->next; return; }
}
/* Wakes the threads waiting on obj. Caller holds klock. */
static void wake_locked(uint32_t obj) {
    for (KWaiter *w = waiters; w; w = w->next)
        for (int i = 0; i < w->n; i++)
            if (w->objs[i] == obj) { pthread_cond_signal(&w->cond); break; }
}
enum { SLEEP_WOKEN, SLEEP_RECHECK, SLEEP_DEADLINE };
/* Sleeps on w until woken, the deadline (if any) or the re-check interval. Caller holds klock. */
static int waiter_sleep_locked(KWaiter *w, const struct timespec *dl) {
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += WAIT_RECHECK_NS;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    int capped = !dl || until.tv_sec < dl->tv_sec || (until.tv_sec == dl->tv_sec && until.tv_nsec < dl->tv_nsec);
    if (plat_cond_timedwait_mono(&w->cond, &klock, capped ? &until : dl) != ETIMEDOUT) {
        KOBJ_WOKE();
        return SLEEP_WOKEN;
    }
    return capped ? SLEEP_RECHECK : SLEEP_DEADLINE;
}

/* Waits on guest object pointers. Returns STATUS_WAIT_0+i, or STATUS_TIMEOUT. */
static uint32_t wait_objects(int n, const uint32_t *objs, int wait_all, uint32_t ptimeout) {
    struct timespec dl;
    int has_dl = deadline_from(ptimeout, &dl);
    KWaiter *w = NULL;
    int last = SLEEP_WOKEN;
    uint32_t r = STATUS_TIMEOUT;
    pthread_mutex_lock(&klock);
    for (;;) {
        if (wait_all) {
            int ok = 1;
            for (int i = 0; i < n; i++) ok &= signaled(objs[i]);
            if (ok) { for (int i = 0; i < n; i++) consume(objs[i]); r = 0; break; }
        } else {
            int i = 0;
            while (i < n && !signaled(objs[i])) i++;
            if (i < n) { consume(objs[i]); r = (uint32_t)i; break; }
        }
        if (!w) w = waiter_add_locked(objs, n);
        if ((last = waiter_sleep_locked(w, has_dl ? &dl : NULL)) == SLEEP_DEADLINE) break;
    }
    if (w) waiter_remove_locked(w);
    if (last == SLEEP_RECHECK && r != STATUS_TIMEOUT && !recheck_reported) {
        recheck_reported = 1;
        klog("[krnl] a wait on %08x was satisfied by the periodic re-check (state changed without a kernel call)\n",
             objs[r]);
    }
    pthread_mutex_unlock(&klock);
    return r;
}

int32_t kobj_set_signal(uint32_t obj, int32_t value) {
    pthread_mutex_lock(&klock);
    int32_t prev = I32(obj, 4);
    I32(obj, 4) = value;
    wake_locked(obj);
    pthread_mutex_unlock(&klock);
    return prev;
}

static uint32_t handle_body(uint32_t h, uint32_t *out) {
    HObj *o = handle_get(h);
    if (!o || !o->body) return STATUS_INVALID_HANDLE;
    *out = o->body;
    return STATUS_SUCCESS;
}

/* ---------- threads ---------- */
static XThread *threads;
typedef struct { XThread *t; int entry_noargs; } StartInfo;

/* Host frame of the running guest thread. glibc's pthread_exit does a DWARF forced unwind,
 * which must not walk guest frames (no CFI), so a thread exiting from inside a kernel call
 * jumps back to host_thread_main and returns from there. The compiler builtins restore only
 * registers (no CRT unwinding: the Windows CRT longjmp would walk SEH frames). */
typedef void *ExitJmp[5];
static __thread ExitJmp *thread_exit_jb;

static uint32_t live_threads;   /* guest threads started and not yet exited */
uint32_t kobj_live_threads(void) { return __atomic_load_n(&live_threads, __ATOMIC_RELAXED); }

static void thread_exit_current(uint32_t status) {
    XThread *t = xthread_current();
    dlog_crumb("thread t%u exit (status %x)", t ? t->tid : 0, status);
    if (t) {
        U32(t->ethread, 0x120) = status;   /* ETHREAD.ExitStatus */
        kobj_set_signal(GA(t->ethread), 1);
    }
    if (thread_exit_jb) __builtin_longjmp(*thread_exit_jb, 1);
    pthread_exit(NULL);
}

static void *host_thread_main(void *p) {
    StartInfo si = *(StartInfo *)p;
    free(p);
    XThread *t = si.t;
    t->host_thread = plat_thread_self();
    diag_thread_init();
    xthread_bind(t, 1);
    pthread_mutex_lock(&klock);
    while (t->suspended) pthread_cond_wait(&kcond, &klock);
    pthread_mutex_unlock(&klock);
    ExitJmp jb;
    if (__builtin_setjmp(jb)) return NULL;           /* thread_exit_current from guest code */
    thread_exit_jb = &jb;
    if (si.entry_noargs) {
        guest_call(t->start_routine, t->stack_top_for_call, 0, NULL);
        klog("[krnl] t%u entry point returned; terminating thread\n", t->tid);
    } else if (t->system_routine) {
        uint32_t args[2] = {t->start_routine, t->start_context};
        guest_call(t->system_routine, t->stack_top_for_call, 2, args);
    } else {
        uint32_t args[1] = {t->start_context};
        guest_call(t->start_routine, t->stack_top_for_call, 1, args);
    }
    thread_exit_current(0);
    return NULL;
}

/* The host thread's own end: after the guest thread exited (either way out of host_thread_main). */
static void *host_thread_entry(void *p) {
    void *r = host_thread_main(p);
    __atomic_sub_fetch(&live_threads, 1, __ATOMIC_RELAXED);
    return r;
}

static void spawn(XThread *t, int entry_noargs) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    StartInfo *si = malloc(sizeof *si);
    *si = (StartInfo){t, entry_noargs};
    pthread_t pt;
    __atomic_add_fetch(&live_threads, 1, __ATOMIC_RELAXED);
    int err = pthread_create(&pt, &attr, host_thread_entry, si);
    if (err) {   /* pthread_create returns the error; errno is not set */
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        plat_report_memory("guest thread creation failed");
        plat_flush_output();
        dlog_crash_dump("guest thread creation failed");
        exit(1);
    }
    pthread_attr_destroy(&attr);
}

void kobj_start_entry(uint32_t entry, uint32_t stack_size) {
    XThread *t = xthread_new(stack_size, 0, 0);
    if (!t) { plat_flush_output(); exit(1); }
    t->start_routine = entry;
    U32(t->ethread, ETHREAD_START_ADDRESS) = entry;
    t->next = threads;
    threads = t;
    dlog_crumb("thread t%u create (entry %08x)", t->tid, entry);
    spawn(t, 1);
}

/* PsCreateSystemThreadEx(ThreadHandle, ThreadExtensionSize, KernelStackSize, TlsDataSize,
 *   ThreadId, StartRoutine, StartContext, CreateSuspended, DebuggerThread, SystemRoutine) */
static uint64_t k_PsCreateSystemThreadEx(KFrame *f, const uint32_t *a) {
    XThread *t = xthread_new(a[2], a[3], a[1]);
    if (!t) return STATUS_NO_MEMORY;
    t->start_routine = a[5];
    t->start_context = a[6];
    t->system_routine = a[9];
    t->suspended = (uint8_t)a[7];
    U32(t->ethread, ETHREAD_START_ADDRESS) = a[5];
    t->next = threads;
    threads = t;
    uint32_t h = handle_new(H_THREAD, GA(t->ethread));
    if (a[0]) U32(a[0], 0) = h;
    if (a[4]) U32(a[4], 0) = t->tid;
    klog("[krnl] creating t%u: start %08x ctx %08x system %08x stack %x tls %x%s\n", t->tid, a[5], a[6], a[9],
         a[2], a[3], t->suspended ? " (suspended)" : "");
    dlog_crumb("thread t%u create (start %08x from %08x)", t->tid, a[5], f->ret_addr);
    spawn(t, 0);
    return STATUS_SUCCESS;
}

static uint64_t k_PsTerminateSystemThread(KFrame *f, const uint32_t *a) {
    XThread *t = xthread_current();
    if (klog_enabled) klog("[krnl] t%u 258 PsTerminateSystemThread(%x)  [from %08x]\n", t ? t->tid : 0, a[0], f->ret_addr);
    thread_exit_current(a[0]);
    return 0;
}

static XThread *thread_by_kthread(uint32_t kt) {
    for (XThread *t = threads; t; t = t->next) if (GA(t->ethread) == kt) return t;
    return NULL;
}

static uint64_t k_NtResumeThread(KFrame *f, const uint32_t *a) {
    uint32_t body;
    if (handle_body(a[0], &body)) return STATUS_INVALID_HANDLE;
    XThread *t = thread_by_kthread(body);
    pthread_mutex_lock(&klock);
    uint32_t prev = t ? (uint32_t)t->suspended : 0;
    if (t && t->suspended) t->suspended--;
    pthread_cond_broadcast(&kcond);
    pthread_mutex_unlock(&klock);
    if (a[1]) U32(a[1], 0) = prev;
    return STATUS_SUCCESS;
}

static uint64_t k_NtSuspendThread(KFrame *f, const uint32_t *a) {
    kfatal(f, "NtSuspendThread on a running thread is not supported");
}

static uint64_t k_KeQueryBasePriorityThread(KFrame *f, const uint32_t *a) { return 0; }
static uint64_t k_KeSetBasePriorityThread(KFrame *f, const uint32_t *a) { return 0; }
static uint64_t k_KeSetDisableBoostThread(KFrame *f, const uint32_t *a) { return 0; }
static uint64_t k_NtYieldExecution(KFrame *f, const uint32_t *a) { sched_yield(); return STATUS_SUCCESS; }

static uint64_t k_KeDelayExecutionThread(KFrame *f, const uint32_t *a) {
    struct timespec dl;
    if (deadline_from(a[2], &dl)) plat_sleep_until(&dl);
    return STATUS_SUCCESS;
}

/* ---------- events, semaphores, mutants ---------- */
static uint64_t k_NtCreateEvent(KFrame *f, const uint32_t *a) {
    uint32_t obj = GA(calloc(1, 16));
    init_header(obj, a[2] ? EVT_SYNC : EVT_NOTIFY, 4, (uint8_t)a[3]);
    U32(a[0], 0) = handle_new(H_EVENT, obj);
    return STATUS_SUCCESS;
}
static uint64_t k_NtSetEvent(KFrame *f, const uint32_t *a) {
    uint32_t b;
    if (handle_body(a[0], &b)) return STATUS_INVALID_HANDLE;
    int32_t prev = kobj_set_signal(b, 1);
    if (a[1]) I32(a[1], 0) = prev;
    return STATUS_SUCCESS;
}
static uint64_t k_NtClearEvent(KFrame *f, const uint32_t *a) {
    uint32_t b;
    if (handle_body(a[0], &b)) return STATUS_INVALID_HANDLE;
    kobj_set_signal(b, 0);
    return STATUS_SUCCESS;
}
static uint64_t k_NtPulseEvent(KFrame *f, const uint32_t *a) {
    uint32_t b;
    if (handle_body(a[0], &b)) return STATUS_INVALID_HANDLE;
    int32_t prev = kobj_set_signal(b, 1);   /* approximation: waiters woken now may miss it */
    sched_yield();
    kobj_set_signal(b, 0);
    if (a[1]) I32(a[1], 0) = prev;
    return STATUS_SUCCESS;
}
static uint64_t k_KeSetEvent(KFrame *f, const uint32_t *a) { return (uint32_t)kobj_set_signal(a[0], 1); }

static uint64_t k_NtCreateSemaphore(KFrame *f, const uint32_t *a) {
    uint32_t obj = GA(calloc(1, 20));
    init_header(obj, OBJ_SEMAPHORE, 5, (int32_t)a[2]);
    I32(obj, 16) = (int32_t)a[3];
    U32(a[0], 0) = handle_new(H_SEMAPHORE, obj);
    return STATUS_SUCCESS;
}
static uint64_t k_NtReleaseSemaphore(KFrame *f, const uint32_t *a) {
    uint32_t b;
    if (handle_body(a[0], &b)) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock(&klock);
    int32_t prev = I32(b, 4);
    uint32_t st = STATUS_SUCCESS;
    if (prev + (int32_t)a[1] > I32(b, 16)) st = STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    else I32(b, 4) = prev + (int32_t)a[1];
    wake_locked(b);
    pthread_mutex_unlock(&klock);
    if (a[2]) I32(a[2], 0) = prev;
    return st;
}

static uint64_t k_NtCreateMutant(KFrame *f, const uint32_t *a) {
    uint32_t obj = GA(calloc(1, 32));
    init_header(obj, OBJ_MUTANT, 8, a[2] ? 0 : 1);
    if (a[2]) U32(obj, 0x18) = cur_kthread();
    U32(a[0], 0) = handle_new(H_MUTANT, obj);
    return STATUS_SUCCESS;
}
static uint64_t k_NtReleaseMutant(KFrame *f, const uint32_t *a) {
    uint32_t b;
    if (handle_body(a[0], &b)) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock(&klock);
    int32_t prev = I32(b, 4);
    uint32_t st = STATUS_SUCCESS;
    if (U32(b, 0x18) != cur_kthread()) st = STATUS_MUTANT_NOT_OWNED;
    else if (++I32(b, 4) > 0) { I32(b, 4) = 1; U32(b, 0x18) = 0; }
    wake_locked(b);
    pthread_mutex_unlock(&klock);
    if (a[1]) I32(a[1], 0) = prev;
    return st;
}

/* ---------- waits ---------- */
static uint64_t k_KeWaitForSingleObject(KFrame *f, const uint32_t *a) {
    return wait_objects(1, &a[0], 0, a[4]);
}
static uint64_t k_NtWaitForSingleObjectEx(KFrame *f, const uint32_t *a) {
    uint32_t b;
    if (handle_body(a[0], &b)) return STATUS_INVALID_HANDLE;
    return wait_objects(1, &b, 0, f->ordinal == 233 ? a[2] : a[3]);
}
/* NtWaitForMultipleObjectsEx(Count, Handles, WaitType, WaitMode, Alertable, Timeout) */
static uint64_t k_NtWaitForMultipleObjectsEx(KFrame *f, const uint32_t *a) {
    uint32_t objs[64], n = a[0] > 64 ? 64 : a[0];
    for (uint32_t i = 0; i < n; i++)
        if (handle_body(U32(a[1], 4 * i), &objs[i])) return STATUS_INVALID_HANDLE;
    return wait_objects(n, objs, a[2] == 0 /* WaitAll */, a[5]);
}
/* NtSignalAndWaitForSingleObjectEx(SignalHandle, WaitHandle, WaitMode, Alertable, Timeout) */
static uint64_t k_NtSignalAndWaitForSingleObjectEx(KFrame *f, const uint32_t *a) {
    HObj *s = handle_get(a[0]);
    uint32_t b;
    if (!s || handle_body(a[1], &b)) return STATUS_INVALID_HANDLE;
    if (s->type == H_EVENT) kobj_set_signal(s->body, 1);
    else if (s->type == H_SEMAPHORE) { uint32_t r[3] = {a[0], 1, 0}; k_NtReleaseSemaphore(f, r); }
    else if (s->type == H_MUTANT) { uint32_t r[2] = {a[0], 0}; k_NtReleaseMutant(f, r); }
    else kfatal(f, "NtSignalAndWait on handle type %d", s->type);
    return wait_objects(1, &b, 0, a[4]);
}

static uint64_t k_NtClose(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o) return STATUS_INVALID_HANDLE;
    const char *movie = o->type == H_FILE ? dlog_movie_name(o->path) : NULL;
    if (movie) dlog_crumb("movie end %s", movie);
    handle_close(a[0]);
    return STATUS_SUCCESS;
}
static uint64_t k_NtDuplicateObject(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o) return STATUS_INVALID_HANDLE;
    HObj copy = *o;
    uint32_t h = handle_new(copy.type, copy.body);
    HObj *n = handle_get(h);
    n->fd = copy.fd >= 0 ? dup(copy.fd) : -1;
    n->path = copy.path ? strdup(copy.path) : NULL;
    if (a[1]) U32(a[1], 0) = h;
    if (a[2] & 1 /* DUPLICATE_CLOSE_SOURCE */) handle_close(a[0]);
    return STATUS_SUCCESS;
}
/* ObReferenceObjectByHandle(Handle, ObjectType, ReturnedObject) */
static uint64_t k_ObReferenceObjectByHandle(KFrame *f, const uint32_t *a) {
    uint32_t b;
    if (a[0] == 0xFFFFFFFE) b = cur_kthread();   /* NtCurrentThread() */
    else if (handle_body(a[0], &b)) return STATUS_INVALID_HANDLE;
    U32(a[2], 0) = b;
    return STATUS_SUCCESS;
}
static uint64_t k_ObfDereferenceObject(KFrame *f, const uint32_t *a) { return 0; }

/* ---------- critical sections (layout: event header, LockCount, RecursionCount, OwningThread) ---------- */
static uint64_t k_RtlInitializeCriticalSection(KFrame *f, const uint32_t *a) {
    init_header(a[0], EVT_SYNC, 4, 0);
    I32(a[0], 0x10) = -1;
    I32(a[0], 0x14) = 0;
    U32(a[0], 0x18) = 0;
    return 0;
}
static uint64_t k_RtlEnterCriticalSection(KFrame *f, const uint32_t *a) {
    uint32_t me = cur_kthread(), cs = a[0];
    pthread_mutex_lock(&klock);
    KWaiter *w = NULL;
    while (U32(cs, 0x18) && U32(cs, 0x18) != me) {
        if (!w) w = waiter_add_locked(&cs, 1);
        waiter_sleep_locked(w, NULL);
    }
    if (w) waiter_remove_locked(w);
    U32(cs, 0x18) = me;
    I32(cs, 0x14)++;
    I32(cs, 0x10)++;
    pthread_mutex_unlock(&klock);
    return 0;
}
static uint64_t k_RtlLeaveCriticalSection(KFrame *f, const uint32_t *a) {
    uint32_t cs = a[0];
    pthread_mutex_lock(&klock);
    I32(cs, 0x10)--;
    if (--I32(cs, 0x14) <= 0) { I32(cs, 0x14) = 0; U32(cs, 0x18) = 0; wake_locked(cs); }
    pthread_mutex_unlock(&klock);
    return 0;
}

/* ---------- IRQL ---------- */
static uint64_t k_KfRaiseIrql(KFrame *f, const uint32_t *a) {
    uint8_t *k = kpcr_current(), old = k[0x24];
    k[0x24] = (uint8_t)a[0];
    return old;
}
static uint64_t k_KfLowerIrql(KFrame *f, const uint32_t *a) { kpcr_current()[0x24] = (uint8_t)a[0]; return 0; }
static uint64_t k_KeRaiseIrqlToDpcLevel(KFrame *f, const uint32_t *a) {
    uint8_t *k = kpcr_current(), old = k[0x24];
    k[0x24] = 2;
    return old;
}

/* ---------- timers and DPCs (serviced by the host main thread) ---------- */
#define MAX_TIMERS 256
static uint32_t timers[MAX_TIMERS];
static uint64_t timer_due[MAX_TIMERS];   /* interrupt time */
#define MAX_DPCS 256
static uint32_t dpc_queue[MAX_DPCS];
static int ndpc;

static void timer_remove_locked(uint32_t t) {
    for (int i = 0; i < MAX_TIMERS; i++) if (timers[i] == t) timers[i] = 0;
}
static uint64_t k_KeInitializeTimerEx(KFrame *f, const uint32_t *a) {
    init_header(a[0], a[1] ? TMR_SYNC : TMR_NOTIFY, 10, 0);
    U64(a[0], 0x10) = 0; U32(a[0], 0x20) = 0; I32(a[0], 0x24) = 0;
    return 0;
}
/* KeSetTimer(Timer, DueTime (by value), Dpc) / KeSetTimerEx(+Period) */
static uint64_t k_KeSetTimer(KFrame *f, const uint32_t *a) {
    uint32_t t = a[0];
    int64_t due = (int64_t)((uint64_t)a[2] << 32 | a[1]);
    int64_t rel = due < 0 ? -due : (int64_t)(due - ktime_system());
    pthread_mutex_lock(&klock);
    int was = 0, slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) { if (timers[i] == t) { was = 1; slot = i; } }
    for (int i = 0; slot < 0 && i < MAX_TIMERS; i++) if (!timers[i]) slot = i;
    if (slot < 0) { pthread_mutex_unlock(&klock); kfatal(f, "timer table full"); }
    timers[slot] = t;
    timer_due[slot] = ktime_interrupt() + (rel > 0 ? rel : 0);
    I32(t, 4) = 0;
    U32(t, 0x20) = a[3];
    I32(t, 0x24) = 0;   /* KeSetTimerEx (periodic) is not imported */
    pthread_cond_broadcast(&kcond);
    pthread_mutex_unlock(&klock);
    return was;
}
static uint64_t k_KeCancelTimer(KFrame *f, const uint32_t *a) {
    pthread_mutex_lock(&klock);
    int was = 0;
    for (int i = 0; i < MAX_TIMERS; i++) if (timers[i] == a[0]) was = 1;
    timer_remove_locked(a[0]);
    pthread_mutex_unlock(&klock);
    return was;
}
static uint64_t k_KeInitializeDpc(KFrame *f, const uint32_t *a) {
    memset(G(a[0]), 0, 0x1C);
    U8(a[0], 0) = 19;   /* DpcObject */
    U32(a[0], 0xC) = a[1];
    U32(a[0], 0x10) = a[2];
    return 0;
}
static int dpc_insert_locked(uint32_t dpc, uint32_t a1, uint32_t a2) {
    if (U8(dpc, 2) || ndpc >= MAX_DPCS) return 0;
    U8(dpc, 2) = 1;
    U32(dpc, 0x14) = a1;
    U32(dpc, 0x18) = a2;
    dpc_queue[ndpc++] = dpc;
    pthread_cond_broadcast(&kcond);
    return 1;
}
static uint64_t k_KeInsertQueueDpc(KFrame *f, const uint32_t *a) {
    pthread_mutex_lock(&klock);
    int r = dpc_insert_locked(a[0], a[1], a[2]);
    pthread_mutex_unlock(&klock);
    return r;
}
static uint64_t k_KeRemoveQueueDpc(KFrame *f, const uint32_t *a) {
    pthread_mutex_lock(&klock);
    int r = 0;
    for (int i = 0; i < ndpc; i++)
        if (dpc_queue[i] == a[0]) { dpc_queue[i] = dpc_queue[--ndpc]; U8(a[0], 2) = 0; r = 1; break; }
    pthread_mutex_unlock(&klock);
    return r;
}

int watchdog_seconds;
static void watchdog_sample(void) {
    klog("[watchdog] %d s elapsed without exit; sampling guest threads\n", watchdog_seconds);
    for (XThread *t = threads; t; t = t->next)
        if (t->host_thread && !*(int32_t *)(t->ethread + 4)) plat_thread_sample(t->host_thread), usleep(200000);
    plat_flush_output();
    _exit(4);
}

/* Signals the timers due at `now` and queues their DPCs. Caller holds klock. */
static void expire_timers_locked(uint64_t now) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        uint32_t t = timers[i];
        if (!t || timer_due[i] > now) continue;
        I32(t, 4) = 1;
        if (U32(t, 0x20)) dpc_insert_locked(U32(t, 0x20), (uint32_t)ktime_system(), (uint32_t)(ktime_system() >> 32));
        if (I32(t, 0x24) > 0) timer_due[i] = now + (uint64_t)I32(t, 0x24) * 10000;
        else timers[i] = 0;
        wake_locked(t);
    }
}

/* Host main thread: KeTickCount, timer expiry and DPC execution (at IRQL 2). */
void (*kobj_idle_hook)(void);

void kobj_service_loop(void) {
    XThread *svc = xthread_new(0x40000, 0, 0);
    if (!svc) { plat_flush_output(); exit(1); }
    xthread_bind(svc, 0);   /* host main thread: own stack; DPCs switch to svc's stack */
    svc->kpcr[0x24] = 2;
    for (;;) {
        pthread_mutex_lock(&klock);
        struct timespec dl;
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_nsec += 1000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
        if (!ndpc) plat_cond_timedwait_mono(&kcond, &klock, &dl);
        uint64_t now = ktime_interrupt();
        tick_count = (uint32_t)(now / 10000);
        if (watchdog_seconds && now / 10000000 >= (uint64_t)watchdog_seconds) { pthread_mutex_unlock(&klock); watchdog_sample(); }
        expire_timers_locked(now);
        uint32_t run[MAX_DPCS];
        int nrun = ndpc;
        memcpy(run, dpc_queue, nrun * sizeof *run);
        ndpc = 0;
        for (int i = 0; i < nrun; i++) U8(run[i], 2) = 0;
        pthread_mutex_unlock(&klock);
        for (int i = 0; i < nrun; i++) {
            uint32_t d = run[i];
            uint32_t args[4] = {d, U32(d, 0x10), U32(d, 0x14), U32(d, 0x18)};
            guest_call(U32(d, 0xC), svc->stack_top_for_call, 4, args);
        }
        if (kobj_idle_hook) kobj_idle_hook();
    }
}

void kobj_register(void) {
    plat_cond_init_monotonic(&kcond);
    clock_gettime(CLOCK_MONOTONIC, &boot);
    kernel_register_data(156, &tick_count);
    kernel_register_data(157, &time_increment);
    kernel_register(255, k_PsCreateSystemThreadEx);
    kernel_register(258, k_PsTerminateSystemThread);
    kernel_register(224, k_NtResumeThread);
    kernel_register(231, k_NtSuspendThread);
    kernel_register_stub(124, k_KeQueryBasePriorityThread);
    kernel_register_stub(143, k_KeSetBasePriorityThread);
    kernel_register_stub(144, k_KeSetDisableBoostThread);
    kernel_register(238, k_NtYieldExecution);
    kernel_register(99, k_KeDelayExecutionThread);
    kernel_register(189, k_NtCreateEvent);
    kernel_register(225, k_NtSetEvent);
    kernel_register(186, k_NtClearEvent);
    kernel_register(205, k_NtPulseEvent);
    kernel_register(145, k_KeSetEvent);
    kernel_register(193, k_NtCreateSemaphore);
    kernel_register(222, k_NtReleaseSemaphore);
    kernel_register(192, k_NtCreateMutant);
    kernel_register(221, k_NtReleaseMutant);
    kernel_register(159, k_KeWaitForSingleObject);
    kernel_register(233, k_NtWaitForSingleObjectEx);
    kernel_register(234, k_NtWaitForSingleObjectEx);
    kernel_register(235, k_NtWaitForMultipleObjectsEx);
    kernel_register(230, k_NtSignalAndWaitForSingleObjectEx);
    kernel_register(187, k_NtClose);
    kernel_register(197, k_NtDuplicateObject);
    kernel_register(246, k_ObReferenceObjectByHandle);
    kernel_register_stub(250, k_ObfDereferenceObject);
    kernel_register(291, k_RtlInitializeCriticalSection);
    kernel_register(277, k_RtlEnterCriticalSection);
    kernel_register(294, k_RtlLeaveCriticalSection);
    kernel_register(160, k_KfRaiseIrql);
    kernel_register(161, k_KfLowerIrql);
    kernel_register(129, k_KeRaiseIrqlToDpcLevel);
    kernel_register(113, k_KeInitializeTimerEx);
    kernel_register(149, k_KeSetTimer);
    kernel_register(97, k_KeCancelTimer);
    kernel_register(107, k_KeInitializeDpc);
    kernel_register(119, k_KeInsertQueueDpc);
    kernel_register(137, k_KeRemoveQueueDpc);
}

#ifdef KOBJ_TEST
/* Game-independent test (make build/kobj_test): waits, wakeups and hand-offs between host
 * threads that stand in for guest threads (guest addresses are host addresses in this 32-bit
 * process, so the handlers are called directly). */
#include <stdarg.h>

int klog_enabled;
void klog(const char *fmt, ...) { (void)fmt; }
void kfatal(KFrame *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL kfatal: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}
void kernel_register(uint32_t ordinal, KHandler h) { (void)ordinal; (void)h; }
void kernel_register_stub(uint32_t ordinal, KHandler h) { (void)ordinal; (void)h; }
void kernel_register_data(uint32_t ordinal, void *data) { (void)ordinal; (void)data; }
void diag_thread_init(void) {}
uint32_t guest_call(uint32_t fn, uint32_t stack_top, int nargs, const uint32_t *args) { return 0; }
static __thread XThread *cur;
XThread *xthread_current(void) { return cur; }
void xthread_bind(XThread *t, int guest_stack) { cur = t; }
XThread *xthread_new(uint32_t stack_size, uint32_t tls_size, uint32_t ext_size) {
    XThread *t = calloc(1, sizeof *t);
    t->ethread = calloc(1, 0x200);
    return t;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define P(x) ((uint32_t)(uintptr_t)(x))

/* Every test thread gets its own KTHREAD so mutant ownership works. */
static void as_guest_thread(void) { if (!cur) xthread_bind(xthread_new(0, 0, 0), 0); }

static uint32_t create_event(int sync, int state) {
    uint32_t h = 0, a[4] = {P(&h), 0, (uint32_t)sync, (uint32_t)state};
    k_NtCreateEvent(NULL, a);
    return h;
}
static uint32_t create_mutant(int owned) {
    uint32_t h = 0, a[3] = {P(&h), 0, (uint32_t)owned};
    as_guest_thread();
    k_NtCreateMutant(NULL, a);
    return h;
}
static uint32_t create_semaphore(int count, int limit) {
    uint32_t h = 0, a[4] = {P(&h), 0, (uint32_t)count, (uint32_t)limit};
    k_NtCreateSemaphore(NULL, a);
    return h;
}
static uint32_t set_event(uint32_t h) { uint32_t a[2] = {h, 0}; return (uint32_t)k_NtSetEvent(NULL, a); }
static uint32_t release_mutant(uint32_t h) { uint32_t a[2] = {h, 0}; return (uint32_t)k_NtReleaseMutant(NULL, a); }
static uint32_t release_semaphore(uint32_t h) { uint32_t a[3] = {h, 1, 0}; return (uint32_t)k_NtReleaseSemaphore(NULL, a); }
/* NtWaitForSingleObjectEx(Handle, WaitMode, Alertable, Timeout); timeout_ms < 0: infinite. */
static uint32_t wait1(uint32_t h, int timeout_ms) {
    int64_t t = -(int64_t)timeout_ms * 10000;
    uint32_t a[4] = {h, 1, 0, timeout_ms < 0 ? 0 : P(&t)};
    KFrame f = {.ordinal = 234};
    as_guest_thread();
    return (uint32_t)k_NtWaitForSingleObjectEx(&f, a);
}
static uint32_t wait_n(uint32_t n, const uint32_t *hs, int all, int timeout_ms) {
    int64_t t = -(int64_t)timeout_ms * 10000;
    uint32_t a[6] = {n, P(hs), all ? 0u : 1u, 1, 0, timeout_ms < 0 ? 0 : P(&t)};
    as_guest_thread();
    return (uint32_t)k_NtWaitForMultipleObjectsEx(NULL, a);
}
static void ms_sleep(int ms) { struct timespec d = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&d, NULL); }

/* A waiter thread: waits on its handles and records the status and how often it was woken. */
typedef struct {
    uint32_t hs[4], n, all;
    int timeout_ms;
    volatile uint32_t status;
    volatile int done;
    unsigned wakeups;
    pthread_t th;
} Waiter_;
static void *waiter_main(void *p) {
    Waiter_ *w = p;
    kobj_test_wakeups = 0;
    w->status = w->n == 1 ? wait1(w->hs[0], w->timeout_ms) : wait_n(w->n, w->hs, (int)w->all, w->timeout_ms);
    w->wakeups = kobj_test_wakeups;
    __atomic_store_n(&w->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void start_waiter(Waiter_ *w) {
    pthread_create(&w->th, NULL, waiter_main, w);
    ms_sleep(30);   /* let it block */
}
static int finished(Waiter_ *w, int ms) {
    for (int i = 0; i < ms && !__atomic_load_n(&w->done, __ATOMIC_SEQ_CST); i++) ms_sleep(1);
    return __atomic_load_n(&w->done, __ATOMIC_SEQ_CST);
}

static void test_event_wakes_waiter(void) {
    uint32_t e = create_event(1, 0);
    Waiter_ w = {.hs = {e}, .n = 1, .timeout_ms = -1};
    start_waiter(&w);
    CHECK(!w.done, "waiter returned before the event was set");
    set_event(e);
    CHECK(finished(&w, 2000) && w.status == 0, "event: waiter not released (status %x)", w.status);
    pthread_join(w.th, NULL);
}

/* The core of the change: a busy mutex on another object must not wake a blocked waiter. */
static void test_unrelated_releases_do_not_wake(void) {
    uint32_t e = create_event(1, 0), m = create_mutant(0), other = create_event(0, 0);
    Waiter_ w = {.hs = {e}, .n = 1, .timeout_ms = -1};
    start_waiter(&w);
    for (int i = 0; i < 20000; i++) {
        CHECK(wait1(m, -1) == 0, "mutex acquire %d", i);
        release_mutant(m);
        if (!(i % 1000)) { set_event(other); uint32_t c[2] = {other, 0}; k_NtClearEvent(NULL, c); }
    }
    set_event(e);
    CHECK(finished(&w, 2000) && w.status == 0, "waiter not released after the poll loop");
    pthread_join(w.th, NULL);
    CHECK(w.wakeups <= 2, "waiter on an idle event was woken %u times by unrelated releases", w.wakeups);
}

static void test_mutant_handoff(void) {
    uint32_t m = create_mutant(1);   /* owned by this thread */
    Waiter_ w = {.hs = {m}, .n = 1, .timeout_ms = -1};
    start_waiter(&w);
    CHECK(!w.done, "waiter got an owned mutant");
    CHECK(release_mutant(m) == 0, "owner release");
    CHECK(finished(&w, 2000) && w.status == 0, "mutant not handed over (status %x)", w.status);
    pthread_join(w.th, NULL);
    CHECK(release_mutant(m) == STATUS_MUTANT_NOT_OWNED, "the waiter, not the old owner, owns the mutant");
}

static void test_semaphore_release(void) {
    uint32_t s = create_semaphore(0, 4);
    Waiter_ w = {.hs = {s}, .n = 1, .timeout_ms = -1};
    start_waiter(&w);
    CHECK(!w.done, "waiter passed an empty semaphore");
    release_semaphore(s);
    CHECK(finished(&w, 2000) && w.status == 0, "semaphore: waiter not released");
    pthread_join(w.th, NULL);
}

static void test_wait_any_and_all(void) {
    uint32_t a = create_event(1, 0), b = create_event(1, 0);
    Waiter_ any = {.hs = {a, b}, .n = 2, .all = 0, .timeout_ms = -1};
    start_waiter(&any);
    set_event(b);
    CHECK(finished(&any, 2000) && any.status == 1, "wait-any: expected index 1, got %x", any.status);
    pthread_join(any.th, NULL);

    Waiter_ all = {.hs = {a, b}, .n = 2, .all = 1, .timeout_ms = -1};
    start_waiter(&all);
    set_event(a);
    ms_sleep(30);
    CHECK(!all.done, "wait-all returned with one of two events set");
    set_event(b);
    CHECK(finished(&all, 2000) && all.status == 0, "wait-all: not released with both set");
    pthread_join(all.th, NULL);
}

static void test_timeout(void) {
    uint32_t e = create_event(0, 0);
    CHECK(wait1(e, 20) == STATUS_TIMEOUT, "unsignaled event: expected STATUS_TIMEOUT");
    CHECK(wait1(e, 0) == STATUS_TIMEOUT, "zero timeout on an unsignaled event");
    set_event(e);
    CHECK(wait1(e, 0) == 0, "notification event stays signaled");
}

static void test_kobj_set_signal_wakes(void) {
    XThread *t = xthread_new(0, 0, 0);   /* a thread object, signaled when the thread exits */
    uint32_t h = handle_new(H_THREAD, P(t->ethread));
    Waiter_ w = {.hs = {h}, .n = 1, .timeout_ms = -1};
    start_waiter(&w);
    kobj_set_signal(P(t->ethread), 1);
    CHECK(finished(&w, 2000) && w.status == 0, "thread-exit signal did not release the waiter");
    pthread_join(w.th, NULL);
}

static uint32_t cs_mem[8];
static volatile int cs_entered;
static void *cs_main(void *p) {
    uint32_t a[1] = {P(cs_mem)};
    as_guest_thread();
    k_RtlEnterCriticalSection(NULL, a);
    __atomic_store_n(&cs_entered, 1, __ATOMIC_SEQ_CST);
    k_RtlLeaveCriticalSection(NULL, a);
    return NULL;
}
static void test_critical_section_handoff(void) {
    uint32_t a[1] = {P(cs_mem)};
    as_guest_thread();
    k_RtlInitializeCriticalSection(NULL, a);
    k_RtlEnterCriticalSection(NULL, a);
    pthread_t th;
    pthread_create(&th, NULL, cs_main, NULL);
    ms_sleep(30);
    CHECK(!cs_entered, "second thread entered a held critical section");
    k_RtlLeaveCriticalSection(NULL, a);
    for (int i = 0; i < 2000 && !cs_entered; i++) ms_sleep(1);
    CHECK(cs_entered, "critical section not handed over on leave");
    pthread_join(th, NULL);
}

static void kobj_expire_timers(void) {
    pthread_mutex_lock(&klock);
    expire_timers_locked(ktime_interrupt());
    pthread_mutex_unlock(&klock);
}
static void test_timer_expiry_wakes(void) {
    static uint32_t timer[12];
    timer[0] = TMR_SYNC;
    uint32_t h = handle_new(H_TIMER, P(timer));
    uint32_t a[4] = {P(timer), (uint32_t)-100000, 0xffffffffu, 0};   /* due in 10 ms, no DPC */
    k_KeSetTimer(NULL, a);
    Waiter_ w = {.hs = {h}, .n = 1, .timeout_ms = -1};
    start_waiter(&w);
    for (int i = 0; i < 200 && !w.done; i++) { kobj_expire_timers(); ms_sleep(1); }
    CHECK(finished(&w, 2000) && w.status == 0, "timer expiry did not release the waiter");
    pthread_join(w.th, NULL);
}

int main(void) {
    kobj_register();
    test_event_wakes_waiter();
    test_unrelated_releases_do_not_wake();
    test_mutant_handoff();
    test_semaphore_release();
    test_wait_any_and_all();
    test_timeout();
    test_kobj_set_signal_wakes();
    test_critical_section_handoff();
    test_timer_expiry_wakes();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}
#endif
