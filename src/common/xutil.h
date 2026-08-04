/* xutil.h — tiny shared utilities: checked allocation, logging, time. */
#ifndef AT_XUTIL_H
#define AT_XUTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* Checked allocators: abort on OOM (a 30 KB daemon has no meaningful
 * degraded mode without memory; aborting is the honest behavior). */
void *xmalloc(size_t n);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Logging to stderr (daemon redirects to a log file when daemonized). */
typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERR } log_level;
void log_set_level(log_level lv);
void log_msg(log_level lv, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* die: log at ERR and exit(1). For unrecoverable startup errors only. */
_Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Monotonic milliseconds — for timers and backoff, never wall clock. */
uint64_t now_ms(void);

#endif
