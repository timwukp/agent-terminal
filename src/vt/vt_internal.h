/* vt_internal.h — shared between vt_parser/vt_dispatch/vt_screen/vt_render. */
#ifndef AT_VT_INTERNAL_H
#define AT_VT_INTERNAL_H

#include "vt.h"

/* Paul Flo Williams VT500-series parser states (vt100.net). */
typedef enum {
    ST_GROUND,
    ST_ESCAPE,
    ST_ESCAPE_INT,   /* escape intermediate */
    ST_CSI_ENTRY,
    ST_CSI_PARAM,
    ST_CSI_INT,      /* csi intermediate */
    ST_CSI_IGNORE,
    ST_OSC_STRING,
    ST_DCS_ENTRY,
    ST_DCS_PARAM,
    ST_DCS_INT,
    ST_DCS_PASS,     /* dcs passthrough (consumed, not stored) */
    ST_DCS_IGNORE,
    ST_SOSPMAPC,     /* sos/pm/apc string (consumed) */
    ST_MAX
} vt_state;

#define VT_PARAMS_MAX 16
#define VT_INTER_MAX  2
#define VT_OSC_MAX    4096  /* fixed buffer; excess discarded (xterm) */
/* Longest in-progress sequence whose raw bytes are retained for replay. Sized
 * off VT_OSC_MAX, the largest sequence body the parser stores, plus room for
 * the ESC ] introducer, a Ps;Pt prefix and the terminator. */
#define VT_PENDING_MAX (VT_OSC_MAX + 64)
#define VT_ROWS_MAX   1000
#define VT_COLS_MAX   1000
#define VT_TABSTOP_WORDS ((VT_COLS_MAX + 31) / 32)

typedef struct {
    uint32_t fg, bg;
    uint16_t attrs;
} vt_pen;

typedef struct {
    uint16_t row, col;
    vt_pen pen;
    bool pending_wrap; /* xterm deferred wrap at right margin */
    uint8_t charset_idx;     /* 0 = G0, 1 = G1 (via SI/SO) */
    bool charset_graphics[2]; /* per-Gn: DEC Special Graphics active */
    bool origin;             /* saved DECOM (DECSC stores it) */
} vt_cursor;

typedef struct {
    vt_cell *cells; /* rows*cols */
} vt_grid;

struct vt {
    uint16_t rows, cols;

    vt_grid grid[2]; /* [0] primary, [1] alternate */
    int active;      /* which grid renders */
    vt_cursor cur;
    vt_cursor saved_cur[2]; /* DECSC per screen */

    uint32_t modes; /* enum vt_mode bits */
    uint16_t scroll_top, scroll_bot; /* DECSTBM, inclusive, 0-based */
    uint32_t tabstops[VT_TABSTOP_WORDS];

    /* parser */
    vt_state state;
    uint32_t params[VT_PARAMS_MAX];
    bool param_seen[VT_PARAMS_MAX];
    int nparams;
    bool param_overflow;
    char inter[VT_INTER_MAX + 1];
    int ninter;
    char priv; /* CSI private marker: '?', '>', '<', '=' or 0 */
    char osc[VT_OSC_MAX];
    size_t osc_len;
    bool osc_overflow;

    /* utf-8 decoder (Höhrmann DFA) — survives vt_feed boundaries */
    uint32_t u8_state, u8_cp;

    /* Raw bytes of the sequence currently being parsed, for vt_snapshot.
     *
     * A snapshot can be taken at any byte boundary, including halfway through
     * a CSI or a multibyte character. Neither the Williams state machine's
     * state nor the UTF-8 DFA's has an ANSI representation, so rather than
     * serialize either, the snapshot replays the bytes that got us here and
     * lets the receiving parser re-derive both. That keeps the entire mechanism
     * inside the existing vt_feed contract with no new format to fuzz.
     *
     * Reset whenever the parser reaches ground with nothing pending, and
     * capped: past the cap the tail is dropped and `pending_lost` is set, so a
     * snapshot re-feeds nothing rather than a truncated prefix that would
     * decode as a different sequence. */
    uint8_t pending[VT_PENDING_MAX];
    size_t pending_len;
    bool pending_lost;

    /* Cell most recently written by vt_screen_put, which is where a following
     * combining mark attaches. Cannot be derived from the cursor: cur.col has
     * already advanced past the base (by 2 for a wide char), and at the right
     * margin it stays put with pending_wrap set. Lives here rather than in
     * vt_cursor so DECSC/DECRC do not save and restore it — a mark arriving
     * after DECRC must not attach to whatever the cursor was parked on when
     * the save happened. Must survive vt_feed boundaries, since a base and its
     * mark can arrive in separate reads. */
    uint16_t last_row, last_col;
    bool last_valid;

    vt_callbacks cb;
    void *ud;
};

/* vt_utf8.c */
uint32_t vt_utf8_step(uint32_t *state, uint32_t *cp, uint8_t byte);
#define UTF8_ACCEPT 0
#define UTF8_REJECT 12

/* vt_width.c (generated) */
int vt_wcwidth(uint32_t cp);
/* A mark that attaches to a base character. Deliberately NOT the same test as
 * vt_wcwidth(cp) == 0, which also holds for NUL, C0/C1 controls, ZWJ/ZWNJ,
 * variation selectors, BiDi controls and U+FEFF. */
bool vt_is_combining(uint32_t cp);

/* vt_screen.c — grid primitives used by dispatch */
vt_cell *vt_cell_at(vt *v, uint16_t row, uint16_t col);
void vt_screen_put(vt *v, uint32_t cp, int width);
void vt_screen_newline(vt *v);      /* IND/LF with scroll */
void vt_screen_reverse_index(vt *v);
void vt_screen_carriage_return(vt *v);
void vt_screen_backspace(vt *v);
void vt_screen_tab(vt *v);
void vt_screen_erase_display(vt *v, int mode);
void vt_screen_erase_line(vt *v, int mode);
void vt_screen_insert_lines(vt *v, int n);
void vt_screen_delete_lines(vt *v, int n);
void vt_screen_insert_chars(vt *v, int n);
void vt_screen_delete_chars(vt *v, int n);
void vt_screen_erase_chars(vt *v, int n);
void vt_screen_scroll_up(vt *v, int n);
void vt_screen_scroll_down(vt *v, int n);
void vt_screen_move_cursor(vt *v, int row, int col); /* clamped, honors DECOM */
void vt_screen_set_scroll_region(vt *v, int top, int bot);
void vt_screen_switch_alt(vt *v, bool alt, bool save_cursor, bool clear);
void vt_screen_reset(vt *v); /* RIS */
void vt_screen_align_test(vt *v); /* DECALN */

/* vt_dispatch.c — called by the parser on dispatch actions */
void vt_do_execute(vt *v, uint8_t c0);
void vt_do_esc(vt *v, uint8_t final);
void vt_do_csi(vt *v, uint8_t final);
void vt_do_osc(vt *v);
void vt_do_print(vt *v, uint32_t cp);

static inline int vt_param(const vt *v, int i, int dflt) {
    if (i >= v->nparams || !v->param_seen[i]) return dflt;
    return (int)v->params[i];
}

#endif
