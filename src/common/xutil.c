#include "xutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

void *xcalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb ? nmemb : 1, size ? size : 1);
    if (!p) die("out of memory (%zu x %zu)", nmemb, size);
    return p;
}

void *xrealloc(void *old, size_t n) {
    void *p = realloc(old, n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static log_level g_level = LOG_INFO;

void log_set_level(log_level lv) { g_level = lv; }

static const char *lv_name(log_level lv) {
    switch (lv) {
    case LOG_DEBUG: return "debug";
    case LOG_INFO:  return "info";
    case LOG_WARN:  return "warn";
    default:        return "error";
    }
}

void log_msg(log_level lv, const char *fmt, ...) {
    if (lv < g_level) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", lv_name(lv));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

_Noreturn void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[fatal] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
