/* Kernel import dispatch: per-ordinal stubs, logging, recent-call ring buffer. */
#define _GNU_SOURCE
#include "kernel.h"
#include "kpcr.h"
#include "diag.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include <unistd.h>

extern void kentry_common(void);

static KHandler handlers[KEXPORT_COUNT];
static void *data_exports[KEXPORT_COUNT];
static uint8_t *stubs;          /* 16 bytes per ordinal */
static uint32_t call_counts[KEXPORT_COUNT];
int klog_enabled = 0;           /* per-call [krnl]/[file] log, on with --trace */
int klog_limit = 0;             /* 0 = log every call; N = first N calls per ordinal */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

#define RING 64
static KCallRec ring[RING];
static unsigned ring_pos;

void klog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&log_lock);
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
    va_end(ap);
}

void kernel_register(uint32_t o, KHandler h) { handlers[o] = h; }
void kernel_register_data(uint32_t o, void *d) { data_exports[o] = d; }
int kernel_is_implemented(uint32_t o) { return o < KEXPORT_COUNT && (handlers[o] || data_exports[o]); }

void kernel_init(void) {
    stubs = plat_alloc_exec(KEXPORT_COUNT * 16);
    for (uint32_t o = 0; o < KEXPORT_COUNT; o++) {
        uint8_t *s = stubs + o * 16;
        int pop = 0;
        if (kexports[o].conv == CC_STDCALL) pop = 4 * kexports[o].arg_dwords;
        if (kexports[o].conv == CC_FASTCALL && kexports[o].arg_dwords > 2) pop = 4 * (kexports[o].arg_dwords - 2);
        s[0] = 0x68; memcpy(s + 1, &o, 4);                               /* push ordinal */
        int32_t rel = (int32_t)((uintptr_t)kentry_common - (uintptr_t)(s + 10));
        s[5] = 0xE8; memcpy(s + 6, &rel, 4);                             /* call kentry_common */
        s[10] = 0x83; s[11] = 0xC4; s[12] = 0x04;                        /* add esp, 4 */
        s[13] = 0xC2; s[14] = pop & 0xFF; s[15] = pop >> 8;              /* ret pop */
    }
    kmem_register();
    kobj_register();
    kfile_register();
    kmisc_register();
    kxc_register();
}

uint32_t kernel_resolve(uint32_t o) {
    if (o < KEXPORT_COUNT && kexports[o].conv == CC_DATA) {
        if (!data_exports[o]) {   /* unknown data export: zeroed page, logged on resolve */
            fprintf(stderr, "[krnl] data export %u %s not provided, using zero page\n", o, kexports[o].name);
            data_exports[o] = calloc(1, 0x1000);
        }
        return GA(data_exports[o]);
    }
    return GA(stubs + (o < KEXPORT_COUNT ? o : 0) * 16);
}

void kfatal(KFrame *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&log_lock);
    fprintf(stdout, "[fatal] ");
    vfprintf(stdout, fmt, ap);
    if (f) {
        const char *sym = NULL;
        uint32_t base = ksym_nearest(f->ret_addr, &sym);
        fprintf(stdout, " (caller %08x %s+0x%x)", f->ret_addr, sym ? sym : "?", f->ret_addr - base);
    }
    fprintf(stdout, "\n");
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
    va_end(ap);
    if (f) { diag_stack_dump(f->esp + 8, 16); diag_backtrace(f->ebp); }
    kring_dump(24);
    plat_flush_output();
    _exit(3);
}

void kring_dump(int max) {
    unsigned end = __atomic_load_n(&ring_pos, __ATOMIC_SEQ_CST);
    int n = end < (unsigned)max ? (int)end : max;
    fprintf(stdout, "--- last %d kernel calls (oldest first) ---\n", n);
    for (unsigned i = end - n; i != end; i++) {
        KCallRec *r = &ring[i % RING];
        fprintf(stdout, "  t%u %s(%x, %x, %x, %x) from %08x\n", r->tid,
                r->ordinal < KEXPORT_COUNT ? kexports[r->ordinal].name : "?",
                r->args[0], r->args[1], r->args[2], r->args[3], r->caller);
    }
    fflush(stdout);
}

void kernel_dispatch(KFrame *f) {
    uint32_t o = f->ordinal;
    const KExport *e = &kexports[o];
    uint32_t a[16];
    int n = e->arg_dwords > 16 ? 16 : e->arg_dwords;
    if (e->conv == CC_FASTCALL) {
        a[0] = f->ecx; a[1] = f->edx;
        for (int i = 2; i < n; i++) a[i] = f->args[i - 2];
    } else {
        for (int i = 0; i < n; i++) a[i] = f->args[i];
        if (e->conv == CC_CDECL) for (int i = n; i < 8; i++) a[i] = f->args[i];  /* varargs */
    }
    XThread *t = xthread_current();
    unsigned slot = __atomic_fetch_add(&ring_pos, 1, __ATOMIC_SEQ_CST) % RING;
    ring[slot] = (KCallRec){o, f->ret_addr, t ? t->tid : 0, {a[0], a[1], a[2], a[3]}};
    if (!handlers[o])
        kfatal(f, "unimplemented kernel import %u %s (%s, %d arg dwords)", o, e->name,
               e->conv == CC_FASTCALL ? "fastcall" : e->conv == CC_CDECL ? "cdecl" : "stdcall", e->arg_dwords);
    uint32_t count = __atomic_add_fetch(&call_counts[o], 1, __ATOMIC_SEQ_CST);
    uint64_t r = handlers[o](f, a);
    f->eax = (uint32_t)r;
    f->edx = (uint32_t)(r >> 32);
    if (klog_enabled && (!klog_limit || count <= (uint32_t)klog_limit)) {
        char buf[256];
        int len = snprintf(buf, sizeof buf, "[krnl] t%u %3u %s(", t ? t->tid : 0, o, e->name);
        for (int i = 0; i < n && len < 200; i++) len += snprintf(buf + len, sizeof buf - len, "%s%x", i ? ", " : "", a[i]);
        klog("%s) -> %x  [from %08x]\n", buf, f->eax, f->ret_addr);
    }
}
