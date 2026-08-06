/* test_pager.c — scrollback copy-mode: line accumulation, movement, search,
 * key decoding, and the request-continuation logic.
 *
 * Drawing is checked by writing to a temp file rather than a tty, which is
 * what pager_set_out_fd exists for: the escape sequences the pager emits are
 * the contract with the terminal (alt screen, autowrap off), and asserting on
 * them here is far cheaper than the integration test.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client/pager.h"
#include "runner.h"

/* ---- drawing capture ---- */

static char g_out[1 << 20];
static size_t g_out_len;
static char g_tmpl[] = "/tmp/at_pager_out_XXXXXX";
static int g_fd = -1;

static void capture_start(pager *pg) {
    if (g_fd >= 0) close(g_fd);
    strcpy(g_tmpl, "/tmp/at_pager_out_XXXXXX");
    g_fd = mkstemp(g_tmpl);
    unlink(g_tmpl); /* keep it anonymous; we only ever pread it */
    pager_set_out_fd(pg, g_fd);
    g_out_len = 0;
}

static const char *capture_read(void) {
    ssize_t n = pread(g_fd, g_out, sizeof g_out - 1, 0);
    g_out_len = n > 0 ? (size_t)n : 0;
    g_out[g_out_len] = '\0';
    return g_out;
}

static void feed(pager *pg, const char *keys) {
    pager_input(pg, (const uint8_t *)keys, strlen(keys));
}

static void add(pager *pg, uint64_t seq, const char *text) {
    pager_add_line(pg, seq, text, (uint32_t)strlen(text));
}

/* Fill with lines "L0".."L(n-1)" at seq 0..n-1. */
static pager *make_pager(uint32_t n, uint16_t cols, uint16_t rows) {
    pager *pg = pager_new();
    capture_start(pg);
    for (uint32_t i = 0; i < n; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "L%u", i);
        add(pg, i, buf);
    }
    pager_enter(pg, cols, rows, n);
    return pg;
}

/* ---- tests ---- */

TEST(dedup_by_seq) {
    pager *pg = pager_new();
    add(pg, 0, "a");
    add(pg, 1, "b");
    ASSERT_EQ_INT(pager_line_count(pg), 2);

    /* The disk log and the ring overlap; re-delivering an old seq must not
     * duplicate the line. Without this, every flushed line would appear twice
     * in copy-mode: once from sb_read_log and once from MSG_SCROLLBACK_DATA. */
    add(pg, 0, "a");
    add(pg, 1, "b");
    ASSERT_EQ_INT(pager_line_count(pg), 2);

    add(pg, 2, "c");
    ASSERT_EQ_INT(pager_line_count(pg), 3);
    pager_free(pg);
}

TEST(seq_gap_still_accepted) {
    /* Rotation can leave a gap if the older generation was clobbered. A gap
     * must not stall accumulation — only non-increasing seqs are dropped. */
    pager *pg = pager_new();
    add(pg, 0, "a");
    add(pg, 500, "b");
    ASSERT_EQ_INT(pager_line_count(pg), 2);
    pager_free(pg);
}

TEST(enter_sets_terminal_modes) {
    pager *pg = make_pager(5, 80, 24);
    const char *o = capture_read();
    /* Alt screen so the live session's screen is not destroyed. */
    ASSERT_TRUE(strstr(o, "\x1b[?1049h") != NULL);
    /* Autowrap OFF is load-bearing: with it on, a stored line wider than the
     * terminal takes two rows and every row below is off by one. The client
     * cannot measure display width, so it makes the terminal clip instead. */
    ASSERT_TRUE(strstr(o, "\x1b[?7l") != NULL);
    pager_free(pg);
}

TEST(leave_restores_terminal_modes) {
    pager *pg = make_pager(5, 80, 24);
    capture_start(pg); /* fresh file so we only see the leave sequence */
    pager_leave(pg);
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "\x1b[?7h") != NULL);    /* autowrap back on */
    ASSERT_TRUE(strstr(o, "\x1b[?1049l") != NULL); /* leave alt screen */
    ASSERT_TRUE(strstr(o, "\x1b[?25h") != NULL);   /* cursor visible again */
    ASSERT_TRUE(!pager_active(pg));
    pager_free(pg);
}

TEST(opens_at_the_bottom) {
    /* 100 lines, 24 rows => 23 view rows, so the last page starts at 77. A
     * user reaching for scrollback wants the newest lines first. */
    pager *pg = make_pager(100, 80, 24);
    ASSERT_EQ_INT(pager_top(pg), 77);
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "L99") != NULL);
    ASSERT_TRUE(strstr(o, "L76") == NULL);
    pager_free(pg);
}

TEST(movement_keys) {
    pager *pg = make_pager(100, 80, 24);
    feed(pg, "g");
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "j");
    ASSERT_EQ_INT(pager_top(pg), 1);
    feed(pg, "k");
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "\x06"); /* Ctrl-f: one page = 23 rows */
    ASSERT_EQ_INT(pager_top(pg), 23);
    feed(pg, "\x02"); /* Ctrl-b */
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "\x04"); /* Ctrl-d: half page */
    ASSERT_EQ_INT(pager_top(pg), 11);
    feed(pg, "\x15"); /* Ctrl-u */
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "G");
    ASSERT_EQ_INT(pager_top(pg), 77);
    pager_free(pg);
}

TEST(movement_clamps_at_both_ends) {
    pager *pg = make_pager(100, 80, 24);
    feed(pg, "g");
    for (int i = 0; i < 10; i++) feed(pg, "k");
    ASSERT_EQ_INT(pager_top(pg), 0); /* no underflow past the first line */
    feed(pg, "G");
    for (int i = 0; i < 10; i++) feed(pg, "j");
    ASSERT_EQ_INT(pager_top(pg), 77); /* never scrolls past the last page */
    pager_free(pg);
}

TEST(arrow_and_page_keys) {
    pager *pg = make_pager(100, 80, 24);
    feed(pg, "g");
    feed(pg, "\x1b[B"); /* down */
    ASSERT_EQ_INT(pager_top(pg), 1);
    feed(pg, "\x1b[A"); /* up */
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "\x1b[6~"); /* PgDn */
    ASSERT_EQ_INT(pager_top(pg), 23);
    feed(pg, "\x1b[5~"); /* PgUp */
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "\x1b[F"); /* End */
    ASSERT_EQ_INT(pager_top(pg), 77);
    feed(pg, "\x1b[H"); /* Home */
    ASSERT_EQ_INT(pager_top(pg), 0);
    /* SS3: some terminals send ESC O A for arrows in application mode. */
    feed(pg, "\x1bOB");
    ASSERT_EQ_INT(pager_top(pg), 1);
    pager_free(pg);
}

TEST(split_escape_sequence_across_reads) {
    /* Arrow keys arrive split across read() boundaries under load. A decoder
     * that only handled whole sequences would treat the leading ESC as "quit"
     * and drop out of copy-mode mid-scroll. */
    pager *pg = make_pager(100, 80, 24);
    feed(pg, "g");
    feed(pg, "\x1b");
    ASSERT_TRUE(pager_esc_pending(pg));
    feed(pg, "[");
    feed(pg, "B");
    ASSERT_EQ_INT(pager_top(pg), 1);
    ASSERT_TRUE(pager_active(pg));
    pager_free(pg);
}

TEST(q_exits_and_lone_esc_exits_on_timeout) {
    pager *pg = make_pager(10, 80, 24);
    ASSERT_EQ_INT(pager_input(pg, (const uint8_t *)"q", 1), PAGER_EXIT);
    pager_free(pg);

    /* A lone ESC must quit, but only after the timeout proves no arrow bytes
     * are following. Before it, the pager stays open. */
    pg = make_pager(10, 80, 24);
    ASSERT_EQ_INT(pager_input(pg, (const uint8_t *)"\x1b", 1), PAGER_CONTINUE);
    ASSERT_TRUE(pager_esc_pending(pg));
    ASSERT_EQ_INT(pager_esc_timeout(pg), PAGER_EXIT);
    pager_free(pg);
}

TEST(esc_timeout_is_noop_when_not_pending) {
    pager *pg = make_pager(10, 80, 24);
    ASSERT_EQ_INT(pager_esc_timeout(pg), PAGER_CONTINUE);
    ASSERT_TRUE(pager_active(pg));
    pager_free(pg);
}

TEST(search_finds_and_wraps_not) {
    pager *pg = pager_new();
    capture_start(pg);
    add(pg, 0, "alpha");
    add(pg, 1, "beta");
    add(pg, 2, "gamma needle");
    add(pg, 3, "delta");
    add(pg, 4, "epsilon needle");
    pager_enter(pg, 80, 3, 5); /* 2 view rows, so max_top is 3 */
    feed(pg, "g");
    ASSERT_EQ_INT(pager_cur(pg), 0);

    feed(pg, "/needle\r");
    ASSERT_EQ_INT(pager_cur(pg), 2);
    ASSERT_EQ_INT(pager_top(pg), 2); /* hit scrolled to row 0 */
    feed(pg, "n");
    /* The next hit is the last line, which cannot be row 0 with 2 view rows:
     * top clamps to 3 while cur is 4. Asserting on top alone would look like
     * the search had not moved. */
    ASSERT_EQ_INT(pager_cur(pg), 4);
    ASSERT_EQ_INT(pager_top(pg), 3);

    /* No wraparound: searching past the last hit reports not-found and leaves
     * the position alone rather than silently jumping to the top. */
    feed(pg, "n");
    ASSERT_EQ_INT(pager_cur(pg), 4);
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "pattern not found") != NULL);

    feed(pg, "N"); /* backwards */
    ASSERT_EQ_INT(pager_cur(pg), 2);
    pager_free(pg);
}

TEST(search_ignores_ansi_in_stored_text) {
    /* Stored lines are pre-rendered ANSI. Searching the raw bytes would both
     * miss text split by an SGR sequence and match on "1m" inside escapes. */
    pager *pg = pager_new();
    capture_start(pg);
    add(pg, 0, "plain");
    add(pg, 1, "\x1b[1mneed\x1b[0mle\x1b[0m");
    pager_enter(pg, 80, 3, 2);
    feed(pg, "g");
    feed(pg, "/needle\r");
    ASSERT_EQ_INT(pager_cur(pg), 1);

    /* And a pattern that only exists inside the escape bytes must not match. */
    feed(pg, "g");
    feed(pg, "/1m\r");
    ASSERT_EQ_INT(pager_cur(pg), 0);
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "pattern not found") != NULL);
    pager_free(pg);
}

TEST(search_prompt_editing) {
    pager *pg = pager_new();
    capture_start(pg);
    add(pg, 0, "aaa");
    add(pg, 1, "target");
    pager_enter(pg, 80, 3, 2);
    feed(pg, "g");

    /* Backspace at the prompt edits the pattern rather than moving. */
    feed(pg, "/targetX");
    feed(pg, "\x7f");
    feed(pg, "\r");
    ASSERT_EQ_INT(pager_cur(pg), 1);

    /* Ctrl-c cancels the prompt without searching or exiting. */
    feed(pg, "g");
    feed(pg, "/zzz\x03");
    ASSERT_EQ_INT(pager_cur(pg), 0);
    ASSERT_TRUE(pager_active(pg));

    /* 'j' typed at a prompt is a literal, not a movement. */
    feed(pg, "/j");
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "\x03");
    pager_free(pg);
}

TEST(esc_at_prompt_cancels_search_not_pager) {
    pager *pg = make_pager(10, 80, 24);
    feed(pg, "/abc");
    ASSERT_EQ_INT(pager_esc_timeout(pg), PAGER_CONTINUE); /* nothing pending yet */
    feed(pg, "\x1b");
    ASSERT_EQ_INT(pager_esc_timeout(pg), PAGER_CONTINUE);
    ASSERT_TRUE(pager_active(pg));
    pager_free(pg);
}

TEST(n_without_pattern_reports_it) {
    pager *pg = make_pager(10, 80, 24);
    feed(pg, "n");
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "no pattern") != NULL);
    pager_free(pg);
}

TEST(status_line_reports_position) {
    pager *pg = make_pager(100, 80, 24);
    feed(pg, "g");
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "1-23/100") != NULL);
    feed(pg, "G");
    o = capture_read();
    ASSERT_TRUE(strstr(o, "78-100/100") != NULL);
    ASSERT_TRUE(strstr(o, "100%") != NULL);
    pager_free(pg);
}

TEST(resize_reclamps_and_redraws) {
    pager *pg = make_pager(100, 80, 24);
    feed(pg, "G");
    ASSERT_EQ_INT(pager_top(pg), 77);
    /* Growing the window shows more lines, so the last valid top shrinks. A
     * stale top would scroll past the end and draw blank rows. */
    pager_resize(pg, 80, 50);
    ASSERT_EQ_INT(pager_top(pg), 51);
    pager_resize(pg, 80, 24);
    ASSERT_EQ_INT(pager_top(pg), 51);
    pager_free(pg);
}

TEST(fewer_lines_than_rows) {
    pager *pg = make_pager(3, 80, 24);
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "G");
    ASSERT_EQ_INT(pager_top(pg), 0);
    feed(pg, "\x06");
    ASSERT_EQ_INT(pager_top(pg), 0);
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "1-3/3") != NULL);
    pager_free(pg);
}

TEST(empty_scrollback_does_not_crash) {
    pager *pg = pager_new();
    capture_start(pg);
    pager_enter(pg, 80, 24, 0);
    feed(pg, "jkgG");
    feed(pg, "/x\r");
    feed(pg, "n");
    ASSERT_EQ_INT(pager_line_count(pg), 0);
    ASSERT_EQ_INT(pager_top(pg), 0);
    ASSERT_TRUE(pager_active(pg));
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "0-0/0") != NULL);
    pager_free(pg);
}

TEST(tiny_terminal) {
    /* rows=1 leaves zero view rows; view_rows() floors at 1 so drawing and
     * movement stay defined instead of dividing by zero. */
    pager *pg = make_pager(10, 20, 1);
    feed(pg, "gjG");
    ASSERT_TRUE(pager_active(pg));
    pager_free(pg);
}

TEST(ring_tail_arriving_after_enter_is_shown) {
    /* The ring reply arrives after pager_enter has already anchored to the
     * bottom of the disk lines. Without tail-following those newest lines are
     * counted in the status line but never drawn, so copy-mode silently opens
     * on older history than it holds — and the un-flushed second is exactly
     * what a user reaching for scrollback just saw scroll away. */
    pager *pg = pager_new();
    capture_start(pg);
    for (uint32_t i = 0; i < 30; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "disk%u", i);
        add(pg, i, buf);
    }
    pager_enter(pg, 80, 5, 40); /* 4 view rows; daemon holds 10 more */
    ASSERT_EQ_INT(pager_top(pg), 26);

    for (uint32_t i = 30; i < 40; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "ring%u", i);
        add(pg, i, buf);
    }
    pager_add_batch_done(pg, 10);
    pager_draw(pg);
    ASSERT_EQ_INT(pager_line_count(pg), 40);
    ASSERT_EQ_INT(pager_top(pg), 36); /* moved with the tail, not left at 26 */
    const char *o = capture_read();
    ASSERT_TRUE(strstr(o, "ring39") != NULL);
    ASSERT_TRUE(strstr(o, "37-40/40") != NULL);
    pager_free(pg);
}

TEST(scrolled_up_position_survives_new_lines) {
    /* The other half of the same rule: a user who scrolled up is reading, and
     * must not be yanked to the bottom when the ring tail lands. */
    pager *pg = pager_new();
    capture_start(pg);
    for (uint32_t i = 0; i < 30; i++) add(pg, i, "x");
    pager_enter(pg, 80, 5, 40);
    feed(pg, "g");
    ASSERT_EQ_INT(pager_top(pg), 0);
    for (uint32_t i = 30; i < 40; i++) add(pg, i, "y");
    ASSERT_EQ_INT(pager_top(pg), 0);
    ASSERT_EQ_INT(pager_cur(pg), 0);
    pager_free(pg);
}

TEST(request_continuation) {
    /* sb_fetch clamps at 1000 lines per reply, so the tail may need several
     * requests. Entering with disk lines 0..9 and a daemon total of 30 must
     * ask for seq 10 next. */
    pager *pg = pager_new();
    capture_start(pg);
    for (uint32_t i = 0; i < 10; i++) add(pg, i, "x");
    pager_enter(pg, 80, 24, 30);
    ASSERT_EQ_INT(pager_want_from(pg), 10);

    /* A batch that lands 10 more (through seq 19) still leaves a gap. */
    for (uint32_t i = 10; i < 20; i++) add(pg, i, "y");
    pager_add_batch_done(pg, 10);
    ASSERT_EQ_INT(pager_want_from(pg), 20);

    /* Reaching the daemon's total stops the requests. */
    for (uint32_t i = 20; i < 30; i++) add(pg, i, "z");
    pager_add_batch_done(pg, 10);
    ASSERT_TRUE(pager_want_from(pg) == UINT64_MAX);
    pager_free(pg);
}

TEST(empty_reply_stops_requesting) {
    /* sb_fetch returns 0 both for "not written yet" and "evicted from the
     * ring" and cannot distinguish them, so an empty reply must end the loop.
     * Otherwise the client would request seq N forever. */
    pager *pg = pager_new();
    capture_start(pg);
    add(pg, 0, "a");
    pager_enter(pg, 80, 24, 1000); /* daemon claims far more than we hold */
    ASSERT_EQ_INT(pager_want_from(pg), 1);
    pager_add_batch_done(pg, 0);
    ASSERT_TRUE(pager_want_from(pg) == UINT64_MAX);
    pager_free(pg);
}

TEST(no_disk_lines_requests_from_zero) {
    pager *pg = pager_new();
    capture_start(pg);
    pager_enter(pg, 80, 24, 42);
    ASSERT_EQ_INT(pager_want_from(pg), 0);
    pager_free(pg);
}

TEST(disk_already_complete_requests_nothing) {
    pager *pg = pager_new();
    capture_start(pg);
    for (uint32_t i = 0; i < 42; i++) add(pg, i, "x");
    pager_enter(pg, 80, 24, 42); /* seqs 0..41 == 42 total: nothing missing */
    ASSERT_TRUE(pager_want_from(pg) == UINT64_MAX);
    pager_free(pg);
}

TEST(strip_ansi) {
    char out[64];
    size_t n = pager_strip_ansi("\x1b[1mbold\x1b[0m", 12, out, sizeof out);
    ASSERT_EQ_INT(n, 4);
    ASSERT_TRUE(strcmp(out, "bold") == 0);

    /* 256-colour and truecolour SGR, multiple params. */
    n = pager_strip_ansi("\x1b[38;5;196mred\x1b[0m", 18, out, sizeof out);
    ASSERT_TRUE(strcmp(out, "red") == 0);

    /* OSC with BEL terminator, and with ESC \ (ST). */
    n = pager_strip_ansi("\x1b]0;title\x07text", 14, out, sizeof out);
    ASSERT_TRUE(strcmp(out, "text") == 0);
    n = pager_strip_ansi("\x1b]0;t\x1b\\ab", 9, out, sizeof out);
    ASSERT_TRUE(strcmp(out, "ab") == 0);

    /* Truncated escape at the end must not read past the buffer. */
    n = pager_strip_ansi("ab\x1b", 3, out, sizeof out);
    ASSERT_TRUE(strcmp(out, "ab") == 0);
    n = pager_strip_ansi("ab\x1b[", 4, out, sizeof out);
    ASSERT_TRUE(strcmp(out, "ab") == 0);
    n = pager_strip_ansi("ab\x1b[38;5", 7, out, sizeof out);
    ASSERT_TRUE(strcmp(out, "ab") == 0);

    /* Control bytes are dropped; UTF-8 passes through untouched. */
    n = pager_strip_ansi("a\tb", 3, out, sizeof out);
    ASSERT_TRUE(strcmp(out, "ab") == 0);
    n = pager_strip_ansi("\xe6\x97\xa5", 3, out, sizeof out);
    ASSERT_EQ_INT(n, 3);

    /* Output cap is respected and always NUL-terminated. */
    char small[4];
    n = pager_strip_ansi("abcdefgh", 8, small, sizeof small);
    ASSERT_EQ_INT(n, 3);
    ASSERT_TRUE(strcmp(small, "abc") == 0);
    ASSERT_EQ_INT(pager_strip_ansi("abc", 3, small, 0), 0);
    pager_free(NULL); /* NULL-safe */
}

TEST(long_line_is_not_wrapped_by_us) {
    /* The pager emits the stored line verbatim and lets the terminal clip,
     * because it has no way to measure display width. One stored line must
     * always occupy exactly one row: emitting a newline mid-line would
     * desynchronise every row below it. */
    pager *pg = pager_new();
    capture_start(pg);
    char wide[400];
    memset(wide, 'x', sizeof wide - 1);
    wide[sizeof wide - 1] = '\0';
    add(pg, 0, wide);
    add(pg, 1, "second");
    pager_enter(pg, 80, 24, 2);
    const char *o = capture_read();
    const char *first = strstr(o, "xxxx");
    ASSERT_TRUE(first != NULL);
    /* No newline between the long line's start and its end. */
    const char *nl = memchr(first, '\n', 399);
    ASSERT_TRUE(nl == NULL);
    ASSERT_TRUE(strstr(o, "second") != NULL);
    pager_free(pg);
}

TEST(load_disk_missing_session) {
    pager *pg = pager_new();
    ASSERT_EQ_INT(pager_load_disk(pg, "no-such-session-xyz"), -1);
    ASSERT_EQ_INT(pager_line_count(pg), 0);
    pager_free(pg);
}

int main(void) {
    RUN(dedup_by_seq);
    RUN(seq_gap_still_accepted);
    RUN(enter_sets_terminal_modes);
    RUN(leave_restores_terminal_modes);
    RUN(opens_at_the_bottom);
    RUN(movement_keys);
    RUN(movement_clamps_at_both_ends);
    RUN(arrow_and_page_keys);
    RUN(split_escape_sequence_across_reads);
    RUN(q_exits_and_lone_esc_exits_on_timeout);
    RUN(esc_timeout_is_noop_when_not_pending);
    RUN(search_finds_and_wraps_not);
    RUN(search_ignores_ansi_in_stored_text);
    RUN(search_prompt_editing);
    RUN(esc_at_prompt_cancels_search_not_pager);
    RUN(n_without_pattern_reports_it);
    RUN(status_line_reports_position);
    RUN(resize_reclamps_and_redraws);
    RUN(fewer_lines_than_rows);
    RUN(empty_scrollback_does_not_crash);
    RUN(tiny_terminal);
    RUN(ring_tail_arriving_after_enter_is_shown);
    RUN(scrolled_up_position_survives_new_lines);
    RUN(request_continuation);
    RUN(empty_reply_stops_requesting);
    RUN(no_disk_lines_requests_from_zero);
    RUN(disk_already_complete_requests_nothing);
    RUN(strip_ansi);
    RUN(long_line_is_not_wrapped_by_us);
    RUN(load_disk_missing_session);
    if (g_fd >= 0) close(g_fd);
    TEST_MAIN_END();
}
