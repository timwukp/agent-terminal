#include "scan.h"

#include "common/xutil.h" /* now_ms */

/* Deviation: dump the swallowed prefix into fwd and reset. The deviating
 * byte is NOT emitted here — the caller's loop re-examines it (a Ctrl-\
 * right after a broken chord must re-arm, not pass through). */
static size_t chord_flush(chord *ch, uint8_t *fwd, size_t o) {
    for (int k = 0; k < ch->npending; k++) fwd[o++] = ch->pending[k];
    ch->armed = 0;
    ch->npending = 0;
    return o;
}

scan_result scan_input(chord *ch, const uint8_t *in, size_t len, uint8_t *fwd,
                              size_t *fwdlen, size_t *consumed) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = in[i];
        switch (ch->armed) {
        case 1: /* after Ctrl-\ */
            ch->armed = 0;
            ch->npending = 0;
            if (b == KEY_CTRL_D) { *fwdlen = o; *consumed = i + 1; return SCAN_DETACH; }
            if (b == KEY_COPY_MODE) { *fwdlen = o; *consumed = i + 1; return SCAN_COPY_MODE; }
            if (b == '"') { *fwdlen = o; *consumed = i + 1; return SCAN_SPLIT_STACKED; }
            if (b == '%') { *fwdlen = o; *consumed = i + 1; return SCAN_SPLIT_SIDE; }
            if (b == 'o') { *fwdlen = o; *consumed = i + 1; return SCAN_PANE_NEXT; }
            if (b == ';') { *fwdlen = o; *consumed = i + 1; return SCAN_PANE_LAST; }
            if (b == 'x') { *fwdlen = o; *consumed = i + 1; return SCAN_PANE_CLOSE; }
            if (b == 'z') { *fwdlen = o; *consumed = i + 1; return SCAN_PANE_ZOOM; }
            if (b == 0x1b) { /* possible arrow: ESC [ A-D */
                ch->armed = 2;
                ch->pending[0] = KEY_CTRL_BACKSLASH;
                ch->pending[1] = 0x1b;
                ch->npending = 2;
                continue;
            }
            fwd[o++] = KEY_CTRL_BACKSLASH; /* forward the swallowed byte */
            fwd[o++] = b;
            continue;
        case 2: /* after Ctrl-\ ESC */
            if (b == '[') {
                ch->armed = 3;
                ch->pending[2] = '[';
                ch->npending = 3;
                continue;
            }
            o = chord_flush(ch, fwd, o);
            i--; /* re-examine this byte from idle */
            continue;
        case 3: /* after Ctrl-\ ESC [ */
            if (b >= 'A' && b <= 'D') {
                static const scan_result dir[4] = {SCAN_PANE_UP, SCAN_PANE_DOWN,
                                                   SCAN_PANE_RIGHT, SCAN_PANE_LEFT};
                ch->armed = 0;
                ch->npending = 0;
                *fwdlen = o;
                *consumed = i + 1;
                return dir[b - 'A'];
            }
            o = chord_flush(ch, fwd, o);
            i--;
            continue;
        default:
            if (b == KEY_CTRL_BACKSLASH) {
                ch->armed = 1;
                ch->armed_at = now_ms();
                continue;
            }
            fwd[o++] = b;
        }
    }
    *fwdlen = o;
    *consumed = len;
    return SCAN_NONE;
}
