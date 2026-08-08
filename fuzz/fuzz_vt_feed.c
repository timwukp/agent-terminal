/* fuzz_vt_feed.c — libFuzzer target for the VT engine.
 *
 * Strategy: derive a geometry and a chunking pattern from the input itself
 * (deterministic — reproducible from the crashing input alone), then feed
 * the rest through vt_feed in randomized chunk sizes with an interleaved
 * resize. Boundary-splitting of UTF-8/escape sequences is the historical
 * terminal-CVE hotspot this specifically exercises.
 *
 * Build: make fuzz BUILD=fuzz CC=clang
 * Run:   build/fuzz/fuzz/fuzz_vt_feed fuzz/corpus/vt -dict=fuzz/dict/vt.dict
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vt/vt.h"

static void cb_response(void *ud, const char *buf, size_t len) {
    (void)ud; (void)buf; (void)len;
}
static void cb_sb(void *ud, const vt_cell *cells, uint16_t n) {
    (void)ud;
    /* Touch every cell so ASan sees any bad pointer. */
    volatile uint32_t sink = 0;
    for (uint16_t i = 0; i < n; i++) sink ^= cells[i].cp;
    (void)sink;
}
static void cb_title(void *ud, const char *utf8, size_t len) {
    (void)ud;
    volatile char sink = 0;
    for (size_t i = 0; i < len; i++) sink ^= utf8[i];
    (void)sink;
}
static void cb_bell(void *ud) { (void)ud; }
static void cb_pass(void *ud, const char *buf, size_t len) {
    (void)ud;
    volatile char sink = 0;
    for (size_t i = 0; i < len; i++) sink ^= buf[i];
    (void)sink;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;

    /* Bytes 0-3 configure the harness; the rest is terminal input. */
    uint16_t rows = (uint16_t)(1 + data[0] % 100);
    uint16_t cols = (uint16_t)(1 + data[1] % 250);
    uint8_t chunk_seed = data[2];
    uint8_t resize_at = data[3];
    data += 4;
    size -= 4;

    vt_callbacks cb = {
        .on_response = cb_response,
        .on_scrollback_line = cb_sb,
        .on_title = cb_title,
        .on_bell = cb_bell,
        .on_passthrough = cb_pass,
    };
    vt *v = vt_new(rows, cols, &cb, NULL);
    if (!v) return 0;

    /* Feed in variable-size chunks; resize partway through. The damage API
     * runs interleaved exactly as a compositor would drive it, putting the
     * bitmap's bounds checks under ASan against arbitrary input. */
    size_t off = 0;
    int chunk_i = 0;
    while (off < size) {
        size_t chunk = 1 + (size_t)((chunk_seed ^ (uint8_t)(chunk_i * 37)) % 31);
        if (chunk > size - off) chunk = size - off;
        vt_feed(v, data + off, chunk);
        off += chunk;
        chunk_i++;
        if (chunk_i == resize_at % 17) {
            /* Mid-stream resize: geometry derived from position. */
            vt_resize(v, (uint16_t)(1 + (rows + chunk_i) % 120),
                      (uint16_t)(1 + (cols + chunk_i * 3) % 300));
        }
        if (vt_any_dirty(v)) {
            uint16_t cur_rows = 0, cur_cols = 0;
            vt_get_size(v, &cur_rows, &cur_cols);
            volatile uint32_t sink = 0;
            for (uint16_t r = 0; r < cur_rows; r++)
                if (vt_row_dirty(v, r)) sink ^= vt_line(v, r)[0].cp;
            (void)sink;
            if (chunk_i % 3 == 0) vt_damage_clear(v);
        }
    }

    /* Snapshot after arbitrary input must be well-formed and re-feedable. */
    char *blob = NULL;
    size_t n = vt_snapshot(v, &blob);
    if (n && blob) {
        vt *w = vt_new(rows, cols, &cb, NULL);
        if (w) {
            vt_feed(w, (const uint8_t *)blob, n);
            vt_free(w);
        }
    }
    free(blob);
    vt_free(v);
    return 0;
}
