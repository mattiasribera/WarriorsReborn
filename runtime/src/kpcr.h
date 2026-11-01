/* Per-thread Xbox environment: KPCR reachable through %fs, ETHREAD, guest stack, TLS. */
#ifndef KPCR_H
#define KPCR_H
#include <stdint.h>

/* Offsets used by XAPI/CRT (verified by disassembly, see README). */
#define KPCR_EXCEPTION_LIST 0x00
#define KPCR_STACK_BASE 0x04
#define KPCR_STACK_LIMIT 0x08
#define KPCR_SELF 0x18
#define KPCR_SELF_PCR 0x1C
#define KPCR_PRCB 0x20
#define KPCR_IRQL 0x24
#define KPCR_CURRENT_THREAD 0x28   /* PrcbData.CurrentThread */
#define KTHREAD_STACK_BASE 0x1C
#define KTHREAD_STACK_LIMIT 0x20
#define KTHREAD_TLS_DATA 0x28
#define KTHREAD_SIZE 0x110
#define ETHREAD_UNIQUE_THREAD 0x12C
#define ETHREAD_START_ADDRESS 0x130
#define ETHREAD_SIZE 0x140

typedef struct XThread {
    uint8_t *kpcr;
    uint8_t *ethread;          /* ETHREAD (+ extension); starts with KTHREAD/DISPATCHER_HEADER */
    uint32_t stack_base, stack_limit, stack_top_for_call;
    uint32_t tid;
    uint32_t start_routine, start_context, system_routine;
    int suspended;
    uintptr_t host_thread;   /* plat_thread_self(), for watchdog sampling */
    struct XThread *next;
} XThread;

/* Creates the environment; stack_size/tls_size as passed to PsCreateSystemThreadEx. */
XThread *xthread_new(uint32_t stack_size, uint32_t tls_size, uint32_t ext_size);
/* Binds t to the calling host thread: its KPCR becomes the thread's %fs view (see
 * platform.h). guest_stack: this host thread runs on t's guest stack from now on. */
void xthread_bind(XThread *t, int guest_stack);
XThread *xthread_current(void);
uint8_t *kpcr_current(void);
#endif
