/* scan.h — the client's input-chord scanner.
 *
 * Extracted from attach.c so the state machine is unit-testable (same
 * pattern as pager.c): the forward-the-swallowed-bytes invariant broke once
 * already in review, and only a table of byte-exact cases keeps it honest. */
#ifndef AT_SCAN_H
#define AT_SCAN_H

#include <stddef.h>
#include <stdint.h>

#define KEY_CTRL_BACKSLASH 0x1c
#define KEY_CTRL_D 0x04
#define KEY_COPY_MODE '['
#define CHORD_TIMEOUT_MS 500

typedef struct {
    /* 0 = idle; 1 = saw Ctrl-\; 2 = saw Ctrl-\ ESC; 3 = saw Ctrl-\ ESC [ .
     * States 2-3 exist for arrow-key selection: an arrow is the 3-byte
     * sequence ESC [ A/B/C/D, so the chord machine must hold up to three
     * swallowed bytes and — the invariant the tests pin — forward every one
     * of them verbatim the moment the sequence deviates or times out. A
     * user typing Ctrl-\ then a literal ESC gets both bytes delivered. */
    int armed;
    uint64_t armed_at;
    uint8_t pending[3]; /* the swallowed prefix, for deviation/timeout flush */
    int npending;
} chord;

typedef enum {
    SCAN_NONE, SCAN_DETACH, SCAN_COPY_MODE,
    SCAN_SPLIT_STACKED,   /* Ctrl-\ "  — one above the other */
    SCAN_SPLIT_SIDE,      /* Ctrl-\ %  — side by side */
    SCAN_PANE_NEXT,       /* Ctrl-\ o */
    SCAN_PANE_LAST,       /* Ctrl-\ ; */
    SCAN_PANE_CLOSE,      /* Ctrl-\ x */
    SCAN_PANE_ZOOM,       /* Ctrl-\ z */
    SCAN_PANE_UP,         /* Ctrl-\ ↑  (ESC [ A) */
    SCAN_PANE_DOWN,       /* Ctrl-\ ↓  (ESC [ B) */
    SCAN_PANE_RIGHT,      /* Ctrl-\ →  (ESC [ C) */
    SCAN_PANE_LEFT,       /* Ctrl-\ ←  (ESC [ D) */
} scan_result;

/* Forwardable bytes land in fwd (caller-sized >= 2*len). *fwdlen is always
 * assigned, and bytes seen before a chord completes are kept: returning early
 * without setting it silently dropped everything typed earlier in the same
 * read() batch. */
scan_result scan_input(chord *ch, const uint8_t *in, size_t len, uint8_t *fwd,
                       size_t *fwdlen, size_t *consumed);

#endif
