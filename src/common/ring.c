#include "ring.h"

#include <stdlib.h>
#include <string.h>

#include "xutil.h"

static size_t next_pow2(size_t n) {
    size_t c = 16;
    while (c < n) c <<= 1;
    return c;
}

void ring_init(ring *r, size_t initial_cap, size_t max_cap) {
    r->cap = next_pow2(initial_cap);
    if (max_cap && r->cap > max_cap) r->cap = max_cap;
    r->buf = xmalloc(r->cap);
    r->max_cap = max_cap;
    r->head = 0;
    r->len = 0;
}

void ring_free(ring *r) {
    free(r->buf);
    memset(r, 0, sizeof *r);
}

static void ring_grow(ring *r, size_t need) {
    size_t newcap = next_pow2(need);
    uint8_t *nb = xmalloc(newcap);
    size_t first = r->cap - r->head;
    if (first > r->len) first = r->len;
    memcpy(nb, r->buf + r->head, first);
    memcpy(nb + first, r->buf, r->len - first);
    free(r->buf);
    r->buf = nb;
    r->cap = newcap;
    r->head = 0;
}

bool ring_write(ring *r, const void *data, size_t n) {
    if (n == 0) return true;
    size_t need = r->len + n;
    if (r->max_cap && need > r->max_cap) return false;
    if (need > r->cap) ring_grow(r, need);
    size_t tail = (r->head + r->len) & (r->cap - 1);
    size_t first = r->cap - tail;
    if (first > n) first = n;
    memcpy(r->buf + tail, data, first);
    memcpy(r->buf, (const uint8_t *)data + first, n - first);
    r->len = need;
    return true;
}

size_t ring_peek(const ring *r, void *out, size_t n) {
    if (n > r->len) n = r->len;
    size_t first = r->cap - r->head;
    if (first > n) first = n;
    memcpy(out, r->buf + r->head, first);
    memcpy((uint8_t *)out + first, r->buf, n - first);
    return n;
}

size_t ring_read(ring *r, void *out, size_t n) {
    n = ring_peek(r, out, n);
    ring_consume(r, n);
    return n;
}

void ring_consume(ring *r, size_t n) {
    if (n > r->len) n = r->len;
    r->head = (r->head + n) & (r->cap - 1);
    r->len -= n;
    if (r->len == 0) r->head = 0;
}
