/* Drives src/platform/outlog.c (force-included as on Windows): four threads print numbered
 * lines through printf/fprintf/vprintf/perror while the log is open; tests/test_outlog.py
 * checks that stdout, stderr and the log hold each line exactly once, whole. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

static void vp(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void *worker(void *arg) {
    int t = (int)(long)arg;
    for (int i = 0; i < 200; i++) {
        if (i % 4 == 0) printf("out t%d line %d\n", t, i);
        else if (i % 4 == 1) fprintf(stdout, "out t%d line %d\n", t, i);
        else if (i % 4 == 2) vp("out t%d line %d\n", t, i);
        else fprintf(stderr, "err t%d line %d\n", t, i);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2 || outlog_open(argv[1], 0)) return 2;
    pthread_t th[4];
    for (long t = 0; t < 4; t++) pthread_create(&th[t], NULL, worker, (void *)t);
    for (int t = 0; t < 4; t++) pthread_join(th[t], NULL);
    errno = ENOENT;
    perror("perror check");
    printf("long %0*d\n", 5000, 7);   /* longer than the 2 KB stack buffer */
    return 0;
}
