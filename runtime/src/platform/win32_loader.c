/* TheWarriors.exe (xbrun.exe in earlier builds): the Windows loader. Linked without the CRT
 * at the fixed base 0x10000 (no ASLR, no relocations, large address aware: see the windows
 * target in the Makefile), so the image itself reserves the XBE's address range before Windows
 * places anything else there (process parameters and the environment otherwise take the
 * lowest free addresses). It reserves the contiguous memory window, loads xbrun-core.dll (the
 * runtime) and never returns: the runtime maps the XBE over this image, code included. Same
 * approach as Cxbx-Reloaded's cxbxr-ldr. Startup lines go to stdout and to a fresh xbrun.log
 * next to the exe (the runtime appends). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Covers the XBE image (0x10000-0x5d5000 for this title) plus slack: lands in .bss right after
 * the loader's code, so it costs address space, not file size. */
__attribute__((used)) unsigned char xbrun_low_reservation[0x700000];

static HANDLE log_file = INVALID_HANDLE_VALUE;

static void out(const char *msg) {
    DWORD n;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, msg, lstrlenA(msg), &n, NULL);
    if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, msg, lstrlenA(msg), &n, NULL);
}

/* kernel32 only (no user32 wsprintf): 8 hex digits */
static char *hex8(char *p, unsigned long v) {
    for (int i = 7; i >= 0; i--) p[7 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
    p[8] = 0;
    return p;
}

static void fail(const char *msg) {
    char buf[16];
    unsigned long err = GetLastError();
    out(msg);
    out(" (error 0x");
    out(hex8(buf, err));
    out(")\r\n");
    ExitProcess(1);
}

static void open_log(void) {
    char path[MAX_PATH + 16];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    while (n && path[n - 1] != '\\') n--;
    lstrcpyA(path + n, "xbrun.log");
    log_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void __stdcall xbrun_loader_start(void) {
    ((volatile unsigned char *)xbrun_low_reservation)[0] = 0;
    open_log();
    char buf[16];
    out("[loader] exe at ");
    out(hex8(buf, (unsigned long)(ULONG_PTR)GetModuleHandleA(NULL)));
    out(", low reservation ");
    out(hex8(buf, (unsigned long)(ULONG_PTR)xbrun_low_reservation));
    out("-");
    out(hex8(buf, (unsigned long)(ULONG_PTR)(xbrun_low_reservation + sizeof xbrun_low_reservation)));
    out("\r\n");
    /* "Physical" memory window (CONTIG_BASE/CONTIG_SIZE in mem.h); needs the LAA flag. */
    if (!VirtualAlloc((void *)0x80000000u, 0x08000000u, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        fail("[loader] cannot reserve 0x80000000-0x88000000 (is the exe large-address-aware?)");
    out("[loader] reserved 80000000-88000000; loading xbrun-core.dll\r\n");
    SetEnvironmentVariableA("XBRUN_LOG_APPEND", "1");
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);   /* the runtime reopens it to append */
    log_file = INVALID_HANDLE_VALUE;
    HMODULE core = LoadLibraryA("xbrun-core.dll");
    if (!core) fail("[loader] cannot load xbrun-core.dll (it must be next to the exe)");
    int (*entry)(void) = (int (*)(void))(void *)GetProcAddress(core, "xbrun_dll_main");
    if (!entry) fail("[loader] xbrun-core.dll has no xbrun_dll_main");
    ExitProcess((UINT)entry());
}
