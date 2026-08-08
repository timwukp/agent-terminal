#include "composite.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"

/* Small append buffer, same shape as vt_render's. Duplicated rather than
 * exported from libvt: this is daemon code and libvt's buffer is an internal
 * detail of its snapshot serializer. */
typedef struct {
    char *buf;
    size_t len, cap;
    bool oom;
} cbuf;

static void cb_put(cbuf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        while (nc < b->len + n) nc *= 2;
        char *nb = realloc(b->buf, nc);
        if (!nb) { b->oom = true; return; }
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

static void cb_str(cbuf *b, const char *s) { cb_put(b, s, strlen(s)); }

static void cb_fmt(cbuf *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) cb_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void emit_utf8(cbuf *b, uint32_t cp) {
    char u[4];
    if (cp < 0x80) { u[0] = (char)cp; cb_put(b, u, 1); return; }
    if (cp < 0x800) {
        u[0] = (char)(0xc0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3f));
        cb_put(b, u, 2);
        return;
    }
    if (cp < 0x10000) {
        u[0] = (char)(0xe0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        u[2] = (char)(0x80 | (cp & 0x3f));
        cb_put(b, u, 3);
        return;
    }
    u[0] = (char)(0xf0 | (cp >> 18));
    u[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    u[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    u[3] = (char)(0x80 | (cp & 0x3f));
    cb_put(b, u, 4);
}

static void emit_color(cbuf *b, uint32_t color, bool fg) {
    if (color == VT_COLOR_DEFAULT) {
        cb_fmt(b, "\x1b[%dm", fg ? 39 : 49);
    } else if ((color & 0xFF000000u) == 0x01000000u) {
        cb_fmt(b, "\x1b[%d;5;%um", fg ? 38 : 48, color & 0xFFu);
    } else {
        cb_fmt(b, "\x1b[%d;2;%u;%u;%um", fg ? 38 : 48,
               (color >> 16) & 0xFFu, (color >> 8) & 0xFFu, color & 0xFFu);
    }
}

typedef struct {
    uint16_t attrs;
    uint32_t fg, bg;
    bool dirty;
} pen_state;

static const uint16_t attrs_public = (uint16_t)~(VT_ATTR_WIDE | VT_ATTR_WIDE_SPACER);

static void emit_pen(cbuf *b, pen_state *ps, uint16_t attrs, uint32_t fg, uint32_t bg) {
    attrs &= attrs_public;
    if (!ps->dirty && attrs == ps->attrs && fg == ps->fg && bg == ps->bg) return;
    cb_str(b, "\x1b[0m");
    if (attrs & VT_ATTR_BOLD) cb_str(b, "\x1b[1m");
    if (attrs & VT_ATTR_DIM) cb_str(b, "\x1b[2m");
    if (attrs & VT_ATTR_ITALIC) cb_str(b, "\x1b[3m");
    if (attrs & VT_ATTR_UNDERLINE) cb_str(b, "\x1b[4m");
    if (attrs & VT_ATTR_BLINK) cb_str(b, "\x1b[5m");
    if (attrs & VT_ATTR_REVERSE) cb_str(b, "\x1b[7m");
    if (attrs & VT_ATTR_INVISIBLE) cb_str(b, "\x1b[8m");
    if (attrs & VT_ATTR_STRIKE) cb_str(b, "\x1b[9m");
    if (fg != VT_COLOR_DEFAULT) emit_color(b, fg, true);
    if (bg != VT_COLOR_DEFAULT) emit_color(b, bg, false);
    ps->attrs = attrs;
    ps->fg = fg;
    ps->bg = bg;
    ps->dirty = false;
}

bool session_should_composite(const session *s) {
    int n = 0;
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use) n++;
    return n >= 2;
}

/* One pane row, clipped to the pane's rectangle at its composite offset.
 *
 * Three details are load-bearing (see AGENTS.md §composite):
 *  - EL (\x1b[K) is unusable: it erases to the TERMINAL's right margin,
 *    wiping the pane to the right. Trailing blanks are literal spaces
 *    carrying the correct bg, so a row always costs >= pane width bytes.
 *  - The pane's own scroll region / alt screen never reach the client:
 *    vt_line already returns the active grid, and this emits plain cells.
 *  - A wide char whose spacer would cross the pane's right edge cannot be
 *    drawn (half a glyph is not a thing); it renders as a space. */
static void emit_pane_row(cbuf *b, pen_state *ps, const pane *p, uint16_t r) {
    const vt_cell *line = vt_line(p->vt, r);
    if (!line) return;
    uint16_t vrows = 0, vcols = 0;
    vt_get_size(p->vt, &vrows, &vcols);
    uint16_t draw = p->cols < vcols ? p->cols : vcols;

    cb_fmt(b, "\x1b[%u;%uH", (unsigned)(p->y + r + 1), (unsigned)(p->x + 1));
    for (uint16_t c = 0; c < draw; c++) {
        const vt_cell *cell = &line[c];
        if (cell->attrs & VT_ATTR_WIDE_SPACER) continue;
        bool wide = (cell->attrs & VT_ATTR_WIDE) != 0;
        emit_pen(b, ps, cell->attrs, cell->fg, cell->bg);
        if (wide && c + 1 >= draw) {
            cb_put(b, " ", 1); /* lead half would clip: blank it */
            continue;
        }
        emit_utf8(b, cell->cp ? cell->cp : ' ');
        if (cell->comb) emit_utf8(b, cell->comb);
    }
    /* The vt is resized to the pane, so vcols < p->cols only transiently
     * (between reflow and resize); pad with default-bg spaces to keep the
     * rectangle opaque. */
    if (draw < p->cols) {
        emit_pen(b, ps, 0, VT_COLOR_DEFAULT, VT_COLOR_DEFAULT);
        for (uint16_t c = draw; c < p->cols; c++) cb_put(b, " ", 1);
    }
}

static void emit_dividers(cbuf *b, pen_state *ps, const session *s) {
    emit_pen(b, ps, VT_ATTR_DIM, VT_COLOR_DEFAULT, VT_COLOR_DEFAULT);
    for (int i = 0; i < LAYOUT_NODES; i++) {
        const layout_node *n = &s->lt.nodes[i];
        if (!n->in_use || n->leaf) continue;
        const layout_node *a = &s->lt.nodes[n->child[0]];
        if (n->stacked) {
            uint16_t dy = (uint16_t)(a->y + a->rows); /* the divider row */
            cb_fmt(b, "\x1b[%u;%uH", (unsigned)(dy + 1), (unsigned)(n->x + 1));
            for (uint16_t c = 0; c < n->cols; c++) emit_utf8(b, 0x2500); /* ─ */
        } else {
            uint16_t dx = (uint16_t)(a->x + a->cols); /* the divider column */
            for (uint16_t r = 0; r < n->rows; r++) {
                cb_fmt(b, "\x1b[%u;%uH", (unsigned)(n->y + r + 1), (unsigned)(dx + 1));
                emit_utf8(b, 0x2502); /* │ */
            }
        }
    }
}

size_t composite_frame(session *s, bool full, char **out) {
    *out = NULL;
    pane *active = session_active_pane(s);
    if (!active) return 0;

    bool any = full;
    for (int i = 0; i < MAX_PANES_PER_SESSION && !any; i++)
        if (s->panes[i].in_use && s->panes[i].vt && vt_any_dirty(s->panes[i].vt))
            any = true;
    if (!any) return 0;

    cbuf b = {0};
    /* Frame preamble. ?7l for the WHOLE frame: a row ending at the client
     * terminal's last column would wrap and shift every row below (same
     * reasoning as the pager). \x1b[r resets any scroll region a pane app
     * set on the client before compositing began; a pane's DECSTBM is
     * engine-internal and never forwarded. */
    cb_str(&b, "\x1b[?25l\x1b[?7l\x1b[r\x1b[0m");
    if (full) cb_str(&b, "\x1b[2J");

    pen_state ps = {.dirty = true};
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++) {
        pane *p = &s->panes[i];
        if (!p->in_use || !p->vt) continue;
        uint16_t vrows = 0, vcols = 0;
        vt_get_size(p->vt, &vrows, &vcols);
        uint16_t rows = p->rows < vrows ? p->rows : vrows;
        for (uint16_t r = 0; r < rows; r++)
            if (full || vt_row_dirty(p->vt, r))
                emit_pane_row(&b, &ps, p, r);
    }

    if (full) emit_dividers(&b, &ps, s);

    /* Input-affecting modes for the ACTIVE pane only, re-emitted every frame:
     * with one keyboard that is the only coherent choice, and a frame is the
     * natural point where the active pane may have changed. */
    {
        uint32_t m = vt_get_modes(active->vt);
        struct { uint32_t bit; int ps_; } modes[] = {
            {VT_MODE_DECCKM, 1},        {VT_MODE_MOUSE_X10, 1000},
            {VT_MODE_MOUSE_BTN, 1002},  {VT_MODE_MOUSE_ANY, 1003},
            {VT_MODE_FOCUS, 1004},      {VT_MODE_MOUSE_UTF8, 1005},
            {VT_MODE_MOUSE_SGR, 1006},  {VT_MODE_PASTE, 2004},
        };
        for (size_t i = 0; i < sizeof modes / sizeof *modes; i++)
            cb_fmt(&b, "\x1b[?%d%c", modes[i].ps_, (m & modes[i].bit) ? 'h' : 'l');
    }

    /* Active pane's cursor, translated into composite coordinates. */
    {
        uint16_t cr = 0, cc = 0;
        bool vis = false;
        vt_get_cursor(active->vt, &cr, &cc, &vis);
        cb_str(&b, "\x1b[0m");
        cb_fmt(&b, "\x1b[%u;%uH", (unsigned)(active->y + cr + 1),
               (unsigned)(active->x + cc + 1));
        cb_str(&b, vis ? "\x1b[?25h" : "\x1b[?25l");
    }

    if (b.oom) {
        free(b.buf);
        return 0;
    }
    *out = b.buf;
    return b.len;
}
