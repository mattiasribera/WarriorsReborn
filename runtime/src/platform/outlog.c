/* See outlog.h. Portable so tee_test can check it on Linux. */
#define OUTLOG_IMPL
#include "outlog.h"
/* -include outlog.h may have defined the redirecting macros before OUTLOG_IMPL: drop them. */
#undef printf
#undef fprintf
#undef vprintf
#undef vfprintf
#undef perror
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HANDLE log_h = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION lock;
static LONG lock_ready;
static void lock_take(void) {
    if (InterlockedCompareExchange(&lock_ready, 1, 0) == 0) { InitializeCriticalSection(&lock); lock_ready = 2; }
    while (lock_ready != 2) Sleep(0);
    EnterCriticalSection(&lock);
}
static void lock_drop(void) { LeaveCriticalSection(&lock); }
static void os_write(int which, const char *buf, size_t n) {   /* which: 1 stdout, 2 stderr, 0 log */
    HANDLE h = which == 0 ? log_h : GetStdHandle(which == 2 ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD w;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, buf, (DWORD)n, &w, NULL);
}
int outlog_open(const char *path, int append) {
    log_h = CreateFileA(path, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return log_h == INVALID_HANDLE_VALUE ? -1 : 0;
}
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
static int log_fd = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_take(void) { pthread_mutex_lock(&lock); }
static void lock_drop(void) { pthread_mutex_unlock(&lock); }
static void os_write(int which, const char *buf, size_t n) {
    int fd = which == 0 ? log_fd : which;
    while (fd >= 0 && n) {
        ssize_t w = write(fd, buf, n);
        if (w <= 0) break;
        buf += w;
        n -= (size_t)w;
    }
}
int outlog_open(const char *path, int append) {
    log_fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    return log_fd < 0 ? -1 : 0;
}
#endif

void outlog_write(int to_stderr, const char *buf, size_t n) {
    lock_take();
    os_write(to_stderr ? 2 : 1, buf, n);
    os_write(0, buf, n);
    lock_drop();
}

int outlog_vfprintf(FILE *s, const char *fmt, va_list ap) {
    if (s != stdout && s != stderr) return vfprintf(s, fmt, ap);
    char small[2048], *buf = small;
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    if (n >= (int)sizeof small && (buf = malloc((size_t)n + 1))) vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    if (!buf) buf = small, n = sizeof small - 1;
    if (n > 0) outlog_write(s == stderr, buf, (size_t)n);
    if (buf != small) free(buf);
    return n;
}

int outlog_fprintf(FILE *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = outlog_vfprintf(s, fmt, ap);
    va_end(ap);
    return n;
}

int outlog_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = outlog_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

void outlog_perror(const char *s) {
    const char *e = strerror(errno);
    outlog_fprintf(stderr, "%s%s%s\n", s && *s ? s : "", s && *s ? ": " : "", e);
}
