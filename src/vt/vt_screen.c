/* vt_screen.c — grid state: cursor, scroll regions, erase/insert/delete,
 * alt screen. All coordinates are clamped; no operation can index outside
 * the grid regardless of parser input. */
#include <stdlib.h>
#include <string.h>

#include "vt_internal.h"

static vt_cell blank_cell(const vt *v) {
    /* Erased cells keep the current background (BCE, xterm behavior).
     * Designated initializers zero .comb, so erasing drops any mark. */
    return (vt_cell){.cp = 0, .fg = VT_COLOR_DEFAULT, .bg = v->cur.pen.bg, .attrs = 0};
}

/* Forget where a combining mark would attach. Called by every primitive that
 * moves the cursor or relocates cells, so a mark can only ever attach to a
 * base printed immediately before it. Erring toward invalidation drops a mark;
 * failing to invalidate would attach one to an unrelated cell, or to a cell
 * that has since been scrolled or shifted elsewhere. */
static void forget_last(vt *v) { v->last_valid = false; }

vt_cell *vt_cell_at(vt *v, uint16_t row, uint16_t col) {
    if (row >= v->rows || col >= v->cols) return NULL;
    return &v->grid[v->active].cells[(size_t)row * v->cols + col];
}

static void clear_row(vt *v, uint16_t row, uint16_t from, uint16_t to /*exclusive*/) {
    if (row >= v->rows) return;
    if (to > v->cols) to = v->cols;
    vt_cell b = blank_cell(v);
    for (uint16_t c = from; c < to; c++)
        *vt_cell_at(v, row, c) = b;
}

/* Blank both halves if either half of a wide char is overwritten. */
static void unsplit_wide(vt *v, uint16_t row, uint16_t col) {
    vt_cell *c = vt_cell_at(v, row, col);
    if (!c) return;
    if ((c->attrs & VT_ATTR_WIDE_SPACER) && col > 0) {
        vt_cell *lead = vt_cell_at(v, row, (uint16_t)(col - 1));
        if (lead && (lead->attrs & VT_ATTR_WIDE)) {
            lead->cp = 0; lead->attrs = 0; lead->comb = 0;
        }
    }
    if ((c->attrs & VT_ATTR_WIDE) && col + 1 < v->cols) {
        vt_cell *sp = vt_cell_at(v, row, (uint16_t)(col + 1));
        if (sp && (sp->attrs & VT_ATTR_WIDE_SPACER)) {
            sp->cp = 0; sp->attrs = 0; sp->comb = 0;
        }
    }
}

/* ---- scrolling ---- */

static void scroll_region_up(vt *v, uint16_t top, uint16_t bot, int n) {
    if (n <= 0) return;
    forget_last(v);
    int height = bot - top + 1;
    if (n > height) n = height;
    /* Lines leaving the top of the PRIMARY screen's full-height region go
     * to scrollback. */
    if (v->active == 0 && top == 0 && v->cb.on_scrollback_line)
        for (int i = 0; i < n; i++)
            v->cb.on_scrollback_line(v->ud, vt_cell_at(v, (uint16_t)i, 0), v->cols);
    for (int r = top; r + n <= bot; r++)
        memcpy(vt_cell_at(v, (uint16_t)r, 0), vt_cell_at(v, (uint16_t)(r + n), 0),
               (size_t)v->cols * sizeof(vt_cell));
    for (int r = bot - n + 1; r <= bot; r++)
        clear_row(v, (uint16_t)r, 0, v->cols);
}

static void scroll_region_down(vt *v, uint16_t top, uint16_t bot, int n) {
    if (n <= 0) return;
    forget_last(v);
    int height = bot - top + 1;
    if (n > height) n = height;
    for (int r = bot; r - n >= top; r--)
        memcpy(vt_cell_at(v, (uint16_t)r, 0), vt_cell_at(v, (uint16_t)(r - n), 0),
               (size_t)v->cols * sizeof(vt_cell));
    for (int r = top; r < top + n; r++)
        clear_row(v, (uint16_t)r, 0, v->cols);
}

void vt_screen_scroll_up(vt *v, int n)   { scroll_region_up(v, v->scroll_top, v->scroll_bot, n); }
void vt_screen_scroll_down(vt *v, int n) { scroll_region_down(v, v->scroll_top, v->scroll_bot, n); }

/* ---- cursor movement ---- */

void vt_screen_move_cursor(vt *v, int row, int col) {
    int top = 0, bot = v->rows - 1;
    if (v->modes & VT_MODE_DECOM) { top = v->scroll_top; bot = v->scroll_bot; row += top; }
    if (row < top) row = top;
    if (row > bot) row = bot;
    if (col < 0) col = 0;
    if (col >= v->cols) col = v->cols - 1;
    v->cur.row = (uint16_t)row;
    v->cur.col = (uint16_t)col;
    v->cur.pending_wrap = false;
    forget_last(v);
}

void vt_screen_carriage_return(vt *v) {
    v->cur.col = 0;
    v->cur.pending_wrap = false;
    forget_last(v);
}

void vt_screen_backspace(vt *v) {
    forget_last(v);
    if (v->cur.pending_wrap) { v->cur.pending_wrap = false; return; }
    if (v->cur.col > 0) v->cur.col--;
}

void vt_screen_newline(vt *v) {
    forget_last(v);
    v->cur.pending_wrap = false;
    if (v->cur.row == v->scroll_bot) {
        scroll_region_up(v, v->scroll_top, v->scroll_bot, 1);
    } else if (v->cur.row + 1 < v->rows) {
        v->cur.row++;
    }
}

void vt_screen_reverse_index(vt *v) {
    forget_last(v);
    v->cur.pending_wrap = false;
    if (v->cur.row == v->scroll_top) {
        scroll_region_down(v, v->scroll_top, v->scroll_bot, 1);
    } else if (v->cur.row > 0) {
        v->cur.row--;
    }
}

void vt_screen_tab(vt *v) {
    forget_last(v);
    v->cur.pending_wrap = false;
    for (uint16_t c = v->cur.col + 1; c < v->cols; c++) {
        if (v->tabstops[c / 32] & (1u << (c % 32))) { v->cur.col = c; return; }
    }
    v->cur.col = (uint16_t)(v->cols - 1);
}

/* ---- printing ---- */

/* Attach a combining mark to the cell vt_screen_put wrote last. Returns
 * without effect if there is no such cell, if the mark is outside the BMP, or
 * if that cell already carries one — each of those drops the mark, which is
 * what v1 did with every mark. */
static void attach_combining(vt *v, uint32_t cp) {
    if (!v->last_valid || cp > 0xFFFF) return;
    vt_cell *cell = vt_cell_at(v, v->last_row, v->last_col);
    if (!cell || cell->cp == 0 || cell->comb != 0) return;
    cell->comb = (uint16_t)cp;
}

void vt_screen_put(vt *v, uint32_t cp, int width) {
    if (width <= 0) {
        /* A mark attaches to the preceding base; anything else zero-width
         * (NUL, C0/C1, ZWJ/ZWNJ, variation selectors, BiDi controls, U+FEFF)
         * still occupies no cell and is dropped. Note this must not call
         * forget_last: a dropped format control between a base and its mark
         * would otherwise detach the mark from a base still on screen. */
        if (vt_is_combining(cp)) attach_combining(v, cp);
        return;
    }

    if (v->cur.pending_wrap && (v->modes & VT_MODE_DECAWM)) {
        vt_screen_carriage_return(v);
        vt_screen_newline(v);
    }

    /* Wide char that doesn't fit at the margin: wrap first (xterm). */
    if (width == 2 && v->cur.col + 1 >= v->cols) {
        if (v->modes & VT_MODE_DECAWM) {
            clear_row(v, v->cur.row, v->cur.col, v->cols);
            vt_screen_carriage_return(v);
            vt_screen_newline(v);
        } else if (v->cols >= 2) {
            v->cur.col = (uint16_t)(v->cols - 2);
        } else {
            return; /* 1-column grid cannot hold a wide char */
        }
    }

    if (v->modes & VT_MODE_IRM) vt_screen_insert_chars(v, width);

    unsplit_wide(v, v->cur.row, v->cur.col);
    vt_cell *cell = vt_cell_at(v, v->cur.row, v->cur.col);
    if (!cell) return;
    cell->cp = cp;
    cell->fg = v->cur.pen.fg;
    cell->bg = v->cur.pen.bg;
    cell->attrs = (uint16_t)(v->cur.pen.attrs | (width == 2 ? VT_ATTR_WIDE : 0));
    cell->comb = 0; /* overwriting a base drops the mark it used to carry */

    /* Remember the base for a mark that may follow. Recorded before the cursor
     * advances, because after a wide char or at the right margin the cursor no
     * longer identifies this cell. */
    v->last_row = v->cur.row;
    v->last_col = v->cur.col;
    v->last_valid = true;

    if (width == 2) {
        unsplit_wide(v, v->cur.row, (uint16_t)(v->cur.col + 1));
        vt_cell *sp = vt_cell_at(v, v->cur.row, (uint16_t)(v->cur.col + 1));
        if (sp) {
            sp->cp = 0;
            sp->fg = v->cur.pen.fg;
            sp->bg = v->cur.pen.bg;
            sp->attrs = VT_ATTR_WIDE_SPACER;
            sp->comb = 0; /* the mark lives on the lead cell, never the spacer */
        }
    }

    uint16_t last = (uint16_t)(v->cols - 1);
    if (v->cur.col + width - 1 >= last) {
        v->cur.col = last;
        v->cur.pending_wrap = true;
    } else {
        v->cur.col = (uint16_t)(v->cur.col + width);
    }
}

/* ---- erase / insert / delete ---- */

void vt_screen_erase_display(vt *v, int mode) {
    forget_last(v);
    v->cur.pending_wrap = false;
    switch (mode) {
    case 0: /* cursor to end */
        clear_row(v, v->cur.row, v->cur.col, v->cols);
        for (uint16_t r = (uint16_t)(v->cur.row + 1); r < v->rows; r++)
            clear_row(v, r, 0, v->cols);
        break;
    case 1: /* start to cursor */
        for (uint16_t r = 0; r < v->cur.row; r++)
            clear_row(v, r, 0, v->cols);
        clear_row(v, v->cur.row, 0, (uint16_t)(v->cur.col + 1));
        break;
    case 2: /* all */
    case 3: /* all + scrollback (ring clear is the daemon's concern) */
        for (uint16_t r = 0; r < v->rows; r++)
            clear_row(v, r, 0, v->cols);
        break;
    default: break;
    }
}

void vt_screen_erase_line(vt *v, int mode) {
    forget_last(v);
    v->cur.pending_wrap = false;
    switch (mode) {
    case 0: clear_row(v, v->cur.row, v->cur.col, v->cols); break;
    case 1: clear_row(v, v->cur.row, 0, (uint16_t)(v->cur.col + 1)); break;
    case 2: clear_row(v, v->cur.row, 0, v->cols); break;
    default: break;
    }
}

void vt_screen_insert_lines(vt *v, int n) {
    if (v->cur.row < v->scroll_top || v->cur.row > v->scroll_bot) return;
    scroll_region_down(v, v->cur.row, v->scroll_bot, n);
    v->cur.pending_wrap = false;
}

void vt_screen_delete_lines(vt *v, int n) {
    if (v->cur.row < v->scroll_top || v->cur.row > v->scroll_bot) return;
    scroll_region_up(v, v->cur.row, v->scroll_bot, n);
    v->cur.pending_wrap = false;
}

void vt_screen_insert_chars(vt *v, int n) {
    if (n <= 0) return;
    forget_last(v);
    int avail = v->cols - v->cur.col;
    if (n > avail) n = avail;
    vt_cell *row = vt_cell_at(v, v->cur.row, 0);
    memmove(row + v->cur.col + n, row + v->cur.col,
            (size_t)(avail - n) * sizeof(vt_cell));
    clear_row(v, v->cur.row, v->cur.col, (uint16_t)(v->cur.col + n));
    v->cur.pending_wrap = false;
}

void vt_screen_delete_chars(vt *v, int n) {
    if (n <= 0) return;
    forget_last(v);
    int avail = v->cols - v->cur.col;
    if (n > avail) n = avail;
    vt_cell *row = vt_cell_at(v, v->cur.row, 0);
    memmove(row + v->cur.col, row + v->cur.col + n,
            (size_t)(avail - n) * sizeof(vt_cell));
    clear_row(v, v->cur.row, (uint16_t)(v->cols - n), v->cols);
    v->cur.pending_wrap = false;
}

void vt_screen_erase_chars(vt *v, int n) {
    if (n <= 0) return;
    forget_last(v);
    int end = v->cur.col + n;
    if (end > v->cols) end = v->cols;
    clear_row(v, v->cur.row, v->cur.col, (uint16_t)end);
    v->cur.pending_wrap = false;
}

/* ---- regions, alt screen, reset ---- */

void vt_screen_set_scroll_region(vt *v, int top, int bot) {
    /* 1-based params; 0 means default. */
    if (top <= 0) top = 1;
    if (bot <= 0 || bot > v->rows) bot = v->rows;
    if (top >= bot) return; /* invalid, ignore (xterm) */
    v->scroll_top = (uint16_t)(top - 1);
    v->scroll_bot = (uint16_t)(bot - 1);
    vt_screen_move_cursor(v, 0, 0); /* home, honoring DECOM */
}

void vt_screen_switch_alt(vt *v, bool alt, bool save_cursor, bool clear) {
    bool cur_alt = (v->modes & VT_MODE_ALTSCREEN) != 0;
    if (alt == cur_alt) return;
    forget_last(v);
    if (alt) {
        if (save_cursor) v->saved_cur[0] = v->cur;
        v->active = 1;
        v->modes |= VT_MODE_ALTSCREEN;
        if (clear)
            for (uint16_t r = 0; r < v->rows; r++)
                clear_row(v, r, 0, v->cols);
    } else {
        v->active = 0;
        v->modes &= ~(uint32_t)VT_MODE_ALTSCREEN;
        if (save_cursor) { v->cur = v->saved_cur[0]; }
    }
    v->cur.pending_wrap = false;
}

static void reset_tabstops(vt *v) {
    memset(v->tabstops, 0, sizeof v->tabstops);
    for (uint16_t c = 8; c < v->cols; c = (uint16_t)(c + 8))
        v->tabstops[c / 32] |= 1u << (c % 32);
}

void vt_screen_reset(vt *v) {
    forget_last(v);
    v->modes = VT_MODE_DECAWM | VT_MODE_DECTCEM;
    v->active = 0;
    v->scroll_top = 0;
    v->scroll_bot = (uint16_t)(v->rows - 1);
    memset(&v->cur, 0, sizeof v->cur);
    v->cur.pen = (vt_pen){.fg = VT_COLOR_DEFAULT, .bg = VT_COLOR_DEFAULT, .attrs = 0};
    v->saved_cur[0] = v->saved_cur[1] = v->cur;
    reset_tabstops(v);
    for (int g = 0; g < 2; g++)
        for (uint16_t r = 0; r < v->rows; r++)
            memset(&v->grid[g].cells[(size_t)r * v->cols], 0,
                   (size_t)v->cols * sizeof(vt_cell));
}

void vt_screen_align_test(vt *v) { /* DECALN: fill with E */
    forget_last(v);
    vt_pen saved = v->cur.pen;
    v->cur.pen = (vt_pen){.fg = VT_COLOR_DEFAULT, .bg = VT_COLOR_DEFAULT, .attrs = 0};
    for (uint16_t r = 0; r < v->rows; r++)
        for (uint16_t c = 0; c < v->cols; c++)
            *vt_cell_at(v, r, c) = (vt_cell){.cp = 'E', .fg = VT_COLOR_DEFAULT,
                                             .bg = VT_COLOR_DEFAULT, .attrs = 0,
                                             .comb = 0};
    v->cur.pen = saved;
    v->scroll_top = 0;
    v->scroll_bot = (uint16_t)(v->rows - 1);
    vt_screen_move_cursor(v, 0, 0);
}

/* ---- lifecycle ---- */

vt *vt_new(uint16_t rows, uint16_t cols, const vt_callbacks *cb, void *ud) {
    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    if (rows > VT_ROWS_MAX) rows = VT_ROWS_MAX;
    if (cols > VT_COLS_MAX) cols = VT_COLS_MAX;

    vt *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->rows = rows;
    v->cols = cols;
    for (int g = 0; g < 2; g++) {
        v->grid[g].cells = calloc((size_t)rows * cols, sizeof(vt_cell));
        if (!v->grid[g].cells) {
            free(v->grid[0].cells);
            free(v);
            return NULL;
        }
    }
    if (cb) v->cb = *cb;
    v->ud = ud;
    v->state = ST_GROUND;
    vt_screen_reset(v);
    return v;
}

void vt_free(vt *v) {
    if (!v) return;
    free(v->grid[0].cells);
    free(v->grid[1].cells);
    free(v);
}

void vt_resize(vt *v, uint16_t rows, uint16_t cols) {
    if (rows == 0) rows = 1;
    if (cols == 0) cols = 1;
    if (rows > VT_ROWS_MAX) rows = VT_ROWS_MAX;
    if (cols > VT_COLS_MAX) cols = VT_COLS_MAX;
    if (rows == v->rows && cols == v->cols) return;

    for (int g = 0; g < 2; g++) {
        vt_cell *nc = calloc((size_t)rows * cols, sizeof(vt_cell));
        if (!nc) return; /* keep old grid; resize is best-effort */
        uint16_t copy_rows = rows < v->rows ? rows : v->rows;
        uint16_t copy_cols = cols < v->cols ? cols : v->cols;
        for (uint16_t r = 0; r < copy_rows; r++)
            memcpy(nc + (size_t)r * cols,
                   v->grid[g].cells + (size_t)r * v->cols,
                   (size_t)copy_cols * sizeof(vt_cell));
        free(v->grid[g].cells);
        v->grid[g].cells = nc;
    }
    v->rows = rows;
    v->cols = cols;
    v->scroll_top = 0;
    v->scroll_bot = (uint16_t)(rows - 1);
    if (v->cur.row >= rows) v->cur.row = (uint16_t)(rows - 1);
    if (v->cur.col >= cols) v->cur.col = (uint16_t)(cols - 1);
    for (int i = 0; i < 2; i++) {
        if (v->saved_cur[i].row >= rows) v->saved_cur[i].row = (uint16_t)(rows - 1);
        if (v->saved_cur[i].col >= cols) v->saved_cur[i].col = (uint16_t)(cols - 1);
    }
    v->cur.pending_wrap = false;
    forget_last(v);
    reset_tabstops(v);
}

/* ---- inspection ---- */

const vt_cell *vt_line(const vt *v, uint16_t row) {
    if (row >= v->rows) return NULL;
    return &v->grid[v->active].cells[(size_t)row * v->cols];
}

void vt_get_cursor(const vt *v, uint16_t *row, uint16_t *col, bool *visible) {
    if (row) *row = v->cur.row;
    if (col) *col = v->cur.col;
    if (visible) *visible = (v->modes & VT_MODE_DECTCEM) != 0;
}

uint32_t vt_get_modes(const vt *v) { return v->modes; }

void vt_get_size(const vt *v, uint16_t *rows, uint16_t *cols) {
    if (rows) *rows = v->rows;
    if (cols) *cols = v->cols;
}
