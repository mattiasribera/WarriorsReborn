/* Linux implementation of src/platform.h (i386 glibc: host TLS in %gs, %fs free for the KPCR). */
#define _GNU_SOURCE
#include "../platform.h"
#include <asm/ldt.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#define PAGE 0x1000u

void plat_init(const char *log_name) {
    (void)log_name;
    setvbuf(stdout, NULL, _IOLBF, 0);
}
void plat_report_memory(const char *why) { (void)why; }
int plat_image_mapped(uint32_t base, uint32_t end) { (void)base; (void)end; return 0; }
static void *probe_main(void *arg) { return arg; }
void plat_thread_probe(const char *phase) {
    pthread_t t;
    int err = pthread_create(&t, NULL, probe_main, NULL);
    if (!err) pthread_join(t, NULL);
    printf("[probe] %s: pthread_create %s (%s)\n", phase, err ? "FAILED" : "ok", err ? strerror(err) : "no error");
}
void plat_flush_output(void) { fflush(stdout); fflush(stderr); }

/* ---- timing ---- */
void plat_sleep_until(const struct timespec *deadline) {
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL) == EINTR) {}
}

void plat_sleep_until_precise(const struct timespec *deadline) { plat_sleep_until(deadline); }
int plat_cond_init_monotonic(pthread_cond_t *c) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    int r = pthread_cond_init(c, &ca);
    pthread_condattr_destroy(&ca);
    return r;
}
int plat_cond_timedwait_mono(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline) {
    return pthread_cond_timedwait(c, m, deadline);
}

/* ---- memory ---- */
/* Pages mapped by plat_map_fixed, one bit per 4 KB page of the 32-bit space (as win32.c), so
 * plat_is_mapped answers for guest memory without a mincore system call (about 1.7 us each here;
 * the draw recording checks about 6 guest ranges per draw, 58% of its time in busy frames). Bits
 * are set after the mmap and cleared before the munmap; guest memory is only freed through
 * plat_unmap. Other memory (host libraries, stacks) falls back to mincore. */
static uint8_t mapped_pages[1u << 17];
static void mark_pages(uintptr_t a, uintptr_t end, int on) {
    for (a >>= 12, end = (end + PAGE - 1) >> 12; a < end; a++) {
        if (on) __atomic_or_fetch(&mapped_pages[a >> 3], (uint8_t)(1u << (a & 7)), __ATOMIC_RELEASE);
        else __atomic_and_fetch(&mapped_pages[a >> 3], (uint8_t)~(1u << (a & 7)), __ATOMIC_RELEASE);
    }
}
void *plat_map_fixed(uint32_t addr, size_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (p != (void *)(uintptr_t)addr) { munmap(p, size); return NULL; }
    mark_pages(addr, (uintptr_t)addr + size, 1);
    return p;
}
void plat_unmap(void *p, size_t size) {
    mark_pages((uintptr_t)p, (uintptr_t)p + size, 0);
    munmap(p, size);
}
void *plat_alloc_exec(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
int plat_is_mapped(uint32_t addr) {
    if (__atomic_load_n(&mapped_pages[addr >> 15], __ATOMIC_ACQUIRE) & (1u << (addr >> 12 & 7))) return 1;
    unsigned char v;
    return mincore((void *)(uintptr_t)(addr & ~(PAGE - 1)), PAGE, &v) == 0;
}
void plat_describe_addr(uint32_t addr, char *buf, size_t n) {
    snprintf(buf, n, "%s", plat_is_mapped(addr) ? "mapped" : "not mapped");
}
uint32_t plat_alloc_granularity(void) { return PAGE; }
uint32_t plat_reserve_guest_range(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; return 0; }

/* Distinct (package, core) pairs among the CPUs of the affinity mask, from sysfs; a CPU without
 * topology files counts as a core of its own. */
static int read_id(int cpu, const char *what) {
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/%s", cpu, what);
    FILE *f = fopen(path, "r");
    int v = -1;
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}
int plat_physical_cores(void) {
    static int cached;
    if (cached) return cached;
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set)) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return cached = n > 0 ? (int)n : 1;
    }
    static uint32_t key[CPU_SETSIZE];
    int n = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &set)) continue;
        int pkg = read_id(cpu, "physical_package_id"), core = read_id(cpu, "core_id");
        uint32_t k = pkg < 0 || core < 0 ? 0x80000000u | (uint32_t)cpu : (uint32_t)pkg << 16 | (uint32_t)(core & 0xFFFF);
        int seen = 0;
        for (int i = 0; i < n && !seen; i++) seen = key[i] == k;
        if (!seen) key[n++] = k;
    }
    return cached = n > 0 ? n : 1;
}

int plat_logical_cores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

double plat_process_cpu_ms(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru)) return 0;
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

/* ---- dynamic libraries ---- */
void *plat_dlopen(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_dlsym(void *lib, const char *sym) { return dlsym(lib, sym); }
const char *plat_dlerror(void) { const char *e = dlerror(); return e ? e : "unknown error"; }
const char *plat_program_dir(void) {
    static char dir[4096];
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    dir[n > 0 ? n : 0] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    return dir;
}

/* ---- KPCR: one GDT TLS slot shared by all threads, per-thread base ---- */
static int fs_entry = -1;
void plat_kpcr_init(void) {}
void plat_kpcr_bind(uint8_t *kpcr, uint32_t stack_base, uint32_t stack_limit, int guest_stack) {
    (void)stack_base; (void)stack_limit; (void)guest_stack;
    struct user_desc d = {0};
    d.entry_number = fs_entry;
    d.base_addr = (uint32_t)(uintptr_t)kpcr;
    d.limit = 0xFFF;
    d.seg_32bit = 1;
    d.useable = 1;
    if (syscall(SYS_set_thread_area, &d) != 0) { perror("set_thread_area"); exit(1); }
    fs_entry = d.entry_number;
    uint16_t sel = (uint16_t)(d.entry_number << 3 | 3);
    __asm__ volatile("movw %0, %%fs" ::"r"(sel));
}
int plat_fs_patch_required(uint32_t *disp) { *disp = 0x1C; return 0; }
uint32_t plat_min_guest_stack(void) { return 0x40000; }

/* ---- faults ---- */
static int (*fault_cb)(PlatRegs *);
static void (*sample_cb)(const PlatRegs *);

static void regs_from(PlatRegs *r, const ucontext_t *uc) {
    const greg_t *g = uc->uc_mcontext.gregs;
    r->eip = g[REG_EIP]; r->esp = g[REG_ESP]; r->ebp = g[REG_EBP];
    r->eax = g[REG_EAX]; r->ebx = g[REG_EBX]; r->ecx = g[REG_ECX];
    r->edx = g[REG_EDX]; r->esi = g[REG_ESI]; r->edi = g[REG_EDI];
}

static void fault_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    PlatRegs r = {0};
    regs_from(&r, uc);
    r.fault_addr = (uint32_t)(uintptr_t)si->si_addr;
    r.code = sig;
    r.subcode = si->si_code;
    r.what = strsignal(sig);
    r.privileged = sig == SIGSEGV;   /* user mode reports privileged instructions as SIGSEGV */
    if (fault_cb(&r)) { uc->uc_mcontext.gregs[REG_EIP] = r.eip; return; }
    _exit(128 + sig);
}

static void sample_handler(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    PlatRegs r = {0};
    regs_from(&r, uc_);
    sample_cb(&r);
}

static void (*crash_hook)(const char *why);
void plat_set_crash_hook(void (*hook)(const char *why)) { crash_hook = hook; }

static void abort_handler(int sig) {
    static const char head[] = "\n=== abort() (signal 6) ===\n";
    fflush(stdout);   /* the lines printed before the abort first */
    if (write(1, head, sizeof head - 1) < 0) _exit(134);
    if (crash_hook) crash_hook("abort()");
    _exit(128 + sig);
}

void plat_thread_init(void) {
    stack_t ss = {.ss_sp = malloc(0x10000), .ss_size = 0x10000};
    sigaltstack(&ss, NULL);
}

void plat_faults_install(int (*fault)(PlatRegs *), void (*sample)(const PlatRegs *)) {
    fault_cb = fault;
    sample_cb = sample;
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
    sa.sa_sigaction = sample_handler;
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGABRT, abort_handler);
    plat_thread_init();
}

uintptr_t plat_thread_self(void) { return (uintptr_t)pthread_self(); }
void plat_thread_sample(uintptr_t thread) { pthread_kill((pthread_t)thread, SIGUSR1); }
void plat_park(void) { for (;;) pause(); }
/* Linux priorities are per thread: nice +10 for this thread and the lowest best-effort I/O class
 * level (IOPRIO_WHO_PROCESS = 1 with a thread id, IOPRIO_CLASS_BE = 2 << 13, level 7). */
void plat_thread_low_priority(void) {
    const pid_t tid = (pid_t)syscall(SYS_gettid);
    errno = 0;
    int nice_now = getpriority(PRIO_PROCESS, (id_t)tid);
    if (!errno) (void)setpriority(PRIO_PROCESS, (id_t)tid, nice_now + 10 > 19 ? 19 : nice_now + 10);
    (void)syscall(SYS_ioprio_set, 1, (int)tid, (2 << 13) | 7);
}

/* ---- files ---- */
ssize_t plat_pread(int fd, void *buf, size_t len, uint64_t off) { return pread(fd, buf, len, (off_t)off); }
ssize_t plat_pwrite(int fd, const void *buf, size_t len, uint64_t off) { return pwrite(fd, buf, len, (off_t)off); }
int plat_mkdir(const char *path) { return mkdir(path, 0755); }
int plat_open_dir(const char *path) { return open(path, O_RDONLY | O_DIRECTORY); }
int plat_disk_space(const char *path, uint64_t *total, uint64_t *avail) {
    struct statvfs vs;
    if (statvfs(path, &vs)) return -1;
    *total = (uint64_t)vs.f_blocks * vs.f_frsize;
    *avail = (uint64_t)vs.f_bavail * vs.f_frsize;
    return 0;
}
static struct timespec filetime_ts(uint64_t ft) {
    if (!ft) return (struct timespec){0, UTIME_OMIT};
    int64_t t = (int64_t)(ft - 116444736000000000ull);   /* 100 ns since 1970 */
    int64_t s = t / 10000000, r = t % 10000000;
    if (r < 0) { s--; r += 10000000; }
    return (struct timespec){(time_t)s, (long)r * 100};
}
int plat_set_file_times(const char *path, uint64_t access, uint64_t write) {
    struct timespec ts[2] = {filetime_ts(access), filetime_ts(write)};
    return utimensat(AT_FDCWD, path, ts, 0);
}
