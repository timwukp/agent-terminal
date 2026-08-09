/* test_scan.c — the input-chord scanner: every chord, and the invariant that
 * bytes a chord swallows are never lost — on deviation they re-emerge
 * verbatim, in order, before the deviating byte is re-examined. */
#include "runner.h"

#include "client/scan.h"

/* One scan over `in`; returns the result and captures fwd bytes. */
static scan_result scan1(chord *ch, const char *in, size_t len,
                         uint8_t *fwd, size_t *fwdlen, size_t *consumed) {
    return scan_input(ch, (const uint8_t *)in, len, fwd, fwdlen, consumed);
}

#define CB "\x1c" /* Ctrl-\ */

TEST(plain_bytes_pass_through) {
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    ASSERT_EQ_INT(scan1(&ch, "hello", 5, fwd, &fl, &co), SCAN_NONE);
    ASSERT_EQ_INT(fl, 5);
    ASSERT_EQ_MEM(fwd, "hello", 5);
    ASSERT_EQ_INT(co, 5);
}

TEST(single_byte_chords) {
    struct { const char *seq; int want; } cases[] = {
        {CB "\x04", SCAN_DETACH},      {CB "[", SCAN_COPY_MODE},
        {CB "\"", SCAN_SPLIT_STACKED}, {CB "%", SCAN_SPLIT_SIDE},
        {CB "o", SCAN_PANE_NEXT},      {CB ";", SCAN_PANE_LAST},
        {CB "x", SCAN_PANE_CLOSE},     {CB "z", SCAN_PANE_ZOOM},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        chord ch = {0};
        uint8_t fwd[64];
        size_t fl, co;
        ASSERT_EQ_INT(scan1(&ch, cases[i].seq, 2, fwd, &fl, &co), cases[i].want);
        ASSERT_EQ_INT(fl, 0);   /* a completed chord forwards nothing */
        ASSERT_EQ_INT(co, 2);
    }
}

TEST(arrow_chords) {
    struct { const char *seq; int want; } cases[] = {
        {CB "\x1b[A", SCAN_PANE_UP},    {CB "\x1b[B", SCAN_PANE_DOWN},
        {CB "\x1b[C", SCAN_PANE_RIGHT}, {CB "\x1b[D", SCAN_PANE_LEFT},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        chord ch = {0};
        uint8_t fwd[64];
        size_t fl, co;
        ASSERT_EQ_INT(scan1(&ch, cases[i].seq, 4, fwd, &fl, &co), cases[i].want);
        ASSERT_EQ_INT(fl, 0);
        ASSERT_EQ_INT(co, 4);
    }
}

TEST(arrow_split_across_reads) {
    /* A chord must survive read() boundaries: one byte per call. */
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    const char *seq = CB "\x1b[C";
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT(scan1(&ch, seq + i, 1, fwd, &fl, &co), SCAN_NONE);
        ASSERT_EQ_INT(fl, 0); /* nothing may leak while the chord is pending */
    }
    ASSERT_EQ_INT(scan1(&ch, seq + 3, 1, fwd, &fl, &co), SCAN_PANE_RIGHT);
    ASSERT_EQ_INT(fl, 0);
}

TEST(unknown_second_byte_forwards_both) {
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    ASSERT_EQ_INT(scan1(&ch, CB "q", 2, fwd, &fl, &co), SCAN_NONE);
    ASSERT_EQ_INT(fl, 2);
    ASSERT_EQ_MEM(fwd, CB "q", 2);
}

TEST(literal_esc_after_prefix_is_not_swallowed) {
    /* Ctrl-\ ESC then a NON-[ byte: all three bytes must come through —
     * the ESC was only provisionally swallowed. */
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    ASSERT_EQ_INT(scan1(&ch, CB "\x1bq", 3, fwd, &fl, &co), SCAN_NONE);
    ASSERT_EQ_INT(fl, 3);
    ASSERT_EQ_MEM(fwd, CB "\x1bq", 3);
}

TEST(esc_bracket_then_non_arrow_forwards_all) {
    /* Ctrl-\ ESC [ then 'z' (not A-D): the three swallowed bytes flush, and
     * the deviating byte is re-examined from idle — 'z' alone is plain. */
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    ASSERT_EQ_INT(scan1(&ch, CB "\x1b[z", 4, fwd, &fl, &co), SCAN_NONE);
    ASSERT_EQ_INT(fl, 4);
    ASSERT_EQ_MEM(fwd, CB "\x1b[z", 4);
}

TEST(deviating_ctrl_backslash_rearms) {
    /* Ctrl-\ ESC Ctrl-\ x : the broken chord flushes its two bytes, and the
     * deviating Ctrl-\ must START A NEW CHORD (x completes it as close) —
     * not pass through as a literal. */
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    ASSERT_EQ_INT(scan1(&ch, CB "\x1b" CB "x", 4, fwd, &fl, &co), SCAN_PANE_CLOSE);
    ASSERT_EQ_INT(fl, 2); /* the flushed Ctrl-\ ESC, nothing else */
    ASSERT_EQ_MEM(fwd, CB "\x1b", 2);
    ASSERT_EQ_INT(co, 4);
}

TEST(text_before_chord_is_kept) {
    /* Bytes before the chord in the same batch must be in fwd when the
     * chord completes — this exact loss shipped once (see the header). */
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    ASSERT_EQ_INT(scan1(&ch, "ab" CB "\x1b[A", 6, fwd, &fl, &co), SCAN_PANE_UP);
    ASSERT_EQ_INT(fl, 2);
    ASSERT_EQ_MEM(fwd, "ab", 2);
    ASSERT_EQ_INT(co, 6);
}

TEST(bytes_after_chord_stay_unconsumed) {
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    ASSERT_EQ_INT(scan1(&ch, CB "ztail", 6, fwd, &fl, &co), SCAN_PANE_ZOOM);
    ASSERT_EQ_INT(co, 2); /* "tail" belongs to the next scan (pager rule) */
}

TEST(pending_state_reports_for_timeout_flush) {
    /* The caller flushes ch.pending on timeout; the machine must expose
     * exactly what it swallowed at each depth. */
    chord ch = {0};
    uint8_t fwd[64];
    size_t fl, co;
    scan1(&ch, CB, 1, fwd, &fl, &co);
    ASSERT_EQ_INT(ch.armed, 1);
    ASSERT_EQ_INT(ch.npending, 0); /* state 1 holds only the implicit Ctrl-\ */
    scan1(&ch, "\x1b", 1, fwd, &fl, &co);
    ASSERT_EQ_INT(ch.armed, 2);
    ASSERT_EQ_INT(ch.npending, 2);
    ASSERT_EQ_MEM(ch.pending, CB "\x1b", 2);
    scan1(&ch, "[", 1, fwd, &fl, &co);
    ASSERT_EQ_INT(ch.armed, 3);
    ASSERT_EQ_INT(ch.npending, 3);
    ASSERT_EQ_MEM(ch.pending, CB "\x1b[", 3);
}

int main(void) {
    RUN(plain_bytes_pass_through);
    RUN(single_byte_chords);
    RUN(arrow_chords);
    RUN(arrow_split_across_reads);
    RUN(unknown_second_byte_forwards_both);
    RUN(literal_esc_after_prefix_is_not_swallowed);
    RUN(esc_bracket_then_non_arrow_forwards_all);
    RUN(deviating_ctrl_backslash_rearms);
    RUN(text_before_chord_is_kept);
    RUN(bytes_after_chord_stay_unconsumed);
    RUN(pending_state_reports_for_timeout_flush);
    TEST_MAIN_END();
}
