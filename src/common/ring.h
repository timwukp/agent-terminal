/* ring.h — growable byte ring buffer with a hard capacity ceiling.
 *
 * Used for per-client output queues and framed-read staging. The ceiling is
 * the backpressure mechanism: ring_write() past max_cap fails, and the caller
 * (daemon) disconnects the slow client rather than buffer unboundedly. */
#ifndef AT_RING_H
#define AT_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t cap;      /* current allocation, power of two */
    size_t max_cap;  /* hard ceiling; 0 = unlimited */
    size_t head;     /* read position */
    size_t len;      /* bytes stored */
} ring;

void ring_init(ring *r, size_t initial_cap, size_t max_cap);
void ring_free(ring *r);

/* Appends n bytes; grows up to max_cap. Returns false (ring unchanged) if the
 * write would exceed max_cap — caller decides the eviction policy. */
bool ring_write(ring *r, const void *data, size_t n);

/* Copies up to n bytes into out, consuming them. Returns bytes copied. */
size_t ring_read(ring *r, void *out, size_t n);

/* Peek without consuming; returns bytes copied. */
size_t ring_peek(const ring *r, void *out, size_t n);

/* Drop n bytes from the front (n > len drops everything). */
void ring_consume(ring *r, size_t n);

static inline size_t ring_len(const ring *r) { return r->len; }

#endif
