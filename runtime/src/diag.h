#ifndef DIAG_H
#define DIAG_H
#include <stdint.h>
int symbols_load(const char *path);
uint32_t symbol_lookup(const char *name);   /* exact name, 0 if missing */
void diag_install(void);                      /* signal handlers (call per host thread for altstack) */
void diag_thread_init(void);
void diag_stack_dump(uint32_t esp, int count);
void diag_backtrace(uint32_t ebp);
int d3d_hook_add(uint32_t addr, const char *name);  /* returns hook index */
#endif
