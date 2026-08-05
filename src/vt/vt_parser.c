/* vt_parser.c — the Williams VT500-series state machine (vt100.net model).
 *
 * Structure: a byte enters vt_feed(); C0/C1 controls and state transitions
 * are handled per the published diagram; printable bytes in GROUND go
 * through the UTF-8 DFA and then to the screen. All parser buffers are
 * fixed-size; parameters saturate; any byte in any state has a defined
 * transition. */
#include "vt_internal.h"

/* ---- parameter/intermediate collection ---- */

static void clear_seq(vt *v) {
    v->nparams = 0;
    v->param_overflow = false;
    for (int i = 0; i < VT_PARAMS_MAX; i++) { v->params[i] = 0; v->param_seen[i] = false; }
    v->ninter = 0;
    v->inter[0] = '\0';
    v->priv = 0;
}

static void collect_inter(vt *v, uint8_t b) {
    if (v->ninter < VT_INTER_MAX) {
        v->inter[v->ninter++] = (char)b;
        v->inter[v->ninter] = '\0';
    } else {
        v->ninter = VT_INTER_MAX + 1; /* too many: sequence becomes no-op */
    }
}

static void collect_param(vt *v, uint8_t b) {
    if (b == ';') {
        if (v->nparams < VT_PARAMS_MAX) v->nparams++;
        else v->param_overflow = true;
        return;
    }
    /* digit */
    int idx = v->nparams;
    if (idx >= VT_PARAMS_MAX) { v->param_overflow = true; return; }
    uint32_t d = (uint32_t)(b - '0');
    uint32_t cur = v->params[idx];
    v->params[idx] = cur > 999999 ? 9999999 : cur * 10 + d; /* saturate */
    v->param_seen[idx] = true;
}

static void finish_params(vt *v) {
    /* Make nparams count the last param if anything was collected. */
    if (v->nparams < VT_PARAMS_MAX &&
        (v->param_seen[v->nparams] || v->nparams > 0 || v->param_seen[0]))
        v->nparams++;
}

/* ---- OSC ---- */

static void osc_start(vt *v) {
    v->osc_len = 0;
    v->osc_overflow = false;
}

static void osc_put(vt *v, uint8_t b) {
    if (v->osc_len < VT_OSC_MAX - 1) v->osc[v->osc_len++] = (char)b;
    else v->osc_overflow = true; /* excess discarded, xterm behavior */
}

/* ---- the state machine ---- */

/* "Anywhere" transitions (highest priority, any state). Returns true if
 * the byte was consumed. */
static bool anywhere(vt *v, uint8_t b) {
    switch (b) {
    case 0x18: case 0x1a: /* CAN, SUB abort the sequence */
        v->state = ST_GROUND;
        return true;
    case 0x1b: /* ESC restarts */
        /* ESC inside OSC/DCS/SOS may be the start of ST (ESC \) — handled
         * by the string states themselves; everywhere else it restarts. */
        if (v->state == ST_OSC_STRING || v->state == ST_DCS_PASS ||
            v->state == ST_SOSPMAPC)
            return false;
        clear_seq(v);
        v->state = ST_ESCAPE;
        return true;
    /* No case for 0x9c (raw C1 ST). This stream is UTF-8, where 0x80-0x9f are
     * continuation bytes and 8-bit C1 controls are unreachable — xterm likewise
     * disables them in UTF-8 mode. Honouring raw ST here dropped every
     * character whose encoding contains 0x9c (4.5% of all codepoints: U+672C
     * 本, U+00DC U-umlaut, U+D55C Korean HAN, ...) and, inside OSC, ended the
     * string early so the tail of a title leaked onto the grid. Strings are
     * terminated by BEL or ESC \ only. */
    default:
        return false;
    }
}

static void feed_byte(vt *v, uint8_t b) {
    if (anywhere(v, b)) return;

    switch (v->state) {
    case ST_GROUND:
        if (b < 0x20) { vt_do_execute(v, b); return; }
        if (b == 0x7f) return; /* DEL ignored */
        /* Printable: UTF-8 DFA (state carried across feeds). */
        {
            uint32_t rc = vt_utf8_step(&v->u8_state, &v->u8_cp, b);
            if (rc == UTF8_ACCEPT) {
                vt_do_print(v, v->u8_cp);
            } else if (rc == UTF8_REJECT) {
                v->u8_state = UTF8_ACCEPT;
                vt_do_print(v, 0xFFFD);
                /* WHATWG resync: retry this byte as a new sequence start
                 * unless it's ASCII (then it prints itself). */
                if (b < 0x80) {
                    vt_do_print(v, b);
                } else if (b >= 0xc2) {
                    vt_utf8_step(&v->u8_state, &v->u8_cp, b);
                }
            }
        }
        return;

    case ST_ESCAPE:
        v->u8_state = UTF8_ACCEPT; /* any escape aborts a partial UTF-8 char */
        if (b < 0x20) { vt_do_execute(v, b); return; }
        if (b >= 0x20 && b <= 0x2f) { collect_inter(v, b); v->state = ST_ESCAPE_INT; return; }
        switch (b) {
        case '[': clear_seq(v); v->state = ST_CSI_ENTRY; return;
        case ']': osc_start(v); v->state = ST_OSC_STRING; return;
        case 'P': clear_seq(v); v->state = ST_DCS_ENTRY; return;
        case 'X': case '^': case '_': v->state = ST_SOSPMAPC; return;
        default:
            if (b >= 0x30 && b <= 0x7e) { vt_do_esc(v, b); v->state = ST_GROUND; }
            return;
        }

    case ST_ESCAPE_INT:
        if (b < 0x20) { vt_do_execute(v, b); return; }
        if (b >= 0x20 && b <= 0x2f) { collect_inter(v, b); return; }
        if (b >= 0x30 && b <= 0x7e) { vt_do_esc(v, b); v->state = ST_GROUND; return; }
        return;

    case ST_CSI_ENTRY:
        if (b < 0x20) { vt_do_execute(v, b); return; }
        if (b >= 0x3c && b <= 0x3f) { v->priv = (char)b; v->state = ST_CSI_PARAM; return; }
        if ((b >= '0' && b <= '9') || b == ';') {
            collect_param(v, b);
            v->state = ST_CSI_PARAM;
            return;
        }
        if (b == ':') { v->state = ST_CSI_IGNORE; return; }
        if (b >= 0x20 && b <= 0x2f) { collect_inter(v, b); v->state = ST_CSI_INT; return; }
        if (b >= 0x40 && b <= 0x7e) {
            finish_params(v);
            vt_do_csi(v, b);
            v->state = ST_GROUND;
            return;
        }
        return;

    case ST_CSI_PARAM:
        if (b < 0x20) { vt_do_execute(v, b); return; }
        if ((b >= '0' && b <= '9') || b == ';') { collect_param(v, b); return; }
        if (b == ':' || (b >= 0x3c && b <= 0x3f)) { v->state = ST_CSI_IGNORE; return; }
        if (b >= 0x20 && b <= 0x2f) { collect_inter(v, b); v->state = ST_CSI_INT; return; }
        if (b >= 0x40 && b <= 0x7e) {
            finish_params(v);
            vt_do_csi(v, b);
            v->state = ST_GROUND;
            return;
        }
        return;

    case ST_CSI_INT:
        if (b < 0x20) { vt_do_execute(v, b); return; }
        if (b >= 0x20 && b <= 0x2f) { collect_inter(v, b); return; }
        if (b >= 0x30 && b <= 0x3f) { v->state = ST_CSI_IGNORE; return; }
        if (b >= 0x40 && b <= 0x7e) {
            finish_params(v);
            vt_do_csi(v, b);
            v->state = ST_GROUND;
            return;
        }
        return;

    case ST_CSI_IGNORE:
        if (b < 0x20) { vt_do_execute(v, b); return; }
        if (b >= 0x40 && b <= 0x7e) v->state = ST_GROUND;
        return;

    case ST_OSC_STRING:
        if (b == 0x1b) { v->state = ST_MAX; return; /* ST_MAX = "saw ESC in OSC" */ }
        if (b == 0x07) { vt_do_osc(v); v->state = ST_GROUND; return; } /* BEL terminator */
        if (b >= 0x20 || b == 0x00) { osc_put(v, b); }
        return;

    case ST_MAX: /* pseudo-state: ESC seen inside OSC */
        if (b == '\\') { vt_do_osc(v); v->state = ST_GROUND; return; } /* ESC \ = ST */
        /* Not ST: abort OSC, reprocess as escape */
        clear_seq(v);
        v->state = ST_ESCAPE;
        feed_byte(v, b);
        return;

    case ST_DCS_ENTRY:
    case ST_DCS_PARAM:
    case ST_DCS_INT:
        /* We consume DCS fully but act on none of it (v1). Route bytes to
         * the appropriate sub-state per the diagram, minus the hook/put
         * actions. */
        if (b >= 0x40 && b <= 0x7e) { v->state = ST_DCS_PASS; return; }
        if (b == 0x18 || b == 0x1a) { v->state = ST_GROUND; return; }
        return;

    case ST_DCS_PASS:
        if (b == 0x1b) { v->state = ST_DCS_IGNORE; return; } /* maybe ST */
        return; /* payload consumed */

    case ST_DCS_IGNORE:
        /* After ESC inside DCS: '\' ends it; anything else keeps consuming
         * (permissive; can't corrupt state). */
        if (b == '\\') { v->state = ST_GROUND; return; }
        if (b != 0x1b) v->state = ST_DCS_PASS;
        return;

    case ST_SOSPMAPC:
        if (b == 0x1b) { v->state = ST_DCS_IGNORE; return; } /* reuse ST hunt */
        return;
    }
}

void vt_feed(vt *v, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        feed_byte(v, data[i]);
}
