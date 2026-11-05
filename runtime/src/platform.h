/* Host platform layer: the few OS services the runtime needs that differ between Linux (POSIX,
 * src/platform/posix.c) and Windows (Win32 under WOW64, src/platform/win32.c). Everything else
 * (pthreads, clock_gettime, stdio, dirent) is common: zig's x86-windows-gnu target links
 * mingw-w64's winpthreads. Modules include this header instead of OS headers for these services. */
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#define PLAT_ASM_SYM(x) "_" #x   /* C symbol name in top-level asm (i386 COFF prefix) */
#define PLAT_LIB_SDL2 "SDL2.dll"
#define PLAT_LIB_VULKAN "vulkan-1.dll"
#define PLAT_O_BINARY 0x8000   /* _O_BINARY: no CRLF translation */
#else
#define PLAT_ASM_SYM(x) #x
#define PLAT_LIB_SDL2 "libSDL2-2.0.so.0"
#define PLAT_LIB_VULKAN "libvulkan.so.1"
#define PLAT_O_BINARY 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Process-wide setup before anything else: stdio buffering, and on Windows the 1 ms timer
 * resolution, CRT error handlers (invalid parameter, abort, unhandled exceptions) and a copy
 * of all output in <log_name>.log next to the executable. */
void plat_init(const char *log_name);
/* Called right after the XBE image is mapped at base..end. Windows: restores a valid PE header
 * at the process image base (MZ + e_lfanew over unused XBE header bytes) and returns 1, after
 * which the XBE magic at base no longer reads "XBEH". Linux: 0. */
int plat_image_mapped(uint32_t base, uint32_t end);
/* --win-thread-probe: reports whether a host thread can be created at this point. */
void plat_thread_probe(const char *phase);
/* Prints an address-space / commit summary (Windows; no-op on Linux) when an allocation, a
 * thread or a library load failed. */
void plat_report_memory(const char *why);
/* Makes everything printed so far reach the console and the log (before exiting on errors). */
void plat_flush_output(void);

/* ---- timing: deadlines are CLOCK_MONOTONIC (winpthreads waits only on CLOCK_REALTIME) ---- */
void plat_sleep_until(const struct timespec *deadline);
int plat_cond_init_monotonic(pthread_cond_t *c);
/* Returns 0 or ETIMEDOUT like pthread_cond_timedwait. */
int plat_cond_timedwait_mono(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline);

/* ---- memory ---- */
/* Zeroed RWX pages at exactly addr; NULL if any part is taken. */
void *plat_map_fixed(uint32_t addr, size_t size);
void plat_unmap(void *p, size_t size);
/* Zeroed RWX pages anywhere (host-generated stubs). NULL on failure. */
void *plat_alloc_exec(size_t size);
int plat_is_mapped(uint32_t addr);
/* One line about the host mapping at addr, for fault reports. */
void plat_describe_addr(uint32_t addr, char *buf, size_t n);
/* Placement granularity of plat_map_fixed (4 KB on Linux, 64 KB on Windows). */
uint32_t plat_alloc_granularity(void);

/* ---- dynamic libraries ---- */
void *plat_dlopen(const char *name);
void *plat_dlsym(void *lib, const char *sym);
const char *plat_dlerror(void);
/* The running program's folder with a trailing separator ("" when unknown). */
const char *plat_program_dir(void);

/* ---- guest thread environment (KPCR through %fs) ---- */
void plat_kpcr_init(void);
/* Makes kpcr the calling thread's KPCR. guest_stack: the thread runs guest code and host
 * callouts on [stack_limit, stack_base) (Windows points the TEB stack range there). */
void plat_kpcr_bind(uint8_t *kpcr, uint32_t stack_base, uint32_t stack_limit, int guest_stack);
/* Windows: guest fs: instructions must be patched (fspatch.c); *disp = fs offset of the
 * per-thread KPCR pointer. Linux: 0 (%fs is the KPCR); *disp = 0x1C (KPCR.SelfPcr) for
 * the optional --patch-fs mode. */
int plat_fs_patch_required(uint32_t *disp);
uint32_t plat_min_guest_stack(void);

/* ---- threads, faults ---- */
/* Physical cores (not SMT siblings) the process may run on; at least 1. */
int plat_physical_cores(void);
typedef struct {
    uint32_t eip, esp, ebp, eax, ebx, ecx, edx, esi, edi;
    uint32_t fault_addr;
    int code;                /* signal number (Linux) or exception code (Windows) */
    int subcode;             /* si_code (Linux) */
    const char *what;        /* human-readable kind */
    int privileged;          /* trap on a privileged instruction (WBINVD/INVD candidates) */
} PlatRegs;
/* fault: returns 1 to resume at regs->eip (it may advance it), otherwise it must not return.
 * sample: watchdog sampling of another thread, prints and returns. */
void plat_faults_install(int (*fault)(PlatRegs *regs), void (*sample)(const PlatRegs *regs));
void plat_thread_init(void);          /* per host thread (Linux: alternate signal stack) */
uintptr_t plat_thread_self(void);     /* handle usable by plat_thread_sample */
void plat_thread_sample(uintptr_t thread);
void plat_park(void);                 /* blocks the calling thread forever */

/* ---- files ---- */
ssize_t plat_pread(int fd, void *buf, size_t len, uint64_t off);
ssize_t plat_pwrite(int fd, const void *buf, size_t len, uint64_t off);
int plat_mkdir(const char *path);
int plat_open_dir(const char *path);  /* an fd that stands for a directory handle */
int plat_disk_space(const char *path, uint64_t *total, uint64_t *avail);

#ifdef __cplusplus
}
#endif
#endif
