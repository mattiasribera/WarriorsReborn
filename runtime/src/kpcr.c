#define _GNU_SOURCE
#include "kpcr.h"
#include "kernel.h"
#include "mem.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host TLS: %gs on i386 glibc, the TEB on Windows; never the guest's %fs. */
static __thread XThread *cur_thread;
static uint32_t next_tid = 1;

#define W32(p, off, v) (*(uint32_t *)((p) + (off)) = (uint32_t)(v))

XThread *xthread_new(uint32_t stack_size, uint32_t tls_size, uint32_t ext_size) {
    XThread *t = calloc(1, sizeof *t);
    t->kpcr = guest_alloc(0x1000);
    t->ethread = guest_alloc(0x1000 + ext_size);
    if (stack_size < plat_min_guest_stack()) stack_size = plat_min_guest_stack();   /* host handlers run on this stack too */
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    uint8_t *stack = guest_alloc(stack_size);
    t->stack_limit = GA(stack);
    t->stack_base = GA(stack) + stack_size;
    uint32_t tls_data = t->stack_base - tls_size;      /* XAPI indexes TLS at StackBase - size */
    t->stack_top_for_call = (tls_data - 16) & ~15u;
    t->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);

    uint8_t *k = t->kpcr, *e = t->ethread;
    W32(k, KPCR_EXCEPTION_LIST, 0xFFFFFFFF);
    W32(k, KPCR_STACK_BASE, t->stack_base);
    W32(k, KPCR_STACK_LIMIT, t->stack_limit);
    W32(k, KPCR_SELF, GA(k));
    W32(k, KPCR_SELF_PCR, GA(k));
    W32(k, KPCR_PRCB, GA(k) + KPCR_CURRENT_THREAD);
    W32(k, KPCR_CURRENT_THREAD, GA(e));
    e[0] = 6;                       /* DISPATCHER_HEADER.Type = ThreadObject */
    e[2] = KTHREAD_SIZE / 4;
    W32(e, 8, GA(e) + 8);           /* empty WaitListHead */
    W32(e, 12, GA(e) + 8);
    W32(e, KTHREAD_STACK_BASE, t->stack_base);
    W32(e, KTHREAD_STACK_LIMIT, t->stack_limit);
    W32(e, KTHREAD_TLS_DATA, tls_size ? tls_data : 0);
    W32(e, ETHREAD_UNIQUE_THREAD, t->tid);
    return t;
}

void xthread_bind(XThread *t, int guest_stack) {
    plat_kpcr_bind(t->kpcr, t->stack_base, t->stack_limit, guest_stack);
    cur_thread = t;
}

XThread *xthread_current(void) { return cur_thread; }
uint8_t *kpcr_current(void) { return cur_thread ? cur_thread->kpcr : NULL; }
