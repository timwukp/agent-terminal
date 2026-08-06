/* vt_dispatch.c — ESC/CSI/OSC dispatch: sequences → screen operations.
 * Unknown finals parse to no-ops; unknown never means undefined. */
#include <stdio.h>
#include <string.h>

#include "vt_internal.h"

/* DEC Special Graphics (ESC ( 0): 0x60..0x7e map to line-drawing glyphs. */
static const uint16_t dec_graphics[31] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,
    0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
    0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,
    0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

void vt_do_print(vt *v, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    /* C1 codepoints arriving via UTF-8: only ST/CSI-alias semantics matter
     * and real apps use 7-bit forms; render as replacement. */
    if (cp >= 0x80 && cp <= 0x9F) return;
    if (v->cur.charset_graphics[v->cur.charset_idx] && cp >= 0x60 && cp <= 0x7e)
        cp = dec_graphics[cp - 0x60];
    vt_screen_put(v, cp, vt_wcwidth(cp));
}

void vt_do_execute(vt *v, uint8_t c0) {
    switch (c0) {
    case 0x05: /* ENQ */ break;
    case 0x07: if (v->cb.on_bell) v->cb.on_bell(v->ud); break;
    case 0x08: vt_screen_backspace(v); break;
    case 0x09: vt_screen_tab(v); break;
    case 0x0a: case 0x0b: case 0x0c: vt_screen_newline(v); break;
    case 0x0d: vt_screen_carriage_return(v); break;
    case 0x0e: v->cur.charset_idx = 1; break; /* SO → G1 */
    case 0x0f: v->cur.charset_idx = 0; break; /* SI → G0 */
    default: break;
    }
}

static void save_cursor(vt *v) {
    int idx = v->active;
    v->saved_cur[idx] = v->cur;
    v->saved_cur[idx].origin = (v->modes & VT_MODE_DECOM) != 0;
}

static void restore_cursor(vt *v) {
    int idx = v->active;
    v->cur = v->saved_cur[idx];
    if (v->cur.row >= v->rows) v->cur.row = (uint16_t)(v->rows - 1);
    if (v->cur.col >= v->cols) v->cur.col = (uint16_t)(v->cols - 1);
    if (v->cur.origin) v->modes |= VT_MODE_DECOM;
    else v->modes &= ~(uint32_t)VT_MODE_DECOM;
    v->cur.pending_wrap = false;
}

void vt_do_esc(vt *v, uint8_t final) {
    if (v->ninter == 1) {
        char i0 = v->inter[0];
        if (i0 == '(' || i0 == ')') {
            int g = i0 == '(' ? 0 : 1;
            if (final == '0') v->cur.charset_graphics[g] = true;
            else v->cur.charset_graphics[g] = false; /* 'B' and others → US-ASCII */
            return;
        }
        if (i0 == '#' && final == '8') { vt_screen_align_test(v); return; }
        return;
    }
    if (v->ninter != 0) return;

    switch (final) {
    case '7': save_cursor(v); break;              /* DECSC */
    case '8': restore_cursor(v); break;           /* DECRC */
    case 'D': vt_screen_newline(v); break;        /* IND */
    case 'E': vt_screen_carriage_return(v); vt_screen_newline(v); break; /* NEL */
    case 'M': vt_screen_reverse_index(v); break;  /* RI */
    case 'H': /* HTS */
        v->tabstops[v->cur.col / 32] |= 1u << (v->cur.col % 32);
        break;
    case 'c': vt_screen_reset(v); break;          /* RIS */
    /* '=' / '>' (DECKPAM/DECKPNM) need no state: the client sends real key
     * bytes either way. They fall through to the no-op default. */
    default: break;
    }
}

/* ---- SGR ---- */

static void do_sgr(vt *v) {
    if (v->nparams == 0) { /* CSI m == CSI 0 m */
        v->cur.pen = (vt_pen){.fg = VT_COLOR_DEFAULT, .bg = VT_COLOR_DEFAULT, .attrs = 0};
        return;
    }
    for (int i = 0; i < v->nparams; i++) {
        int p = vt_param(v, i, 0);
        switch (p) {
        case 0: v->cur.pen = (vt_pen){.fg = VT_COLOR_DEFAULT, .bg = VT_COLOR_DEFAULT, .attrs = 0}; break;
        case 1: v->cur.pen.attrs |= VT_ATTR_BOLD; break;
        case 2: v->cur.pen.attrs |= VT_ATTR_DIM; break;
        case 3: v->cur.pen.attrs |= VT_ATTR_ITALIC; break;
        case 4: v->cur.pen.attrs |= VT_ATTR_UNDERLINE; break;
        case 5: case 6: v->cur.pen.attrs |= VT_ATTR_BLINK; break;
        case 7: v->cur.pen.attrs |= VT_ATTR_REVERSE; break;
        case 8: v->cur.pen.attrs |= VT_ATTR_INVISIBLE; break;
        case 9: v->cur.pen.attrs |= VT_ATTR_STRIKE; break;
        case 21: case 22: v->cur.pen.attrs &= (uint16_t)~(VT_ATTR_BOLD | VT_ATTR_DIM); break;
        case 23: v->cur.pen.attrs &= (uint16_t)~VT_ATTR_ITALIC; break;
        case 24: v->cur.pen.attrs &= (uint16_t)~VT_ATTR_UNDERLINE; break;
        case 25: v->cur.pen.attrs &= (uint16_t)~VT_ATTR_BLINK; break;
        case 27: v->cur.pen.attrs &= (uint16_t)~VT_ATTR_REVERSE; break;
        case 28: v->cur.pen.attrs &= (uint16_t)~VT_ATTR_INVISIBLE; break;
        case 29: v->cur.pen.attrs &= (uint16_t)~VT_ATTR_STRIKE; break;
        case 39: v->cur.pen.fg = VT_COLOR_DEFAULT; break;
        case 49: v->cur.pen.bg = VT_COLOR_DEFAULT; break;
        case 38: case 48: {
            /* 38;5;n / 48;5;n indexed — 38;2;r;g;b truecolor */
            uint32_t *dst = p == 38 ? &v->cur.pen.fg : &v->cur.pen.bg;
            int kind = vt_param(v, i + 1, -1);
            if (kind == 5 && i + 2 < v->nparams) {
                *dst = VT_COLOR_IDX(vt_param(v, i + 2, 0) & 0xff);
                i += 2;
            } else if (kind == 2 && i + 4 < v->nparams) {
                uint32_t r = (uint32_t)vt_param(v, i + 2, 0) & 0xff;
                uint32_t g = (uint32_t)vt_param(v, i + 3, 0) & 0xff;
                uint32_t b = (uint32_t)vt_param(v, i + 4, 0) & 0xff;
                *dst = VT_COLOR_RGB(r << 16 | g << 8 | b);
                i += 4;
            } else {
                return; /* malformed extended color: stop processing */
            }
            break;
        }
        default:
            if (p >= 30 && p <= 37) v->cur.pen.fg = VT_COLOR_IDX(p - 30);
            else if (p >= 40 && p <= 47) v->cur.pen.bg = VT_COLOR_IDX(p - 40);
            else if (p >= 90 && p <= 97) v->cur.pen.fg = VT_COLOR_IDX(p - 90 + 8);
            else if (p >= 100 && p <= 107) v->cur.pen.bg = VT_COLOR_IDX(p - 100 + 8);
            break;
        }
    }
}

/* ---- DEC private modes ---- */

static void set_mode_bit(vt *v, uint32_t bit, bool on) {
    if (on) v->modes |= bit;
    else v->modes &= ~bit;
}

static void do_decset(vt *v, bool on) {
    for (int i = 0; i < v->nparams; i++) {
        switch (vt_param(v, i, 0)) {
        case 1:    set_mode_bit(v, VT_MODE_DECCKM, on); break;
        case 6:
            set_mode_bit(v, VT_MODE_DECOM, on);
            vt_screen_move_cursor(v, 0, 0);
            break;
        case 7:    set_mode_bit(v, VT_MODE_DECAWM, on); break;
        case 12:   break; /* cursor blink: presentation-only */
        case 25:   set_mode_bit(v, VT_MODE_DECTCEM, on); break;
        case 47:   vt_screen_switch_alt(v, on, false, false); break;
        case 1000: set_mode_bit(v, VT_MODE_MOUSE_X10, on); break;
        case 1002: set_mode_bit(v, VT_MODE_MOUSE_BTN, on); break;
        case 1003: set_mode_bit(v, VT_MODE_MOUSE_ANY, on); break;
        case 1004: set_mode_bit(v, VT_MODE_FOCUS, on); break;
        case 1005: set_mode_bit(v, VT_MODE_MOUSE_UTF8, on); break;
        case 1006: set_mode_bit(v, VT_MODE_MOUSE_SGR, on); break;
        case 1047: vt_screen_switch_alt(v, on, false, on); break;
        case 1048:
            if (on) save_cursor(v);
            else restore_cursor(v);
            break;
        case 1049:
            if (on) { save_cursor(v); vt_screen_switch_alt(v, true, false, true); }
            else { vt_screen_switch_alt(v, false, false, false); restore_cursor(v); }
            break;
        case 2004: set_mode_bit(v, VT_MODE_PASTE, on); break;
        default: break;
        }
    }
}

static void do_sm_rm(vt *v, bool on) { /* non-private SM/RM */
    for (int i = 0; i < v->nparams; i++) {
        if (vt_param(v, i, 0) == 4) set_mode_bit(v, VT_MODE_IRM, on);
    }
}

/* ---- responses ---- */

static void respond(vt *v, const char *s, size_t n) {
    if (v->cb.on_response) v->cb.on_response(v->ud, s, n);
}

static void do_dsr(vt *v) {
    switch (vt_param(v, 0, 0)) {
    case 5: respond(v, "\x1b[0n", 4); break; /* device OK */
    case 6: { /* CPR — vim startup depends on this */
        char buf[32];
        int row = v->cur.row + 1, col = v->cur.col + 1;
        if (v->modes & VT_MODE_DECOM) row -= v->scroll_top;
        int n = snprintf(buf, sizeof buf, "\x1b[%d;%dR", row, col);
        if (n > 0) respond(v, buf, (size_t)n);
        break;
    }
    default: break;
    }
}

static void do_decrqm(vt *v) { /* CSI ? Ps $ p */
    int ps = vt_param(v, 0, 0);
    uint32_t bit = 0;
    switch (ps) {
    case 1: bit = VT_MODE_DECCKM; break;
    case 6: bit = VT_MODE_DECOM; break;
    case 7: bit = VT_MODE_DECAWM; break;
    case 25: bit = VT_MODE_DECTCEM; break;
    case 47: case 1047: case 1049: bit = VT_MODE_ALTSCREEN; break;
    case 1000: bit = VT_MODE_MOUSE_X10; break;
    case 1002: bit = VT_MODE_MOUSE_BTN; break;
    case 1003: bit = VT_MODE_MOUSE_ANY; break;
    case 1004: bit = VT_MODE_FOCUS; break;
    case 1006: bit = VT_MODE_MOUSE_SGR; break;
    case 2004: bit = VT_MODE_PASTE; break;
    default: {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "\x1b[?%d;0$y", ps); /* not recognized */
        if (n > 0) respond(v, buf, (size_t)n);
        return;
    }
    }
    char buf[32];
    int n = snprintf(buf, sizeof buf, "\x1b[?%d;%d$y", ps, (v->modes & bit) ? 1 : 2);
    if (n > 0) respond(v, buf, (size_t)n);
}

/* ---- CSI dispatch ---- */

void vt_do_csi(vt *v, uint8_t final) {
    if (v->ninter > VT_INTER_MAX) return; /* over-collected: no-op */

    if (v->priv == '?') {
        switch (final) {
        case 'h': do_decset(v, true); return;
        case 'l': do_decset(v, false); return;
        case 'p':
            if (v->ninter == 1 && v->inter[0] == '$') do_decrqm(v);
            return;
        default: return;
        }
    }
    if (v->priv == '>') {
        if (final == 'c') respond(v, "\x1b[>1;10;0c", 10); /* DA2 */
        return; /* modifyOtherKeys etc.: consumed */
    }
    if (v->priv) return; /* '<', '=': consumed */

    if (v->ninter == 1 && v->inter[0] == ' ') {
        if (final == 'q') return; /* DECSCUSR: cursor style, presentation-only */
        return;
    }
    if (v->ninter == 1 && v->inter[0] == '$') {
        return; /* DECRQM ANSI etc.: consumed */
    }
    if (v->ninter != 0) return;

    int p0 = vt_param(v, 0, 1); /* most sequences default to 1 */
    switch (final) {
    case 'A': vt_screen_move_cursor(v, v->cur.row - (v->modes & VT_MODE_DECOM ? v->scroll_top : 0) - (p0 ? p0 : 1), v->cur.col); break;
    case 'B': vt_screen_move_cursor(v, v->cur.row - (v->modes & VT_MODE_DECOM ? v->scroll_top : 0) + (p0 ? p0 : 1), v->cur.col); break;
    case 'C': vt_screen_move_cursor(v, v->cur.row - (v->modes & VT_MODE_DECOM ? v->scroll_top : 0), v->cur.col + (p0 ? p0 : 1)); break;
    case 'D': vt_screen_move_cursor(v, v->cur.row - (v->modes & VT_MODE_DECOM ? v->scroll_top : 0), v->cur.col - (p0 ? p0 : 1)); break;
    case 'E': vt_screen_move_cursor(v, v->cur.row - (v->modes & VT_MODE_DECOM ? v->scroll_top : 0) + (p0 ? p0 : 1), 0); break;
    case 'F': vt_screen_move_cursor(v, v->cur.row - (v->modes & VT_MODE_DECOM ? v->scroll_top : 0) - (p0 ? p0 : 1), 0); break;
    case 'G': vt_screen_move_cursor(v, v->cur.row - (v->modes & VT_MODE_DECOM ? v->scroll_top : 0), (p0 ? p0 : 1) - 1); break; /* CHA */
    case 'H': case 'f': /* CUP/HVP, 1-based */
        vt_screen_move_cursor(v, vt_param(v, 0, 1) - 1, vt_param(v, 1, 1) - 1);
        break;
    case 'd': vt_screen_move_cursor(v, (p0 ? p0 : 1) - 1, v->cur.col); break; /* VPA */
    case 'J': vt_screen_erase_display(v, vt_param(v, 0, 0)); break;
    case 'K': vt_screen_erase_line(v, vt_param(v, 0, 0)); break;
    case 'L': vt_screen_insert_lines(v, p0 ? p0 : 1); break;
    case 'M': vt_screen_delete_lines(v, p0 ? p0 : 1); break;
    case '@': vt_screen_insert_chars(v, p0 ? p0 : 1); break;
    case 'P': vt_screen_delete_chars(v, p0 ? p0 : 1); break;
    case 'X': vt_screen_erase_chars(v, p0 ? p0 : 1); break;
    case 'S': vt_screen_scroll_up(v, p0 ? p0 : 1); break;
    case 'T': vt_screen_scroll_down(v, p0 ? p0 : 1); break;
    case 'b': { /* REP: repeat last graphic char */
        int n = p0 ? p0 : 1;
        if (n > VT_COLS_MAX) n = VT_COLS_MAX;
        uint16_t col = v->cur.col ? (uint16_t)(v->cur.col - 1) : 0;
        vt_cell *prev = vt_cell_at(v, v->cur.row, col);
        if (prev && prev->cp) {
            /* Copy out of the grid before printing: vt_screen_put can scroll,
             * which relocates cells under `prev` while the loop still runs. */
            uint32_t cp = prev->cp, mark = prev->comb;
            int w = vt_wcwidth(cp);
            for (int i = 0; i < n; i++) {
                vt_screen_put(v, cp, w);
                /* Repeat the whole grapheme, not just the base. */
                if (mark) vt_screen_put(v, mark, 0);
            }
        }
        break;
    }
    case 'g': /* TBC */
        if (vt_param(v, 0, 0) == 3) memset(v->tabstops, 0, sizeof v->tabstops);
        else v->tabstops[v->cur.col / 32] &= ~(1u << (v->cur.col % 32));
        break;
    case 'h': do_sm_rm(v, true); break;
    case 'l': do_sm_rm(v, false); break;
    case 'm': do_sgr(v); break;
    case 'n': do_dsr(v); break;
    case 'r': vt_screen_set_scroll_region(v, vt_param(v, 0, 0), vt_param(v, 1, 0)); break;
    case 'c': respond(v, "\x1b[?62;22c", 9); break; /* DA1: VT220 + color */
    case 's': save_cursor(v); break;
    case 'u': restore_cursor(v); break;
    /* 't' (XTWINOPS) is deliberately unimplemented: geometry belongs to the
     * attached client, not the app. Consumed by the no-op default. */
    default: break;
    }
}

/* ---- OSC ---- */

void vt_do_osc(vt *v) {
    v->osc[v->osc_len] = '\0';
    /* Parse "Ps;Pt" */
    char *sep = memchr(v->osc, ';', v->osc_len);
    if (!sep) return;
    *sep = '\0';
    int ps = 0;
    for (char *p = v->osc; *p; p++) {
        if (*p < '0' || *p > '9') return;
        ps = ps > 999 ? 9999 : ps * 10 + (*p - '0');
    }
    const char *pt = sep + 1;
    size_t pt_len = v->osc_len - (size_t)(pt - v->osc);
    *sep = ';'; /* restore for passthrough reconstruction */

    switch (ps) {
    case 0: case 2: /* title: handle AND pass through */
        if (v->cb.on_title) v->cb.on_title(v->ud, pt, pt_len);
        if (v->cb.on_passthrough && !v->osc_overflow) {
            char buf[VT_OSC_MAX + 8];
            size_t n = 0;
            buf[n++] = '\x1b'; buf[n++] = ']';
            memcpy(buf + n, v->osc, v->osc_len); n += v->osc_len;
            buf[n++] = '\x07';
            v->cb.on_passthrough(v->ud, buf, n);
        }
        break;
    case 52: /* clipboard: passthrough only; host terminal owns the clipboard */
        if (v->cb.on_passthrough && !v->osc_overflow) {
            char buf[VT_OSC_MAX + 8];
            size_t n = 0;
            buf[n++] = '\x1b'; buf[n++] = ']';
            memcpy(buf + n, v->osc, v->osc_len); n += v->osc_len;
            buf[n++] = '\x07';
            v->cb.on_passthrough(v->ud, buf, n);
        }
        break;
    default: break; /* consumed */
    }
}
