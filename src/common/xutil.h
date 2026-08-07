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

/* Guarantee fds 0, 1 and 2 are open, reopening any that are not onto /dev/null.
 * Call once at the very top of main(), before any pipe/socket/open.
 *
 * Not defensive boilerplate: both binaries create a self-pipe during startup,
 * and pipe() returns the LOWEST free descriptors. Started with fd 0 closed —
 * which a parent can do trivially, and `sh -c 'exec 0<&-; exec prog'` does —
 * stdin silently becomes the read end of that self-pipe, so the client polls a
 * pipe nobody writes and blocks forever instead of detaching. Worse, the pipe's
 * write end is called from a signal handler, so a program reading "stdin" would
 * be reading its own SIGWINCH notifications.
 *
 * Returns the number of descriptors it had to reopen (0 in the normal case), so
 * a caller can log the anomaly. */
int fd_sanitize_std(void);

#endif
