/* The part of src/platform.h the renderer uses (dynamic loading of the Vulkan loader), for the
 * x86_64 replay build: src/platform/posix.c is the i386 runtime's layer (its fault handler reads
 * i386 registers) and does not build for x86_64. */
#include "../../src/platform.h"
#include <dlfcn.h>
void *plat_dlopen(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_dlsym(void *lib, const char *sym) { return dlsym(lib, sym); }
const char *plat_dlerror(void) { const char *e = dlerror(); return e ? e : "unknown error"; }
/* The pipeline workers' condition variables (--pipeline-async): as posix.c. */
int plat_cond_init_monotonic(pthread_cond_t *c) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    int r = pthread_cond_init(c, &ca);
    pthread_condattr_destroy(&ca);
    return r;
}
int plat_cond_timedwait_mono(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline) {
    return pthread_cond_timedwait(c, m, deadline);
}
