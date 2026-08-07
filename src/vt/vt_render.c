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

/* The wide-char bookkeeping bits are grid-internal: they describe cell layout,
 * not SGR state, and have no escape sequence. Masked off everywhere a pen is
 * emitted. */
static const uint16_t attrs_public = (uint16_t)~(VT_ATTR_WIDE | VT_ATTR_WIDE_SPACER);

static void render_grid(sb *b, const vt *v) {
    uint16_t attrs_mask = attrs_public;
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

    /* Tabstops. Emitted before any cursor placement below, because HTS sets a
     * stop at wherever the cursor currently is: this walks the cursor across
     * the row to plant each one, and would otherwise destroy the position the
     * snapshot ends by restoring. TBC(3) first so the receiver's defaults
     * (every 8 columns) do not survive as extra stops. */
    {
        bool custom = false;
        for (uint16_t c = 0; c < v->cols; c++) {
            bool set = (v->tabstops[c / 32] & (1u << (c % 32))) != 0;
            if (set != (c != 0 && c % 8 == 0)) { custom = true; break; }
        }
        if (custom) {
            sb_str(&b, "\x1b[3g");
            for (uint16_t c = 0; c < v->cols; c++)
                if (v->tabstops[c / 32] & (1u << (c % 32)))
                    sb_fmt(&b, "\x1b[%u;%uH\x1bH", 1u, c + 1u);
        }
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

    /* Per-Gn charset designation, and which of G0/G1 is currently mapped by
     * SI/SO. Without this a box-drawing TUI restores with its line-drawing
     * characters rendered as raw ASCII (q, x, m) — the app has already sent
     * ESC ( 0 and will not send it again. */
    sb_fmt(&b, "\x1b(%c", v->cur.charset_graphics[0] ? '0' : 'B');
    sb_fmt(&b, "\x1b)%c", v->cur.charset_graphics[1] ? '0' : 'B');
    /* Which Gn is mapped into GL. Designating G1 does not select it; that takes
     * SO/SI, and the app sent those once and will not repeat them. */
    sb_str(&b, v->cur.charset_idx == 1 ? "\x0e" : "\x0f");

    /* Saved-cursor slot for the ACTIVE screen (DECSC), then the live cursor.
     * Emitted as "move there, DECSC, move back": DECSC has no parameterized
     * form, so the only way to load the receiver's save slot is to put the
     * cursor where the save should point. The pen is saved along with the
     * position by DECSC, so it is set here too, then reset — the live pen is
     * re-established afterwards. The inactive screen's slot is not restorable:
     * the receiver has one save slot per screen and reaching the other would
     * mean switching screens, which would repaint. An app that DECRCs on the
     * screen it is not currently on is out of scope, as before. */
    {
        const vt_cursor *sc = &v->saved_cur[v->active];
        bool nondefault = sc->row || sc->col || sc->pen.attrs ||
                          sc->pen.fg != VT_COLOR_DEFAULT ||
                          sc->pen.bg != VT_COLOR_DEFAULT;
        if (nondefault) {
            emit_pen(&b, (uint16_t)(sc->pen.attrs & attrs_public), sc->pen.fg, sc->pen.bg);
            sb_fmt(&b, "\x1b[%u;%uH", sc->row + 1u, sc->col + 1u);
            sb_str(&b, "\x1b\x37"); /* DECSC */
            sb_str(&b, "\x1b[0m");
        }
    }

    /* Current SGR pen, so the next character the app prints has the colour and
     * attributes it set before the snapshot — render_grid left the pen on
     * whatever the last cell used, and the trailing reset cleared even that. */
    emit_pen(&b, (uint16_t)(v->cur.pen.attrs & attrs_public), v->cur.pen.fg, v->cur.pen.bg);

    /* Cursor position + visibility (DECOM affects addressing: place with
     * origin mode as the app left it). */
    if (v->modes & VT_MODE_DECOM)
        sb_fmt(&b, "\x1b[%u;%uH", v->cur.row - v->scroll_top + 1, v->cur.col + 1);
    else
        sb_fmt(&b, "\x1b[%u;%uH", v->cur.row + 1, v->cur.col + 1);

    /* Deferred wrap: the cursor sits on the last column with the next
     * character due to wrap first. Reproduced by printing the cell that is
     * already there again, which re-arms the receiver's own pending_wrap —
     * there is no escape sequence for the flag. Costs one redundant write of a
     * character identical to what the grid holds, so the screen is unchanged. */
    if (v->cur.pending_wrap && (v->modes & VT_MODE_DECAWM)) {
        const vt_cell *line = vt_line(v, v->cur.row);
        const vt_cell *cell = line ? &line[v->cur.col] : NULL;
        if (cell && !(cell->attrs & VT_ATTR_WIDE_SPACER)) {
            emit_pen(&b, (uint16_t)(cell->attrs & attrs_public), cell->fg, cell->bg);
            emit_utf8(&b, cell->cp ? cell->cp : ' ');
            if (cell->comb) emit_utf8(&b, cell->comb);
            emit_pen(&b, (uint16_t)(v->cur.pen.attrs & attrs_public), v->cur.pen.fg,
                     v->cur.pen.bg);
        }
    }

    sb_str(&b, (v->modes & VT_MODE_DECTCEM) ? "\x1b[?25h" : "\x1b[?25l");

    /* Finally, the bytes of a sequence that was still being parsed. Replaying
     * them leaves the receiving parser in the same state mid-sequence, so the
     * rest of the sequence — arriving from the child after the restart —
     * completes correctly. Must be last: everything above is itself escape
     * sequences, which would be swallowed as this one's parameters.
     *
     * Dropped only when the recorder overflowed: a truncated prefix can decode
     * as a different sequence, and losing the tail of one sequence beats
     * corrupting the screen.
     *
     * String states (OSC/DCS/SOS) are replayed like any other. An earlier
     * version excluded them, reasoning that their bodies never touch the grid
     * so replaying a partial title achieved nothing. That was backwards, and a
     * mutation test caught it: without the replay the receiver sits in GROUND,
     * so the remainder of the title arriving from the child is *printed as
     * text* — measured as `hiMORE` on screen where the real engine shows `hi`.
     * The same applies to the user's actual terminal, which must also be left
     * mid-string to swallow the tail. */
    if (v->pending_len && !v->pending_lost)
        sb_put(&b, (const char *)v->pending, v->pending_len);

    if (b.oom) {
        free(b.buf);
        *out = NULL;
        return 0;
    }
    *out = b.buf;
    return b.len;
}
