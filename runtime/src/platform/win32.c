/* Windows implementation of src/platform.h: a 32-bit process under WOW64 (64-bit Windows),
 * built into xbrun-core.dll and started by the TheWarriors.exe loader (win32_loader.c), which
 * reserved the XBE image range (0x10000+) and the contiguous window (0x80000000+) first.
 *
 * FS is the TEB here and cannot be pointed at the KPCR (no LDT on 64-bit Windows), so the
 * title's fs: instructions are patched (fspatch.c) to read a per-thread KPCR pointer from a
 * TlsAlloc slot: TEB.TlsSlots[i] lives at fs:[0xE10 + 4 * i] in a 32-bit TEB. */
#define WIN32_LEAN_AND_MEAN
#include "../platform.h"
#include "outlog.h"
#include <windows.h>
#include <tlhelp32.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* UCRT export (api-ms-win-crt-runtime) that mingw-w64's headers do not declare. */
_invalid_parameter_handler __cdecl _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);

static uintptr_t exe_base;   /* the process EXE's base as loaded (0x10000 for the loader) */

#define TEB_TLS_SLOTS 0xE10   /* offsetof(TEB, TlsSlots), 32-bit TEB */

/* ---- process: log file, CRT error reporting ----
 * Output goes through outlog.c (force-included): one write to the console handle and one to
 * <name>.log next to the executable per printf, so nothing is buffered when the process dies. */
void plat_flush_output(void) {}

/* Writes a report line directly (after the buffered output), bypassing the CRT. */
static void diag(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    outlog_write(1, buf, (size_t)n);
}

static const char *module_of(uintptr_t addr, uintptr_t *base) {
    static char name[MAX_PATH];
    HMODULE m;
    *base = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m))
        return addr < 0x60000000u ? "guest" : "?";
    *base = (uintptr_t)m;
    DWORD n = GetModuleFileNameA(m, name, sizeof name);
    const char *slash = n ? strrchr(name, '\\') : NULL;
    return slash ? slash + 1 : n ? name : "?";
}

/* Stack values that look like return addresses into modules (who called the CRT). */
static void diag_stack(uintptr_t esp) {
    int shown = 0;
    for (int i = 0; i < 512 && shown < 12; i++) {
        uintptr_t a = esp + 4u * i;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        uintptr_t v = *(uintptr_t *)a, base;
        if (!VirtualQuery((void *)v, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        const char *mod = module_of(v, &base);
        diag("  [esp+%03x] %08x  %s+0x%x\n", (unsigned)(4 * i), (unsigned)v, mod, (unsigned)(v - base));
        shown++;
    }
}

static void invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)r;
    static volatile LONG reports;
    plat_flush_output();
    if (InterlockedIncrement(&reports) > 20) return;
    diag("\n=== CRT invalid parameter (thread %lu)%s%ls%s%ls%s%ls line %u ===\n", GetCurrentThreadId(),
         expr ? ": " : " (release CRT: no expression)", expr ? expr : L"", func ? " in " : "", func ? func : L"",
         file ? " at " : "", file ? file : L"", line);
    diag("caller candidates:\n");
    diag_stack((uintptr_t)__builtin_frame_address(0));
    /* Returning makes the CRT function fail with EINVAL/EBADF, like POSIX, instead of __fastfail. */
}

static void abort_handler(int sig) {
    plat_flush_output();
    diag("\n=== abort() (signal %d, thread %lu) ===\n", sig, GetCurrentThreadId());
    diag_stack((uintptr_t)__builtin_frame_address(0));
    _exit(134);
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    plat_flush_output();
    uintptr_t base;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    const char *mod = module_of(at, &base);
    diag("\n=== unhandled exception %08lx at %08x (%s+0x%x), thread %lu ===\n", ep->ExceptionRecord->ExceptionCode,
         (unsigned)at, mod, (unsigned)(at - base), GetCurrentThreadId());
    if (ep->ExceptionRecord->NumberParameters >= 2)
        diag("parameters %08x %08x\n", (unsigned)ep->ExceptionRecord->ExceptionInformation[0],
             (unsigned)ep->ExceptionRecord->ExceptionInformation[1]);
    diag_stack(ep->ContextRecord->Esp);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void setup_output(const char *log_name) {
    char path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n ? strrchr(path, '\\') : NULL;
    if (slash) slash[1] = 0; else path[0] = 0;
    strcat(path, log_name);
    strcat(path, ".log");
    /* Append when started by the loader, which wrote its own startup lines first. */
    int ok = outlog_open(path, GetEnvironmentVariableA("XBRUN_LOG_APPEND", NULL, 0) > 0) == 0;
    printf("[start] %s: output also in %s\n", log_name, ok ? path : "(log file not writable)");
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    printf("[start] stdout handle %p type %lu console %d\n", (void *)h, GetFileType(h), GetConsoleMode(h, &mode) ? 1 : 0);
}

/* Address-space summary for failures that smell of exhaustion (threads, DLL loads). */
void plat_report_memory(const char *why) {
    uintptr_t a = 0x10000, largest[4] = {0}, free_total = 0, reserved = 0, committed = 0;
    while (a < 0xFFFF0000u) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || !mi.RegionSize) break;
        uintptr_t n = mi.RegionSize;
        if (mi.State == MEM_FREE) {
            free_total += n;
            for (int i = 0; i < 4; i++)
                if (n > largest[i]) { for (int j = 3; j > i; j--) largest[j] = largest[j - 1]; largest[i] = n; break; }
        } else if (mi.State == MEM_RESERVE) reserved += n;
        else committed += n;
        uintptr_t next = (uintptr_t)mi.BaseAddress + n;
        if (next <= a) break;
        a = next;
    }
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    printf("[mem] %s: last error %lu; address space: free %u MB (largest %u, %u, %u, %u MB), reserved %u MB, committed %u MB; "
           "commit available %u MB of %u MB, virtual available %u MB\n", why, GetLastError(),
           (unsigned)(free_total >> 20), (unsigned)(largest[0] >> 20), (unsigned)(largest[1] >> 20), (unsigned)(largest[2] >> 20),
           (unsigned)(largest[3] >> 20), (unsigned)(reserved >> 20), (unsigned)(committed >> 20), (unsigned)(ms.ullAvailPageFile >> 20),
           (unsigned)(ms.ullTotalPageFile >> 20), (unsigned)(ms.ullAvailVirtual >> 20));
}

/* The XBE is mapped over the loader EXE, so the PE header at the process image base becomes
 * the XBE header. Windows (including the 64-bit WOW64 side, which ignores PEB32 changes)
 * reads the process image header whenever it creates a thread stack (CreateThread then fails
 * with ERROR_BAD_EXE_FORMAT) and on DLL loads. Same fix as Cxbx-Reloaded (CxbxKrnl.cpp,
 * RestoreExeImageHeader): keep "MZ" at 0x10000 (over the XBE magic) and point e_lfanew (in
 * the unused XBE signature bytes) at a copy of the loader's NT headers. The copy sits at
 * EXE_HDR_COPY, inside the loader's placeholder but past the XBE image, because
 * RtlImageNtHeader rejects e_lfanew of 256 MB or more. */
#define EXE_HDR_COPY 0x700000u
static uint8_t exe_headers[0x1000];   /* the loader's original headers */
static uint32_t exe_headers_size;

static void save_process_image(void) {
    exe_base = (uintptr_t)GetModuleHandleA(NULL);
    if (exe_base != 0x10000) return;   /* not started by the loader (e.g. a test exe) */
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)exe_base;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(exe_base + dos->e_lfanew);
    exe_headers_size = nt->OptionalHeader.SizeOfHeaders;
    if (exe_headers_size > sizeof exe_headers) exe_headers_size = sizeof exe_headers;
    memcpy(exe_headers, dos, exe_headers_size);
}

int plat_image_mapped(uint32_t base, uint32_t end) {
    if (exe_base != 0x10000 || base != 0x10000 || end > EXE_HDR_COPY || !exe_headers_size) {
        printf("[start] loader header not restored (exe base %08x, XBE %08x-%08x)\n", (unsigned)exe_base, base, end);
        return 0;
    }
    uint8_t *copy = plat_map_fixed(EXE_HDR_COPY, 0x1000);
    if (!copy) { printf("[start] cannot claim %08x for the EXE header copy\n", EXE_HDR_COPY); return 0; }
    LONG lfanew = ((const IMAGE_DOS_HEADER *)exe_headers)->e_lfanew;
    memcpy(copy, exe_headers + lfanew, exe_headers_size - (uint32_t)lfanew);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe_base;
    dos->e_magic = IMAGE_DOS_SIGNATURE;                          /* 'MZ' over 'XB' of "XBEH" */
    dos->e_lfanew = (LONG)(EXE_HDR_COPY - (uint32_t)exe_base);   /* inside the XBE signature */
    printf("[start] process image header kept valid: MZ at %08x, NT headers copy at %08x\n", (unsigned)exe_base, EXE_HDR_COPY);
    return 1;
}

/* --win-thread-probe: can Windows create a thread right now? */
static DWORD WINAPI probe_proc(void *arg) { (void)arg; return 0; }
void plat_thread_probe(const char *phase) {
    HANDLE h = CreateThread(NULL, 0, probe_proc, NULL, 0, NULL);
    DWORD err = h ? 0 : GetLastError();
    if (h) { WaitForSingleObject(h, 5000); CloseHandle(h); }
    printf("[probe] %s: CreateThread %s (error %lu)\n", phase, h ? "ok" : "FAILED", err);
}

static void mark_own_reservation(uintptr_t base);
void plat_init(const char *log_name) {
    mark_own_reservation(0x80000000u);   /* the contiguous window, reserved by win32_loader.c */
    _set_invalid_parameter_handler(invalid_parameter);
    _set_thread_local_invalid_parameter_handler(invalid_parameter);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, abort_handler);
    SetUnhandledExceptionFilter(unhandled_filter);
    setup_output(log_name);
    save_process_image();
    /* Sleep/WaitForSingleObject granularity 1 ms instead of 15.6 ms (pacing, mixer, waits). */
    HMODULE winmm = LoadLibraryA("winmm.dll");
    if (winmm) {
        UINT(WINAPI * begin)(UINT) = (UINT(WINAPI *)(UINT))(void *)GetProcAddress(winmm, "timeBeginPeriod");
        printf("[start] timer resolution 1 ms: %s\n", begin && begin(1) == 0 ? "ok" : "not available");
    }
}

/* ---- timing ---- */
static void mono_now(struct timespec *t) { clock_gettime(CLOCK_MONOTONIC, t); }
static int64_t ns_until(const struct timespec *deadline) {
    struct timespec now;
    mono_now(&now);
    return (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000 + (deadline->tv_nsec - now.tv_nsec);
}

/* High-resolution waitable timer (Windows 10 1803+; ~0.5 ms precision without changing the
 * system timer), one per thread; Sleep() with timeBeginPeriod(1) as the fallback. The last
 * half millisecond is spun so 60 Hz pacing does not drift. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static __thread HANDLE sleep_timer;
static __thread int sleep_timer_tried;

void plat_sleep_until(const struct timespec *deadline) {
    int64_t ns;
    while ((ns = ns_until(deadline)) > 0) {
        if (ns > 700000) {
            if (!sleep_timer_tried) {
                sleep_timer_tried = 1;
                sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            }
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((ns - 500000) / 100);   /* relative, 100 ns units */
            if (sleep_timer && SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(sleep_timer, INFINITE);
            else
                Sleep((DWORD)((ns - 500000) / 1000000));
        } else {
            SwitchToThread();
        }
    }
}

int plat_cond_init_monotonic(pthread_cond_t *c) { return pthread_cond_init(c, NULL); }

/* winpthreads condition variables only take CLOCK_REALTIME deadlines: convert. */
int plat_cond_timedwait_mono(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline) {
    int64_t ns = ns_until(deadline);
    if (ns < 0) ns = 0;
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    rt.tv_sec += ns / 1000000000;
    rt.tv_nsec += ns % 1000000000;
    if (rt.tv_nsec >= 1000000000) { rt.tv_sec++; rt.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(c, m, &rt);
}

/* ---- memory ----
 * A range is free for plat_map_fixed when every part is MEM_FREE, a MEM_RESERVE of our own (the
 * loader's contiguous window, or a reservation plat_map_fixed made, with pages plat_unmap
 * decommitted), or a page of the loader EXE image that was not claimed yet (claimed pages are made
 * PAGE_EXECUTE_READWRITE). Reservations of other code are never used: the AMD Vulkan driver
 * (amdvlk32.dll, loaded by the Vulkan loader to enumerate devices) reserves 32 MB ranges and
 * commits and releases pieces of them later, which overwrote or freed guest pages committed there
 * while the mapped bitmap still reported them. */
static int image_page(const MEMORY_BASIC_INFORMATION *mi) {
    return mi->Type == MEM_IMAGE && mi->AllocationBase == (void *)exe_base;
}
static int claimed_image(const MEMORY_BASIC_INFORMATION *mi) {
    return (mi->Protect & (PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* Our reservations (plat_map_fixed's and the loader's contiguous window), one bit per 64 KB
 * allocation granule of the reservation's AllocationBase; XBRUN_MEMTRACE labels the others. */
static uint8_t own_granules[1u << 13];
static void mark_own_reservation(uintptr_t base) {
    __atomic_or_fetch(&own_granules[base >> 19], (uint8_t)(1u << (base >> 16 & 7)), __ATOMIC_RELEASE);
}
static int own_reservation(const MEMORY_BASIC_INFORMATION *mi) {
    uintptr_t b = (uintptr_t)mi->AllocationBase;
    return (__atomic_load_n(&own_granules[b >> 19], __ATOMIC_ACQUIRE) >> (b >> 16 & 7)) & 1;
}

static int range_available(uintptr_t a, uintptr_t end) {
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) return 0;
        int ok = mi.State == MEM_FREE || (mi.State == MEM_RESERVE && own_reservation(&mi)) ||
                 (image_page(&mi) && !claimed_image(&mi));
        if (!ok) return 0;
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

/* Pages committed by plat_map_fixed, one bit per 4 KB page of the 32-bit space, so plat_is_mapped
 * answers for guest memory without VirtualQuery: VirtualQuery computes RegionSize by walking the
 * region's pages, about 2.6 us per resident MB (168 us per call on the touched 128 MB contiguous
 * window), and the draw recording checks a dozen guest ranges per draw. Bits are set after the
 * commit and cleared before the decommit; guest memory is only freed through plat_unmap. */
static uint8_t mapped_pages[1u << 17];
static void mark_pages(uintptr_t a, uintptr_t end, int on) {
    for (a >>= 12, end = (end + 0xFFF) >> 12; a < end; a++) {
        if (on) __atomic_or_fetch(&mapped_pages[a >> 3], (uint8_t)(1u << (a & 7)), __ATOMIC_RELEASE);
        else __atomic_and_fetch(&mapped_pages[a >> 3], (uint8_t)~(1u << (a & 7)), __ATOMIC_RELEASE);
    }
}

/* XBRUN_MEMTRACE=LO-HI (hex) or "all": logs the regions plat_map_fixed and plat_unmap meet inside
 * that range and the stack reservation of each host thread that runs guest code. Quiet by default. */
static int memtrace_mode = -1;
static uintptr_t memtrace_lo, memtrace_hi;
static int memtrace(uintptr_t a, uintptr_t end) {
    if (memtrace_mode < 0) {
        const char *e = getenv("XBRUN_MEMTRACE");
        unsigned lo, hi;
        if (e && !strcmp(e, "all")) memtrace_lo = 0, memtrace_hi = UINTPTR_MAX, memtrace_mode = 1;
        else if (e && sscanf(e, "%x-%x", &lo, &hi) == 2) memtrace_lo = lo, memtrace_hi = hi, memtrace_mode = 1;
        else memtrace_mode = 0;
    }
    return memtrace_mode && a < memtrace_hi && end > memtrace_lo;
}
static const char *mem_state(DWORD s) { return s == MEM_COMMIT ? "commit" : s == MEM_RESERVE ? "reserve" : s == MEM_FREE ? "free" : "?"; }
static const char *mem_type(DWORD t) { return t == MEM_PRIVATE ? "private" : t == MEM_MAPPED ? "mapped" : t == MEM_IMAGE ? "image" : "-"; }
/* The thread whose stack reservation starts at base (TEB DeallocationStack), or 0. */
static DWORD stack_owner(uintptr_t base) {
    typedef LONG(NTAPI * QueryThread)(HANDLE, int, void *, ULONG, ULONG *);
    static QueryThread query;
    if (!query) query = (QueryThread)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    DWORD owner = 0;
    for (BOOL ok = Thread32First(snap, &te); ok && !owner; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        struct { LONG status; uint8_t *teb; uintptr_t pid, tid, affinity; LONG prio, base_prio; } tbi;   /* THREAD_BASIC_INFORMATION */
        if (h && query && !query(h, 0, &tbi, sizeof tbi, NULL) && *(uintptr_t *)(tbi.teb + 0xE0C) == base) owner = te.th32ThreadID;
        if (h) CloseHandle(h);
    }
    CloseHandle(snap);
    return owner;
}
static void memtrace_piece(const char *op, const MEMORY_BASIC_INFORMATION *mi, uintptr_t a, uintptr_t end) {
    if (!memtrace(a, end)) return;
    int foreign = mi->State != MEM_FREE && !image_page(mi) && !own_reservation(mi);
    DWORD owner = foreign ? stack_owner((uintptr_t)mi->AllocationBase) : 0;
    printf("[memtrace] %s %08x-%08x thread %lu: %s %s protect %#lx allocation %08x%s", op, (unsigned)a, (unsigned)end,
           GetCurrentThreadId(), mem_state(mi->State), mem_type(mi->Type), mi->Protect, (unsigned)(uintptr_t)mi->AllocationBase,
           foreign ? " (FOREIGN)" : mi->State == MEM_FREE ? "" : " (ours)");
    if (owner) {
        typedef HRESULT(WINAPI * GetDescription)(HANDLE, PWSTR *);
        GetDescription get = (GetDescription)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, owner);
        PWSTR name = NULL;
        printf(" = stack of host thread %lu", owner);
        if (h && get && SUCCEEDED(get(h, &name)) && name && *name) printf(" \"%ls\"", name);
        if (name) LocalFree(name);
        if (h) CloseHandle(h);
    }
    if (foreign) {   /* the whole reservation: every region with the same AllocationBase */
        uintptr_t r = (uintptr_t)mi->AllocationBase, q = r;
        MEMORY_BASIC_INFORMATION m;
        while (VirtualQuery((void *)q, &m, sizeof m) && m.AllocationBase == mi->AllocationBase) q = (uintptr_t)m.BaseAddress + m.RegionSize;
        printf(", reservation %08x-%08x", (unsigned)r, (unsigned)q);
    }
    printf("\n");
}

void *plat_map_fixed(uint32_t addr, size_t size) {
    uintptr_t a = addr, end = (uintptr_t)addr + size;
    if (!size || end < a || !range_available(a, end)) return NULL;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        VirtualQuery((void *)a, &mi, sizeof mi);
        uintptr_t piece_end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (piece_end > end) piece_end = end;
        size_t n = piece_end - a;
        memtrace_piece("map", &mi, a, piece_end);
        if (mi.State == MEM_FREE) {
            /* Reservations start on 64 KB boundaries: reserve from there, commit only [a, end). */
            uintptr_t r = a & ~(uintptr_t)0xFFFF;
            size_t rn = n + (a - r);
            if (r < a && !range_available(r, a)) return NULL;
            if (!VirtualAlloc((void *)r, rn, MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return NULL;
            mark_own_reservation(r);
            if (!VirtualAlloc((void *)a, n, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return NULL;
        } else if (mi.State == MEM_RESERVE) {
            if (!VirtualAlloc((void *)a, n, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return NULL;
        } else {   /* loader image placeholder */
            DWORD old;
            if (!VirtualProtect((void *)a, n, PAGE_EXECUTE_READWRITE, &old)) return NULL;
            memset((void *)a, 0, n);
        }
        a = piece_end;
    }
    mark_pages(addr, end, 1);
    if (memtrace(addr, end)) printf("[memtrace] map %08x-%08x thread %lu: ok\n", addr, (unsigned)end, GetCurrentThreadId());
    return (void *)(uintptr_t)addr;
}

void plat_unmap(void *p, size_t size) {
    uintptr_t a = (uintptr_t)p, end = a + size;
    mark_pages(a, end, 0);
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) return;
        uintptr_t piece_end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (piece_end > end) piece_end = end;
        memtrace_piece("unmap", &mi, a, piece_end);
        if (image_page(&mi)) {
            DWORD old;
            memset((void *)a, 0, piece_end - a);
            VirtualProtect((void *)a, piece_end - a, PAGE_READWRITE, &old);
        } else if (mi.State == MEM_COMMIT) {
            VirtualFree((void *)a, piece_end - a, MEM_DECOMMIT);   /* the reservation stays reusable */
        }
        a = piece_end;
    }
}

void *plat_alloc_exec(size_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE); }

int plat_is_mapped(uint32_t addr) {
    if (__atomic_load_n(&mapped_pages[addr >> 15], __ATOMIC_ACQUIRE) & (1u << (addr >> 12 & 7))) return 1;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi)) return 0;
    return mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

void plat_describe_addr(uint32_t addr, char *buf, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    int bit = (__atomic_load_n(&mapped_pages[addr >> 15], __ATOMIC_ACQUIRE) >> (addr >> 12 & 7)) & 1;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi)) { snprintf(buf, n, "VirtualQuery failed; mapped bit %d", bit); return; }
    snprintf(buf, n, "%s %s protect %#lx, allocation %08x%s, region %08x size %#lx; mapped bit %d", mem_state(mi.State),
             mem_type(mi.Type), mi.Protect, (unsigned)(uintptr_t)mi.AllocationBase,
             mi.State == MEM_FREE ? "" : own_reservation(&mi) ? " (ours)" : " (foreign)", (unsigned)(uintptr_t)mi.BaseAddress,
             (unsigned long)mi.RegionSize, bit);
}

uint32_t plat_alloc_granularity(void) { return 0x10000; }

/* RelationProcessorCore entries whose logical processors meet the process affinity mask. */
int plat_physical_cores(void) {
    static int cached;
    if (cached) return cached;
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask)) proc_mask = ~(DWORD_PTR)0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[256];
    DWORD bytes = sizeof info;
    int n = 0;
    if (GetLogicalProcessorInformation(info, &bytes))
        for (DWORD i = 0; i < bytes / sizeof *info; i++)
            n += info[i].Relationship == RelationProcessorCore && (info[i].ProcessorMask & proc_mask);
    if (n <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
    }
    return cached = n;
}

/* ---- dynamic libraries ---- */
static char dl_err[256];
void *plat_dlopen(const char *name) {
    HMODULE m = LoadLibraryA(name);
    if (!m) {
        DWORD e = GetLastError();
        int n = snprintf(dl_err, sizeof dl_err, "%s: error %lu: ", name, (unsigned long)e);
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, dl_err + n,
                       (DWORD)(sizeof dl_err - n), NULL);
        dl_err[strcspn(dl_err, "\r\n")] = 0;
    }
    if (!m) plat_report_memory("LoadLibrary failed");
    return m;
}
void *plat_dlsym(void *lib, const char *sym) { return (void *)GetProcAddress((HMODULE)lib, sym); }
const char *plat_dlerror(void) { return dl_err[0] ? dl_err : "unknown error"; }
const char *plat_program_dir(void) {
    static char dir[MAX_PATH + 1];
    DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    dir[n < MAX_PATH ? n : 0] = 0;
    char *slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0; else dir[0] = 0;
    return dir;
}

/* ---- KPCR ---- */
static DWORD kpcr_slot = TLS_OUT_OF_INDEXES;

void plat_kpcr_init(void) {
    kpcr_slot = TlsAlloc();
    if (kpcr_slot == TLS_OUT_OF_INDEXES || kpcr_slot >= 64) {
        fprintf(stderr, "no TEB TLS slot below 64 for the KPCR pointer (got %lu)\n", (unsigned long)kpcr_slot);
        exit(1);
    }
    /* The patched code reads fs:[0xE10 + 4 * slot] directly: check the TEB layout once. */
    TlsSetValue(kpcr_slot, (void *)0x4B504352);
    uint32_t v;
    __asm__ volatile("movl %%fs:(%1), %0" : "=r"(v) : "r"(TEB_TLS_SLOTS + 4 * kpcr_slot));
    TlsSetValue(kpcr_slot, NULL);
    if (v != 0x4B504352) { fprintf(stderr, "unexpected TEB layout: TLS slot %lu is not at fs:[%#lx]\n", (unsigned long)kpcr_slot, (unsigned long)(TEB_TLS_SLOTS + 4 * kpcr_slot)); exit(1); }
}

void plat_kpcr_bind(uint8_t *kpcr, uint32_t stack_base, uint32_t stack_limit, int guest_stack) {
    TlsSetValue(kpcr_slot, kpcr);
    if (guest_stack) {
        /* Host code called from guest code runs on the guest stack: point the TEB stack range at
         * it so stack probes (_chkstk) and SEH frame validation in system and driver DLLs accept
         * it, as SwitchToFiber does for fiber stacks. */
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        tib->StackBase = (void *)(uintptr_t)stack_base;
        tib->StackLimit = (void *)(uintptr_t)stack_limit;
    }
}

int plat_fs_patch_required(uint32_t *disp) {
    *disp = TEB_TLS_SLOTS + 4 * kpcr_slot;
    return 1;
}

uint32_t plat_min_guest_stack(void) { return 0x100000; }   /* host DLL code is stack hungry */

/* ---- faults ---- */
static int (*fault_cb)(PlatRegs *);
static void (*sample_cb)(const PlatRegs *);
static HMODULE core_module;

static void regs_from(PlatRegs *r, const CONTEXT *c) {
    r->eip = c->Eip; r->esp = c->Esp; r->ebp = c->Ebp;
    r->eax = c->Eax; r->ebx = c->Ebx; r->ecx = c->Ecx;
    r->edx = c->Edx; r->esi = c->Esi; r->edi = c->Edi;
}

static const char *exception_name(DWORD code, int *exit_code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: *exit_code = 139; return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: *exit_code = 135; return "in-page error";
    case EXCEPTION_PRIV_INSTRUCTION: *exit_code = 139; return "privileged instruction";
    case EXCEPTION_ILLEGAL_INSTRUCTION: *exit_code = 132; return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: *exit_code = 136; return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: *exit_code = 136; return "integer overflow";
    case EXCEPTION_BREAKPOINT: *exit_code = 133; return "breakpoint";
    case EXCEPTION_SINGLE_STEP: *exit_code = 133; return "single step";
    case EXCEPTION_STACK_OVERFLOW: *exit_code = 139; return "stack overflow";
    default: return NULL;
    }
}

/* Guest code (the XBE image over the loader EXE, the guest arena, generated stubs) and this
 * DLL are ours; exceptions in other modules (system or driver DLLs) may be handled there. */
static int ours(uint32_t eip) {
    HMODULE m;
    if (eip >= 0x10000 && eip < 0x01000000) return 1;   /* XBE image (fast path: no loader lock) */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)eip, &m))
        return 1;
    return m == core_module || m == (HMODULE)exe_base;
}

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    int exit_code = 1;
    const char *what = exception_name(er->ExceptionCode, &exit_code);
    if (!what || !ours(ep->ContextRecord->Eip)) return EXCEPTION_CONTINUE_SEARCH;
    PlatRegs r = {0};
    regs_from(&r, ep->ContextRecord);
    r.code = (int)er->ExceptionCode;
    r.subcode = er->NumberParameters ? (int)er->ExceptionInformation[0] : 0;   /* AV: 0 read, 1 write, 8 execute */
    r.fault_addr = er->NumberParameters > 1 ? (uint32_t)er->ExceptionInformation[1] : (uint32_t)(uintptr_t)er->ExceptionAddress;
    r.what = what;
    r.privileged = er->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION;
    if (fault_cb(&r)) { ep->ContextRecord->Eip = r.eip; return EXCEPTION_CONTINUE_EXECUTION; }
    plat_flush_output();
    _exit(exit_code);
}

void plat_thread_init(void) {
    /* TEB (x86): NT_TIB.StackBase at 0x04, DeallocationStack (the reservation base) at 0xE0C. */
    uint8_t *teb = (uint8_t *)NtCurrentTeb();
    uintptr_t base = *(uintptr_t *)(teb + 4), dealloc = *(uintptr_t *)(teb + 0xE0C);
    if (memtrace(dealloc, base))
        printf("[memtrace] host thread %lu stack reservation %08x-%08x\n", GetCurrentThreadId(), (unsigned)dealloc, (unsigned)base);
}

void plat_faults_install(int (*fault)(PlatRegs *), void (*sample)(const PlatRegs *)) {
    fault_cb = fault;
    sample_cb = sample;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)vectored_handler, &core_module);
    AddVectoredExceptionHandler(1, vectored_handler);
}

uintptr_t plat_thread_self(void) {
    HANDLE h = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return (uintptr_t)h;
}

/* Watchdog sample: suspend, read the registers, print from the calling thread, resume. */
void plat_thread_sample(uintptr_t thread) {
    HANDLE h = (HANDLE)thread;
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(h, &c)) {
        PlatRegs r = {0};
        regs_from(&r, &c);
        sample_cb(&r);
    }
    ResumeThread(h);
}

void plat_park(void) { for (;;) Sleep(INFINITE); }

/* ---- files ----
 * Positional I/O through the OS handle; ReadFile with an OVERLAPPED offset moves the file
 * pointer of a synchronous handle, so it is restored to keep pread semantics. */
static ssize_t positional(int fd, void *buf, size_t len, uint64_t off, int write) {
    if (fd < 0) { errno = EBADF; return -1; }   /* _get_osfhandle(-1) is an invalid parameter */
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    LARGE_INTEGER zero = {0}, cur;
    SetFilePointerEx(h, zero, &cur, FILE_CURRENT);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD n = 0;
    BOOL ok = write ? WriteFile(h, buf, (DWORD)len, &n, &ov) : ReadFile(h, buf, (DWORD)len, &n, &ov);
    DWORD err = ok ? 0 : GetLastError();
    SetFilePointerEx(h, cur, NULL, FILE_BEGIN);
    if (!ok && err != ERROR_HANDLE_EOF) { errno = EIO; return -1; }
    return (ssize_t)n;
}
ssize_t plat_pread(int fd, void *buf, size_t len, uint64_t off) { return positional(fd, buf, len, off, 0); }
ssize_t plat_pwrite(int fd, const void *buf, size_t len, uint64_t off) { return positional(fd, (void *)buf, len, off, 1); }

int plat_mkdir(const char *path) { return _mkdir(path); }

/* The CRT cannot open directories: hand out an fd on NUL (kfile.c stats directories by path). */
int plat_open_dir(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { errno = ENOTDIR; return -1; }
    return _open("NUL", _O_RDONLY | _O_BINARY);
}

int plat_disk_space(const char *path, uint64_t *total, uint64_t *avail) {
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    DWORD attr = GetFileAttributesA(dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {   /* a file: its directory */
        char *slash = strrchr(dir, '/'), *bslash = strrchr(dir, '\\');
        if (bslash > slash) slash = bslash;
        if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    }
    ULARGE_INTEGER a, t;
    if (!GetDiskFreeSpaceExA(dir, &a, &t, NULL)) return -1;
    *total = t.QuadPart;
    *avail = a.QuadPart;
    return 0;
}
