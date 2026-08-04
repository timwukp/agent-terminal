/* vt.h — public API of libvt, the terminal-emulation engine.
 *
 * ISOLATION CONTRACT: libvt performs no I/O and no syscalls, and never
 * allocates proportionally to untrusted input. All effects flow through
 * vt_callbacks. Any byte sequence fed to vt_feed() is defined behavior.
 * This is what makes the untrusted-input surface fuzzable in isolation. */
#ifndef AT_VT_H
#define AT_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cell color encoding: 0 = default, 0x01000000|idx = indexed 0..255,
 * 0x02000000|0xRRGGBB = truecolor. */
#define VT_COLOR_DEFAULT  0u
#define VT_COLOR_IDX(n)   (0x01000000u | (uint32_t)(n))
#define VT_COLOR_RGB(rgb) (0x02000000u | ((rgb) & 0xFFFFFFu))

enum vt_attr {
    VT_ATTR_BOLD        = 1 << 0,
    VT_ATTR_DIM         = 1 << 1,
    VT_ATTR_ITALIC      = 1 << 2,
    VT_ATTR_UNDERLINE   = 1 << 3,
    VT_ATTR_BLINK       = 1 << 4,
    VT_ATTR_REVERSE     = 1 << 5,
    VT_ATTR_INVISIBLE   = 1 << 6,
    VT_ATTR_STRIKE      = 1 << 7,
    VT_ATTR_WIDE        = 1 << 8, /* lead cell of a double-width char */
    VT_ATTR_WIDE_SPACER = 1 << 9, /* trailing half of a double-width char */
};

typedef struct {
    uint32_t cp; /* base codepoint; 0 = empty cell */
    uint32_t fg, bg;
    uint16_t attrs;
} vt_cell;

/* Tracked DEC private modes — re-asserted by vt_snapshot() on reattach. */
enum vt_mode {
    VT_MODE_DECCKM     = 1 << 0,  /* ?1  application cursor keys */
    VT_MODE_DECOM      = 1 << 1,  /* ?6  origin mode */
    VT_MODE_DECAWM     = 1 << 2,  /* ?7  autowrap */
    VT_MODE_DECTCEM    = 1 << 3,  /* ?25 cursor visible */
    VT_MODE_ALTSCREEN  = 1 << 4,  /* ?47/?1047/?1049 */
    VT_MODE_MOUSE_X10  = 1 << 5,  /* ?1000 */
    VT_MODE_MOUSE_BTN  = 1 << 6,  /* ?1002 */
    VT_MODE_MOUSE_ANY  = 1 << 7,  /* ?1003 */
    VT_MODE_FOCUS      = 1 << 8,  /* ?1004 */
    VT_MODE_MOUSE_UTF8 = 1 << 9,  /* ?1005 */
    VT_MODE_MOUSE_SGR  = 1 << 10, /* ?1006 */
    VT_MODE_PASTE      = 1 << 11, /* ?2004 bracketed paste */
    VT_MODE_IRM        = 1 << 12, /* insert mode (CSI 4 h) */
};

typedef struct {
    /* Query responses (DA/DSR/CPR/DECRQM) destined for the PTY master. */
    void (*on_response)(void *ud, const char *buf, size_t len);
    /* A line scrolled off the top of the primary screen (never the alt
     * screen). cells borrows engine memory; copy before returning. */
    void (*on_scrollback_line)(void *ud, const vt_cell *cells, uint16_t n);
    /* OSC 0/2 window title (also mirrored to on_passthrough). */
    void (*on_title)(void *ud, const char *utf8, size_t len);
    void (*on_bell)(void *ud);
    /* Sequences we track-but-forward (OSC 52 clipboard, title). */
    void (*on_passthrough)(void *ud, const char *buf, size_t len);
} vt_callbacks;

typedef struct vt vt;

/* Any callback may be NULL. Returns NULL on allocation failure. */
vt *vt_new(uint16_t rows, uint16_t cols, const vt_callbacks *cb, void *ud);
void vt_free(vt *v);

/* Feed child output. Total: never fails, any input is defined behavior. */
void vt_feed(vt *v, const uint8_t *data, size_t len);

/* Resize grid; content in the overlapping region is preserved (no reflow). */
void vt_resize(vt *v, uint16_t rows, uint16_t cols);

/* Serialize visible grid + cursor + modes as an ANSI repaint blob for a
 * freshly attached client. *out is malloc'd; caller frees. Returns length,
 * 0 on allocation failure. */
size_t vt_snapshot(const vt *v, char **out);

/* Inspection (tests, vtdump). Row is 0-based; returns NULL if out of range. */
const vt_cell *vt_line(const vt *v, uint16_t row);
void vt_get_cursor(const vt *v, uint16_t *row, uint16_t *col, bool *visible);
uint32_t vt_get_modes(const vt *v);
void vt_get_size(const vt *v, uint16_t *rows, uint16_t *cols);

#endif
