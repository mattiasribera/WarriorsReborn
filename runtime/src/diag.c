/* Crash diagnostics: symbolization, fault reports, D3D success hook. */
#define _GNU_SOURCE
#include "diag.h"
#include "hle_audio.h"
#include "hle_d3d.h"
#include "kernel.h"
#include "kpcr.h"
#include "mem.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#define PLAT_FAULT_KIND "exception"
#else
#define PLAT_FAULT_KIND "signal"
#endif

typedef struct { uint32_t addr; char *name; } Sym;
static Sym *syms;
static int nsyms;

static int sym_cmp(const void *a, const void *b) {
    uint32_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

/* Parses XbSymbolDatabaseCLI output: "LIB__Name = 0xADDR". */
int symbols_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512], name[400];
    unsigned addr;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%399s = %x", name, &addr) != 2 || addr < 0x10000) continue;
        syms = realloc(syms, (nsyms + 1) * sizeof *syms);
        syms[nsyms++] = (Sym){addr, strdup(name)};
    }
    fclose(f);
    qsort(syms, nsyms, sizeof *syms, sym_cmp);
    return nsyms;
}

uint32_t symbol_lookup(const char *name) {
    for (int i = 0; i < nsyms; i++)
        if (!strcmp(syms[i].name, name)) return syms[i].addr;
    return 0;
}

uint32_t ksym_nearest(uint32_t addr, const char **name) {
    int lo = 0, hi = nsyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (syms[mid].addr <= addr) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) { *name = NULL; return addr; }
    *name = syms[best].name;
    return syms[best].addr;
}

static void print_sym(const char *label, uint32_t a) {
    const char *n;
    uint32_t b = ksym_nearest(a, &n);
    printf("%s %08x  %s+0x%x\n", label, a, n ? n : "?", n ? a - b : 0);
}

/* Prints dwords from the guest stack, symbolizing values that look like code addresses. */
void diag_stack_dump(uint32_t esp, int count) {
    printf("stack from %08x:\n", esp);
    for (int i = 0; i < count && mem_is_mapped(esp + i * 4); i++) {
        uint32_t v = ((uint32_t *)(uintptr_t)esp)[i];
        const char *n;
        uint32_t b = ksym_nearest(v, &n);
        printf("  [esp+%02x] %08x", i * 4, v);
        if (n && v >= 0x11000 && v < 0x35d000 && v - b < 0x10000) printf("  %s+0x%x", n, v - b);
        printf("\n");
    }
}

/* Walks the EBP chain (frame-pointer code only; best effort). */
void diag_backtrace(uint32_t ebp) {
    printf("ebp backtrace:\n");
    for (int i = 0; i < 24 && ebp && !(ebp & 3) && mem_is_mapped(ebp) && mem_is_mapped(ebp + 4); i++) {
        uint32_t ret = ((uint32_t *)(uintptr_t)ebp)[1];
        const char *n;
        uint32_t b = ksym_nearest(ret, &n);
        if (n && ret >= 0x11000 && ret < 0x60000000 && ret - b < 0x10000) printf("  #%d %08x  %s+0x%x\n", i, ret, n, ret - b);
        else printf("  #%d %08x\n", i, ret);
        uint32_t next = ((uint32_t *)(uintptr_t)ebp)[0];
        if (next <= ebp) break;
        ebp = next;
    }
}

/* Fault callback (platform.h): skips privileged cache flushes, otherwise reports and exits. */
static int fault_report(PlatRegs *r) {
    uint32_t eip = r->eip, esp = r->esp;
    /* Title code flushes the CPU caches with the privileged WBINVD (0f 09) after loading data for
     * the GPU (e.g. 0xbb24d). User mode traps it; on the host it is a no-op, so skip it. */
    if (r->privileged && eip < 0x60000000u && mem_is_mapped(eip) && ((uint8_t *)(uintptr_t)eip)[0] == 0x0F &&
        (((uint8_t *)(uintptr_t)eip)[1] == 0x09 || ((uint8_t *)(uintptr_t)eip)[1] == 0x08)) {
        /* Traps are expensive (a WOW64 exception dispatch on Windows): replace this one with a
         * 2-byte nop (66 90) so it traps once. The code pages are writable. */
        static int sites;
        __atomic_store_n((uint16_t *)(uintptr_t)eip, (uint16_t)0x9066, __ATOMIC_SEQ_CST);   /* one store: no half-patched opcode */
        printf("[diag] privileged WBINVD/INVD at %08x replaced by a nop (site %d)\n", eip, ++sites);
        r->eip = eip + 2;
        return 1;
    }
    XThread *t = xthread_current();
    printf("\n=== FAULT: %s %d (%s) code %d in guest thread t%u ===\n", PLAT_FAULT_KIND, r->code, r->what, r->subcode,
           t ? t->tid : 0);
    print_sym("EIP  ", eip);
    printf("fault address %08x%s\n", r->fault_addr,
           r->fault_addr >= 0xFD000000u ? "  (hardware MMIO range: NV2A 0xFD000000+, APU/ACI/USB 0xFE800000+)" : "");
    char where[200];
    plat_describe_addr(r->fault_addr, where, sizeof where);
    printf("fault page: %s\n", where);
    printf("eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n", r->eax, r->ebx, r->ecx,
           r->edx, r->esi, r->edi, r->ebp, esp);
    if (mem_is_mapped(eip)) {
        printf("code at EIP:");
        for (int i = 0; i < 16; i++) printf(" %02x", ((uint8_t *)(uintptr_t)eip)[i]);
        printf("\n");
    }
    diag_stack_dump(esp, 40);
    diag_backtrace(r->ebp);
    kring_dump(16);
    hle_audio_summary();
    hle_d3d_summary();
    fflush(stdout);
    return 0;
}

/* Watchdog sample callback: print where this guest thread currently is, then continue. */
static void sample_report(const PlatRegs *r) {
    XThread *t = xthread_current();
    printf("--- sample t%u ---\n", t ? t->tid : 0);
    print_sym("EIP  ", r->eip);
    if (mem_is_mapped(r->eip)) {
        printf("code at EIP:");
        for (int i = 0; i < 16; i++) printf(" %02x", ((uint8_t *)(uintptr_t)r->eip)[i]);
        printf("\n");
    }
    printf("eax=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n", r->eax, r->ecx, r->edx, r->esi, r->edi);
    diag_backtrace(r->ebp);
    if (r->eip < 0x60000000) {   /* guest code: show return-address candidates */
        uint32_t esp = r->esp;
        printf("code-like stack values:\n");
        for (int i = 0; i < 512 && mem_is_mapped(esp + i * 4); i++) {
            uint32_t v = ((uint32_t *)(uintptr_t)esp)[i];
            const char *n;
            uint32_t b = ksym_nearest(v, &n);
            if (n && v >= 0x11000 && v < 0x35d000 + 0xb0000 && v - b < 0x10000) printf("  [esp+%03x] %08x  %s+0x%x\n", i * 4, v, n, v - b);
        }
    }
    fflush(stdout);
}

void diag_thread_init(void) { plat_thread_init(); }

void diag_install(void) { plat_faults_install(fault_report, sample_report); }

#define MAX_HOOKS 16
static struct { uint32_t addr; const char *name; } hooks[MAX_HOOKS];
static int nhooks;
int d3d_hook_add(uint32_t addr, const char *name) {
    if (nhooks >= MAX_HOOKS) return -1;
    hooks[nhooks].addr = addr;
    hooks[nhooks].name = name;
    return nhooks++;
}

void __attribute__((noreturn)) d3d_hook_reached(uint32_t index, uint32_t caller) {
    const char *cn;
    uint32_t cb = ksym_nearest(caller, &cn);
    XThread *t = xthread_current();
    printf("POC SUCCESS: reached %s (%08x) from %08x (%s+0x%x) in t%u\n", hooks[index].name, hooks[index].addr,
           caller, cn ? cn : "?", caller - cb, t ? t->tid : 0);
    hle_audio_summary();
    plat_flush_output();
    _exit(0);
}
