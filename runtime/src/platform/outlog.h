/* Console + log file output for Windows builds (force-included with -include by the windows
 * target and by tee_test): printf/fprintf(stdout|stderr)/vprintf/vfprintf/perror format the
 * text once and write it with one OS write to the original stdout/stderr handle and one to the
 * log file. No pipes or fd redirection: the console handle is never replaced. */
#if !defined(OUTLOG_H) && !defined(__ASSEMBLER__)   /* also force-included into .S files */
#define OUTLOG_H
#include <stdarg.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
int outlog_open(const char *path, int append);   /* 0 ok, -1 if the log cannot be opened */
void outlog_write(int to_stderr, const char *buf, size_t n);
int outlog_vfprintf(FILE *s, const char *fmt, va_list ap);
int outlog_fprintf(FILE *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int outlog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void outlog_perror(const char *s);
#ifdef __cplusplus
}
#endif
#ifndef OUTLOG_IMPL
#define printf(...) outlog_printf(__VA_ARGS__)
#define fprintf(...) outlog_fprintf(__VA_ARGS__)
#define vprintf(f, ap) outlog_vfprintf(stdout, f, ap)
#define vfprintf(s, f, ap) outlog_vfprintf(s, f, ap)
#define perror(s) outlog_perror(s)
#endif
#endif
