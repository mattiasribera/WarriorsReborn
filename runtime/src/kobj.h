/* Handles, dispatcher objects, waits, threads, timers and DPCs. */
#ifndef KOBJ_H
#define KOBJ_H
#include <stdint.h>
#include "kpcr.h"

typedef enum { H_FREE, H_EVENT, H_SEMAPHORE, H_MUTANT, H_TIMER, H_THREAD, H_FILE, H_SYMLINK, H_IOCOMPLETION } HType;

typedef struct {
    HType type;
    uint32_t body;    /* guest pointer to the kernel object (DISPATCHER_HEADER first) */
    int fd;           /* H_FILE */
    char *path;       /* H_FILE host path, H_SYMLINK target */
    void *dir;        /* H_FILE directory enumeration state */
    int read_only;    /* H_FILE on the disc: never changed on the host */
    int refs;
} HObj;

uint32_t handle_new(HType type, uint32_t body);
HObj *handle_get(uint32_t h);
void handle_close(uint32_t h);

void kobj_start_entry(uint32_t entry, uint32_t stack_size);
uint32_t kobj_live_threads(void);   /* guest threads running now ([crash-context]) */
void kobj_service_loop(void) __attribute__((noreturn));
/* Called by the service loop (host main thread) about every millisecond, outside the kernel lock. */
extern void (*kobj_idle_hook)(void);
uint64_t ktime_interrupt(void);   /* 100ns units since boot */
uint64_t ktime_system(void);      /* 100ns units since 1601 */
int32_t kobj_set_signal(uint32_t obj, int32_t value);   /* sets SignalState and wakes waiters */
#endif
