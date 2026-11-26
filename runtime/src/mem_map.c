/* Address space by owner (mem_map.h). */
#include "mem_map.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define MB(x) ((unsigned)((x) >> 20))
#ifdef _WIN32
void mem_map_report(const char *why, int guest_ranges) {
    unsigned long long cat[7] = {0}, largest = 0, free_total = 0;   /* guest, images-driver, images-other, mapped, private, reserved, free */
    MEMORY_BASIC_INFORMATION mi;
    for (uintptr_t a = 0x10000; a < 0xFFFF0000u && VirtualQuery((void *)a, &mi, sizeof mi); a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
        const uintptr_t b = (uintptr_t)mi.BaseAddress, n = mi.RegionSize;
        const int guest = guest_ranges && ((b >= 0x10000 && b < 0x5d5000) || (b >= 0x10000000 && b < 0x70000000 && mi.Type == MEM_PRIVATE) ||
                          (b >= 0x80000000u && b < 0x88000000u));
        if (mi.State == MEM_FREE) { free_total += n; if (n > largest) largest = n; continue; }
        if (guest) cat[0] += n;
        else if (mi.Type == MEM_IMAGE) {
            char name[MAX_PATH] = "";
            GetModuleFileNameA((HMODULE)mi.AllocationBase, name, sizeof name);
            _strlwr(name);
            cat[strstr(name, "nv") || strstr(name, "amd") || strstr(name, "igvk") || strstr(name, "igc") || strstr(name, "vulkan") ? 1 : 2] += n;
        } else if (mi.Type == MEM_MAPPED) cat[3] += n;
        else if (mi.State == MEM_COMMIT) cat[4] += n;
        else cat[5] += n;
    }
    printf("[mem-map] %s: guest %u MB, driver images %u MB, other images %u MB, mapped %u MB, private %u MB, reserved %u MB; "
           "free %u MB, largest hole %u MB\n", why, MB(cat[0]), MB(cat[1]), MB(cat[2]), MB(cat[3]), MB(cat[4]), MB(cat[5]),
           MB(free_total), MB(largest));
}
#else
/* /proc/self/maps below 4 GB by owner: guest (the fixed XBE, arena and contiguous ranges), the
 * GPU driver's libraries, other libraries, device mappings (/dev/: memory the driver maps into
 * the process, e.g. persistently mapped buffers), the brk heap, other anonymous host memory
 * (malloc arenas, thread stacks, driver heaps: indistinguishable here), files (packs, caches)
 * and the rest. */
enum { MM_GUEST, MM_DRIVER, MM_LIBS, MM_DEVICE, MM_HEAP, MM_ANON, MM_FILES, MM_OTHER, MM_CATS };
static const char *const mm_name[MM_CATS] = {"guest", "driver-libs", "other-libs", "device", "heap", "anon", "files", "other"};
static int is_driver(const char *p) {
    static const char *const k[] = {"nvidia", "libvulkan", "mesa", "LLVM", "libdrm", "_dri", "lvp", "radeon", "intel", "libgallium", "glslang",
                                    "libVkLayer", "libGLX"};
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (strstr(p, k[i])) return 1;
    return 0;
}
void mem_map_report(const char *why, int guest_ranges) {
    /* a 64-bit process (the replay): every mapping counts, the holes are not reported */
    const int wide = sizeof(void *) > 4;
    const unsigned long long top = wide ? ~0ull : 0xFFFFE000ull;
    unsigned long long cat[MM_CATS] = {0}, prev = 0x10000, free_total = 0, largest = 0, anon_big = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    char line[768];
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        char path[512] = "";
        if (sscanf(line, "%llx-%llx %*s %*s %*s %*s %511[^\n]", &a, &b, path) < 2 || b <= a || a >= top) continue;
        if (b > top) b = top;
        if (a > prev) { free_total += a - prev; if (a - prev > largest) largest = a - prev; }
        if (b > prev) prev = b;
        const unsigned long long n = b - a;
        const int guest = guest_ranges && ((a >= 0x10000 && b <= 0x10000000ull) || (a >= 0x10000000ull && b <= 0x70000000ull) ||
                          (a >= 0x80000000ull && b <= 0x88000000ull));
        int c;
        if (!path[0] || !strncmp(path, "[anon", 5)) {
            c = guest ? MM_GUEST : MM_ANON;
            if (c == MM_ANON && n > anon_big) anon_big = n;
        } else if (!strcmp(path, "[heap]")) c = MM_HEAP;
        else if (path[0] == '[') c = MM_OTHER;
        else if (!strncmp(path, "/dev/", 5)) c = MM_DEVICE;
        else if (strstr(path, ".so")) c = is_driver(path) ? MM_DRIVER : MM_LIBS;
        else if (strstr(path, "xbrun")) c = MM_LIBS;
        else c = MM_FILES;
        cat[c] += n;
    }
    if (f) fclose(f);
    if (!wide && prev < top) { free_total += top - prev; if (top - prev > largest) largest = top - prev; }
    char out[512];
    int k = 0;
    for (int c = 0; c < MM_CATS; c++) k += snprintf(out + k, sizeof out - (size_t)k, "%s%s %u MB", c ? ", " : "", mm_name[c], MB(cat[c]));
    unsigned long long mapped = 0;
    for (int c = 0; c < MM_CATS; c++) mapped += cat[c];
    if (wide) printf("[mem-map] %s: %s (largest anon %u MB); mapped %u MB\n", why, out, MB(anon_big), MB(mapped));
    else
        printf("[mem-map] %s: %s (largest anon %u MB); mapped %u MB, free %u MB, largest hole %u MB\n", why, out, MB(anon_big), MB(mapped),
               MB(free_total), MB(largest));
}
#endif

