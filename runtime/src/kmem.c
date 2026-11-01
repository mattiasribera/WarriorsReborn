/* Memory manager exports. Contiguous ("physical") memory lives in a window at
 * 0x80000000 so that physical address = VA - 0x80000000. */
#include "kernel.h"
#include "mem.h"
#include <stdlib.h>
#include <string.h>

#define U32(p, o) (*(uint32_t *)((uintptr_t)(p) + (o)))
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04

/* NtAllocateVirtualMemory(BaseAddress*, ZeroBits, RegionSize*, AllocationType, Protect) */
static uint64_t k_NtAllocateVirtualMemory(KFrame *f, const uint32_t *a) {
    uint32_t base = U32(a[0], 0) & ~0xFFFu, size = (U32(a[2], 0) + 0xFFF) & ~0xFFFu;
    if (base) {
        uint32_t rsize, rbase = guest_region(base, &rsize);
        if (rbase && base + size <= rbase + rsize) {   /* commit inside an existing reservation */
            U32(a[0], 0) = base;
            U32(a[2], 0) = size;
            return STATUS_SUCCESS;
        }
        if (!guest_alloc_at(base, size)) return STATUS_NO_MEMORY;
    } else {
        void *p = guest_alloc(size);
        if (!p) return STATUS_NO_MEMORY;
        base = GA(p);
    }
    U32(a[0], 0) = base;
    U32(a[2], 0) = size;
    return STATUS_SUCCESS;
}

/* NtFreeVirtualMemory(BaseAddress*, RegionSize*, FreeType) */
static uint64_t k_NtFreeVirtualMemory(KFrame *f, const uint32_t *a) {
    if (a[2] & MEM_RELEASE) {
        uint32_t size;
        if (guest_free(U32(a[0], 0), &size)) return STATUS_INVALID_PARAMETER;
        U32(a[1], 0) = size;
    }
    return STATUS_SUCCESS;   /* decommit: keep pages */
}

/* NtQueryVirtualMemory(BaseAddress, MEMORY_BASIC_INFORMATION*) */
static uint64_t k_NtQueryVirtualMemory(KFrame *f, const uint32_t *a) {
    uint32_t size, base = guest_region(a[0], &size);
    uint32_t *m = G(a[1]);
    memset(m, 0, 28);
    m[0] = a[0] & ~0xFFFu;
    if (base) {
        m[1] = base; m[2] = PAGE_READWRITE; m[3] = base + size - m[0]; m[4] = MEM_COMMIT; m[5] = PAGE_READWRITE;
        m[6] = 0x20000;   /* MEM_PRIVATE */
    } else {
        m[3] = 0x1000; m[4] = 0x10000;   /* MEM_FREE */
        m[5] = 1;                         /* PAGE_NOACCESS */
    }
    return STATUS_SUCCESS;
}
static uint64_t k_NtProtectVirtualMemory(KFrame *f, const uint32_t *a) {
    if (a[3]) U32(a[3], 0) = PAGE_READWRITE;
    return STATUS_SUCCESS;
}

static uint64_t k_MmAllocateContiguousMemory(KFrame *f, const uint32_t *a) {
    return contig_alloc(a[0], 0x1000, 0, 0xFFFFFFFF);
}
/* MmAllocateContiguousMemoryEx(Size, LowestAcceptable, HighestAcceptable, Alignment, Protect) */
static uint64_t k_MmAllocateContiguousMemoryEx(KFrame *f, const uint32_t *a) {
    return contig_alloc(a[0], a[3], a[1], a[2]);
}
static uint64_t k_MmFreeContiguousMemory(KFrame *f, const uint32_t *a) { return 0; }
static uint64_t k_MmPersistContiguousMemory(KFrame *f, const uint32_t *a) { return 0; }
static uint64_t k_MmGetPhysicalAddress(KFrame *f, const uint32_t *a) {
    if (a[0] >= CONTIG_BASE && a[0] < CONTIG_BASE + CONTIG_SIZE) return a[0] - CONTIG_BASE;
    return a[0];   /* identity for non-contiguous memory (logged by the dispatcher) */
}
static uint64_t k_MmAllocateSystemMemory(KFrame *f, const uint32_t *a) { return GA(guest_alloc(a[0])); }
static uint64_t k_MmFreeSystemMemory(KFrame *f, const uint32_t *a) {
    uint32_t size = 0;
    guest_free(a[0], &size);
    return size / 0x1000;
}
static uint64_t k_MmQueryAllocationSize(KFrame *f, const uint32_t *a) {
    uint32_t s = contig_size(a[0]);
    if (!s) guest_region(a[0], &s);
    return s;
}
static uint64_t k_MmQueryAddressProtect(KFrame *f, const uint32_t *a) { return PAGE_READWRITE; }
static uint64_t k_MmSetAddressProtect(KFrame *f, const uint32_t *a) { return 0; }
static uint64_t k_MmLockUnlock(KFrame *f, const uint32_t *a) { return 0; }
static uint64_t k_MmQueryStatistics(KFrame *f, const uint32_t *a) {
    uint32_t *s = G(a[0]);
    if (s[0] != 36) return STATUS_INVALID_PARAMETER;
    memset(s + 1, 0, 32);
    s[1] = 64 * 256;   /* 64 MB in pages */
    s[2] = 32 * 256;
    return STATUS_SUCCESS;
}
static uint64_t k_MmClaimGpuInstanceMemory(KFrame *f, const uint32_t *a) {
    kfatal(f, "MmClaimGpuInstanceMemory (GPU instance memory) reached");
}

/* Pool: host heap with a size header so ExQueryPoolBlockSize works. */
static uint64_t k_ExAllocatePoolWithTag(KFrame *f, const uint32_t *a) {
    uint32_t *p = calloc(1, a[0] + 16);
    if (!p) return 0;
    p[0] = a[0];
    return GA(p) + 16;
}
static uint64_t k_ExFreePool(KFrame *f, const uint32_t *a) {
    if (a[0]) free((uint8_t *)G(a[0]) - 16);
    return 0;
}
static uint64_t k_ExQueryPoolBlockSize(KFrame *f, const uint32_t *a) { return U32(a[0] - 16, 0); }

void kmem_register(void) {
    kernel_register(184, k_NtAllocateVirtualMemory);
    kernel_register(199, k_NtFreeVirtualMemory);
    kernel_register(217, k_NtQueryVirtualMemory);
    kernel_register(204, k_NtProtectVirtualMemory);
    kernel_register(165, k_MmAllocateContiguousMemory);
    kernel_register(166, k_MmAllocateContiguousMemoryEx);
    kernel_register(171, k_MmFreeContiguousMemory);
    kernel_register(178, k_MmPersistContiguousMemory);
    kernel_register(173, k_MmGetPhysicalAddress);
    kernel_register(167, k_MmAllocateSystemMemory);
    kernel_register(172, k_MmFreeSystemMemory);
    kernel_register(180, k_MmQueryAllocationSize);
    kernel_register(179, k_MmQueryAddressProtect);
    kernel_register(182, k_MmSetAddressProtect);
    kernel_register(175, k_MmLockUnlock);
    kernel_register(176, k_MmLockUnlock);
    kernel_register(181, k_MmQueryStatistics);
    kernel_register(168, k_MmClaimGpuInstanceMemory);
    kernel_register(15, k_ExAllocatePoolWithTag);
    kernel_register(17, k_ExFreePool);
    kernel_register(23, k_ExQueryPoolBlockSize);
}
