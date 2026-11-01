/* Miscellaneous exports: data exports, HAL/AV stubs, settings, Rtl helpers, sections. */
#define _GNU_SOURCE
#include "kernel.h"
#include "nvset.h"
#include "platform.h"
#include "kobj.h"
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define U16(p, o) (*(uint16_t *)((uintptr_t)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uintptr_t)(p) + (o)))
extern uint32_t guest_call(uint32_t fn, uint32_t stack_top, int nargs, const uint32_t *args);

static uint8_t object_types[6][64];
static uint32_t launch_data_page;              /* NULL: not launched with data */
static uint32_t disk_cache_partitions = 3;
static uint8_t hardware_info[8] = {0, 0, 0, 0, 0xD3, 0xD3, 0, 0};   /* Flags, GpuRevision, McpRevision (values arbitrary) */
static uint16_t krnl_version[4] = {1, 0, 5838, 1};
static uint8_t hd_key[16], signature_key[16], alt_signature_keys[16][16];
static uint32_t boot_smc_video_mode;
static uint8_t idex_channel[256];
static char image_name[] = "\\Device\\CdRom0\\default.xbe";
static uint32_t image_file_name[2];             /* ANSI_STRING */

static uint64_t k_zero(KFrame *f, const uint32_t *a) { return 0; }
static uint64_t k_true(KFrame *f, const uint32_t *a) { return 1; }

static uint64_t k_DbgPrint(KFrame *f, const uint32_t *a) {
    char buf[512];
    char *va = (char *)&f->args[1];   /* i386 va_list is a plain pointer to the next stack arg */
    va_list ap;
    memcpy(&ap, &va, sizeof va);
    vsnprintf(buf, sizeof buf, (const char *)G(a[0]), ap);
    klog("[dbg] %s%s", buf, buf[0] && buf[strlen(buf) - 1] == '\n' ? "" : "\n");
    return 0;
}
static uint64_t k_DbgBreakPoint(KFrame *f, const uint32_t *a) { klog("[krnl] DbgBreakPoint ignored\n"); return 0; }

/* ExQueryNonVolatileSetting(ValueIndex, Type*, Value, ValueLength, ResultLength*) */
static uint64_t k_ExQueryNonVolatileSetting(KFrame *f, const uint32_t *a) {
    uint32_t v = nvset_value(a[0]);   /* src/nvset.c */
    if (a[0] == 0xFFFF) kfatal(f, "ExQueryNonVolatileSetting(XC_MAX_ALL) not implemented");
    if (a[1]) U32(a[1], 0) = 4;          /* REG_DWORD */
    if (a[3] < 4) return STATUS_BUFFER_TOO_SMALL;
    memset(G(a[2]), 0, a[3]);
    U32(a[2], 0) = v;
    if (a[4]) U32(a[4], 0) = 4;
    return STATUS_SUCCESS;
}

static uint64_t k_FscGetCacheSize(KFrame *f, const uint32_t *a) { return 16; }   /* pages */
static uint64_t k_HalGetInterruptVector(KFrame *f, const uint32_t *a) {
    if (a[1]) *(uint8_t *)G(a[1]) = 0;   /* KIRQL out */
    return 0x30 + a[0];
}
/* HalReadWritePCISpace(Bus, Slot, RegisterNumber, Buffer, Length, WritePCISpace) */
static uint64_t k_HalReadWritePCISpace(KFrame *f, const uint32_t *a) {
    if (!(uint8_t)a[5]) memset(G(a[3]), 0, a[4]);
    return 0;
}
static uint64_t k_HalReturnToFirmware(KFrame *f, const uint32_t *a) {
    kfatal(f, "HalReturnToFirmware(%u): title requested reboot/dashboard", a[0]);
}
static uint64_t k_KeBugCheck(KFrame *f, const uint32_t *a) { kfatal(f, "KeBugCheck(%x)", a[0]); }

static uint64_t k_KeQueryInterruptTime(KFrame *f, const uint32_t *a) { return ktime_interrupt(); }
static uint64_t k_KeQuerySystemTime(KFrame *f, const uint32_t *a) {
    uint64_t t = ktime_system();
    memcpy(G(a[0]), &t, 8);
    return 0;
}
/* A stall is a busy wait of a few microseconds; usleep would round up to a whole millisecond
 * on Windows (winpthreads sleeps with Sleep()). */
static uint64_t k_KeStallExecutionProcessor(KFrame *f, const uint32_t *a) {
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += a[0] / 1000000;
    dl.tv_nsec += (long)(a[0] % 1000000) * 1000;
    if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    if (a[0] >= 2000) { plat_sleep_until(&dl); return 0; }
    struct timespec now;
    do clock_gettime(CLOCK_MONOTONIC, &now);
    while (now.tv_sec < dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec < dl.tv_nsec));
    return 0;
}
/* KeSynchronizeExecution(Interrupt, SynchronizeRoutine, SynchronizeContext) */
static uint64_t k_KeSynchronizeExecution(KFrame *f, const uint32_t *a) {
    uint32_t arg = a[2];
    return guest_call(a[1], 0, 1, &arg);
}

static uint64_t k_RtlCompareMemory(KFrame *f, const uint32_t *a) {
    const uint8_t *x = G(a[0]), *y = G(a[1]);
    uint32_t i = 0;
    while (i < a[2] && x[i] == y[i]) i++;
    return i;
}
static uint64_t k_RtlCompareMemoryUlong(KFrame *f, const uint32_t *a) {
    const uint32_t *x = G(a[0]);
    uint32_t i = 0;
    while (i + 4 <= a[1] && x[i / 4] == a[2]) i += 4;
    return i;
}
static uint64_t k_RtlFillMemoryUlong(KFrame *f, const uint32_t *a) {
    uint32_t *x = G(a[0]);
    for (uint32_t i = 0; i < a[1] / 4; i++) x[i] = a[2];
    return 0;
}
/* RtlInitAnsiString(DestinationString, SourceString) */
static uint64_t k_RtlInitAnsiString(KFrame *f, const uint32_t *a) {
    uint32_t n = a[1] ? strlen(G(a[1])) : 0;
    U16(a[0], 0) = n;
    U16(a[0], 2) = a[1] ? n + 1 : 0;
    U32(a[0], 4) = a[1];
    return 0;
}
static int cmp_string(uint32_t s1, uint32_t s2, int ci) {
    uint32_t n1 = U16(s1, 0), n2 = U16(s2, 0), n = n1 < n2 ? n1 : n2;
    const uint8_t *p1 = G(U32(s1, 4)), *p2 = G(U32(s2, 4));
    for (uint32_t i = 0; i < n; i++) {
        int c1 = p1[i], c2 = p2[i];
        if (ci) { c1 = c1 >= 'a' && c1 <= 'z' ? c1 - 32 : c1; c2 = c2 >= 'a' && c2 <= 'z' ? c2 - 32 : c2; }
        if (c1 != c2) return c1 - c2;
    }
    return (int)n1 - (int)n2;
}
static uint64_t k_RtlCompareString(KFrame *f, const uint32_t *a) { return (uint32_t)cmp_string(a[0], a[1], (uint8_t)a[2]); }
static uint64_t k_RtlEqualString(KFrame *f, const uint32_t *a) { return cmp_string(a[0], a[1], (uint8_t)a[2]) == 0; }
static uint64_t k_RtlNtStatusToDosError(KFrame *f, const uint32_t *a) {
    switch (a[0]) {
    case STATUS_SUCCESS: return 0;
    case STATUS_OBJECT_NAME_NOT_FOUND: case STATUS_NO_SUCH_FILE: return 2;      /* ERROR_FILE_NOT_FOUND */
    case STATUS_OBJECT_PATH_NOT_FOUND: return 3;                                 /* ERROR_PATH_NOT_FOUND */
    case STATUS_ACCESS_DENIED: return 5;
    case STATUS_INVALID_HANDLE: return 6;
    case STATUS_NO_MEMORY: return 8;
    case STATUS_END_OF_FILE: return 38;                                          /* ERROR_HANDLE_EOF */
    case STATUS_OBJECT_NAME_COLLISION: return 183;                               /* ERROR_ALREADY_EXISTS */
    case STATUS_TIMEOUT: return 1460;
    case STATUS_NO_MORE_FILES: return 18;
    default: return 317;                                                          /* ERROR_MR_MID_NOT_FOUND */
    }
}

/* XeLoadSection/XeUnloadSection(PXBE_SECTION_HEADER): all sections are preloaded by the
 * loader, so only the reference count (offset 0x18 in the section header) is tracked. */
static uint64_t k_XeLoadSection(KFrame *f, const uint32_t *a) { U32(a[0], 0x18)++; return STATUS_SUCCESS; }
static uint64_t k_XeUnloadSection(KFrame *f, const uint32_t *a) {
    if (U32(a[0], 0x18)) U32(a[0], 0x18)--;
    return STATUS_SUCCESS;
}

void kmisc_register(void) {
    static const int type_ords[6] = {16, 22, 30, 31, 71, 259};
    for (int i = 0; i < 6; i++) kernel_register_data(type_ords[i], object_types[i]);
    image_file_name[0] = (uint32_t)(sizeof image_name - 1) | (uint32_t)sizeof image_name << 16;
    image_file_name[1] = GA(image_name);
    kernel_register_data(164, &launch_data_page);
    kernel_register_data(40, &disk_cache_partitions);
    kernel_register_data(322, hardware_info);
    kernel_register_data(324, krnl_version);
    kernel_register_data(323, hd_key);
    kernel_register_data(325, signature_key);
    kernel_register_data(354, alt_signature_keys);
    kernel_register_data(356, &boot_smc_video_mode);
    kernel_register_data(357, idex_channel);
    kernel_register_data(326, image_file_name);

    kernel_register(1, k_zero);     /* AvGetSavedDataAddress */
    kernel_register(2, k_zero);     /* AvSendTVEncoderOption */
    kernel_register(3, k_zero);     /* AvSetDisplayMode */
    kernel_register(4, k_zero);     /* AvSetSavedDataAddress */
    kernel_register(5, k_DbgBreakPoint);
    kernel_register(8, k_DbgPrint);
    kernel_register(24, k_ExQueryNonVolatileSetting);
    kernel_register(35, k_FscGetCacheSize);
    kernel_register(37, k_zero);    /* FscSetCacheSize */
    kernel_register(44, k_HalGetInterruptVector);
    kernel_register(46, k_HalReadWritePCISpace);
    kernel_register(47, k_zero);    /* HalRegisterShutdownNotification */
    kernel_register(49, k_HalReturnToFirmware);
    kernel_register(95, k_KeBugCheck);
    kernel_register(98, k_true);    /* KeConnectInterrupt: accepted, never fires */
    kernel_register(100, k_true);   /* KeDisconnectInterrupt */
    kernel_register(109, k_zero);   /* KeInitializeInterrupt */
    kernel_register(125, k_KeQueryInterruptTime);
    kernel_register(128, k_KeQuerySystemTime);
    kernel_register(139, k_zero);   /* KeRestoreFloatingPointState */
    kernel_register(142, k_zero);   /* KeSaveFloatingPointState */
    kernel_register(151, k_KeStallExecutionProcessor);
    kernel_register(153, k_KeSynchronizeExecution);
    kernel_register(268, k_RtlCompareMemory);
    kernel_register(269, k_RtlCompareMemoryUlong);
    kernel_register(270, k_RtlCompareString);
    kernel_register(279, k_RtlEqualString);
    kernel_register(285, k_RtlFillMemoryUlong);
    kernel_register(289, k_RtlInitAnsiString);
    kernel_register(301, k_RtlNtStatusToDosError);
    kernel_register(327, k_XeLoadSection);
    kernel_register(328, k_XeUnloadSection);
}
