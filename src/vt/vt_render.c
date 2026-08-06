/* vt_render.c — serialize the grid as an ANSI repaint blob for reattach.
 *
 * Output contract: feeding this blob to a fresh xterm-compatible terminal
 * reproduces the visible screen, cursor position/visibility, and re-arms
 * every tracked mode (alt screen, mouse, bracketed paste, DECAWM, ...). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vt_internal.h"

typedef struct {
    char *buf;
    size_t len, cap;
    bool oom;
} sb;

static void sb_put(sb *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n) nc *= 2;
        char *nb = realloc(b->buf, nc);
        if (!nb) { b->oom = true; return; }
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

static void sb_str(sb *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_fmt(sb *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void emit_utf8(sb *b, uint32_t cp) {
    char u[4];
    if (cp < 0x80) {
        u[0] = (char)cp;
        sb_put(b, u, 1);
    } else if (cp < 0x800) {
        u[0] = (char)(0xc0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3f));
        sb_put(b, u, 2);
    } else if (cp < 0x10000) {
        u[0] = (char)(0xe0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        u[2] = (char)(0x80 | (cp & 0x3f));
        sb_put(b, u, 3);
    } else {
        u[0] = (char)(0xf0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        u[3] = (char)(0x80 | (cp & 0x3f));
        sb_put(b, u, 4);
    }
}

static void emit_color(sb *b, uint32_t color, bool fg) {
    if (color == VT_COLOR_DEFAULT) {
        sb_fmt(b, "\x1b[%dm", fg ? 39 : 49);
    } else if ((color & 0xFF000000u) == 0x01000000u) {
        sb_fmt(b, "\x1b[%d;5;%um", fg ? 38 : 48, color & 0xFFu);
    } else {
        sb_fmt(b, "\x1b[%d;2;%u;%u;%um", fg ? 38 : 48,
               (color >> 16) & 0xFFu, (color >> 8) & 0xFFu, color & 0xFFu);
    }
}

static void emit_pen(sb *b, uint16_t attrs, uint32_t fg, uint32_t bg) {
    sb_str(b, "\x1b[0m");
    if (attrs & VT_ATTR_BOLD) sb_str(b, "\x1b[1m");
    if (attrs & VT_ATTR_DIM) sb_str(b, "\x1b[2m");
    if (attrs & VT_ATTR_ITALIC) sb_str(b, "\x1b[3m");
    if (attrs & VT_ATTR_UNDERLINE) sb_str(b, "\x1b[4m");
    if (attrs & VT_ATTR_BLINK) sb_str(b, "\x1b[5m");
    if (attrs & VT_ATTR_REVERSE) sb_str(b, "\x1b[7m");
    if (attrs & VT_ATTR_INVISIBLE) sb_str(b, "\x1b[8m");
    if (attrs & VT_ATTR_STRIKE) sb_str(b, "\x1b[9m");
    if (fg != VT_COLOR_DEFAULT) emit_color(b, fg, true);
    if (bg != VT_COLOR_DEFAULT) emit_color(b, bg, false);
}

static void render_grid(sb *b, const vt *v) {
    uint16_t attrs_mask = (uint16_t)~(VT_ATTR_WIDE | VT_ATTR_WIDE_SPACER);
    uint16_t cur_attrs = 0;
    uint32_t cur_fg = VT_COLOR_DEFAULT, cur_bg = VT_COLOR_DEFAULT;
    bool pen_dirty = true;

    for (uint16_t r = 0; r < v->rows; r++) {
        sb_fmt(b, "\x1b[%u;1H", r + 1);
        const vt_cell *line = vt_line(v, r);
        for (uint16_t c = 0; c < v->cols; c++) {
            const vt_cell *cell = &line[c];
            if (cell->attrs & VT_ATTR_WIDE_SPACER) continue; /* lead cell drew it */
            uint16_t a = cell->attrs & attrs_mask;
            if (pen_dirty || a != cur_attrs || cell->fg != cur_fg || cell->bg != cur_bg) {
                emit_pen(b, a, cell->fg, cell->bg);
                cur_attrs = a;
                cur_fg = cell->fg;
                cur_bg = cell->bg;
                pen_dirty = false;
            }
            emit_utf8(b, cell->cp ? cell->cp : ' ');
            /* The mark follows its base, which is how a terminal composes the
             * grapheme; it is width-0, so the row still occupies `cols`. */
            if (cell->comb) emit_utf8(b, cell->comb);
        }
    }
    sb_str(b, "\x1b[0m");
}

size_t vt_snapshot(const vt *v, char **out) {
    sb b = {0};

    /* Start from a known state. */
    sb_str(&b, "\x1b[0m\x1b[?25l"); /* hide cursor during repaint */

    bool alt = (v->modes & VT_MODE_ALTSCREEN) != 0;

    if (alt) {
        /* Paint the saved PRIMARY screen first, so leaving the alt screen
         * later shows the right content; then enter alt and paint it. */
        vt tmp = *v;
        tmp.active = 0;
        sb_str(&b, "\x1b[?1049l\x1b[2J");
        render_grid(&b, &tmp);
        sb_str(&b, "\x1b[?1049h\x1b[2J");
        tmp.active = 1;
        render_grid(&b, &tmp);
    } else {
        sb_str(&b, "\x1b[?1049l\x1b[2J");
        render_grid(&b, v);
    }

    /* Scroll region. */
    if (v->scroll_top != 0 || v->scroll_bot != v->rows - 1)
        sb_fmt(&b, "\x1b[%u;%ur", v->scroll_top + 1, v->scroll_bot + 1);
    else
        sb_str(&b, "\x1b[r");

    /* Re-arm tracked modes. */
    struct { uint32_t bit; int ps; } modes[] = {
        {VT_MODE_DECCKM, 1},     {VT_MODE_DECOM, 6},      {VT_MODE_MOUSE_X10, 1000},
        {VT_MODE_MOUSE_BTN, 1002}, {VT_MODE_MOUSE_ANY, 1003}, {VT_MODE_FOCUS, 1004},
        {VT_MODE_MOUSE_UTF8, 1005}, {VT_MODE_MOUSE_SGR, 1006}, {VT_MODE_PASTE, 2004},
    };
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++)
        sb_fmt(&b, "\x1b[?%d%c", modes[i].ps, (v->modes & modes[i].bit) ? 'h' : 'l');
    /* DECAWM default is on; only emit when off. */
    if (!(v->modes & VT_MODE_DECAWM)) sb_str(&b, "\x1b[?7l");
    else sb_str(&b, "\x1b[?7h");
    if (v->modes & VT_MODE_IRM) sb_str(&b, "\x1b[4h");

    /* Cursor position + visibility (DECOM affects addressing: place with
     * origin mode as the app left it). */
    if (v->modes & VT_MODE_DECOM)
        sb_fmt(&b, "\x1b[%u;%uH", v->cur.row - v->scroll_top + 1, v->cur.col + 1);
    else
        sb_fmt(&b, "\x1b[%u;%uH", v->cur.row + 1, v->cur.col + 1);
    sb_str(&b, (v->modes & VT_MODE_DECTCEM) ? "\x1b[?25h" : "\x1b[?25l");

    if (b.oom) {
        free(b.buf);
        *out = NULL;
        return 0;
    }
    *out = b.buf;
    return b.len;
}
