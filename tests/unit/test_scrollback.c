/* test_scrollback.c — persistence, torn-write recovery, rotation, fetch. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "runner.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/scrollback.h"
#include "vt/vt.h"

static char g_home[256];

static void setup_home(void) {
    snprintf(g_home, sizeof g_home, "/tmp/at_sb_test_%d", (int)getpid());
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", g_home, g_home);
    if (system(cmd) != 0) exit(1);
    setenv("HOME", g_home, 1);
    unsetenv("XDG_RUNTIME_DIR");
}

static void make_line(vt_cell *cells, int n, const char *text) {
    memset(cells, 0, sizeof(vt_cell) * (size_t)n);
    for (int i = 0; i < n && text[i]; i++)
        cells[i] = (vt_cell){.cp = (uint32_t)text[i], .fg = VT_COLOR_DEFAULT,
                             .bg = VT_COLOR_DEFAULT};
}

typedef struct {
    char lines[64][128];
    int count;
} collect;

static void collect_cb(void *ud, uint64_t seq, const char *text, uint32_t len) {
    /* Keeps the LAST 64 lines (ring) so tail assertions work on long logs;
     * count is the total seen. */
    collect *c = ud;
    (void)seq;
    uint32_t n = len < 127 ? len : 127;
    memcpy(c->lines[c->count % 64], text, n);
    c->lines[c->count % 64][n] = '\0';
    c->count++;
}

TEST(push_and_read_back) {
    scrollback *sb = sb_open("t-basic", 100, 0);
    ASSERT_TRUE(sb != NULL);
    vt_cell cells[20];
    make_line(cells, 20, "line-one");
    sb_push_line(sb, cells, 20);
    make_line(cells, 20, "line-two");
    sb_push_line(sb, cells, 20);
    ASSERT_EQ_INT((long long)sb_total_lines(sb), 2);
    sb_close(sb);

    collect got = {0};
    int64_t n = sb_read_log("t-basic", collect_cb, &got);
    ASSERT_EQ_INT((long long)n, 2);
    ASSERT_TRUE(strstr(got.lines[0], "line-one") != NULL);
    ASSERT_TRUE(strstr(got.lines[1], "line-two") != NULL);
}

TEST(seq_resumes_after_reopen) {
    scrollback *sb = sb_open("t-resume", 100, 0);
    vt_cell cells[10];
    make_line(cells, 10, "aaa");
    sb_push_line(sb, cells, 10);
    sb_close(sb);

    sb = sb_open("t-resume", 100, 0);
    ASSERT_EQ_INT((long long)sb_total_lines(sb), 1); /* resumed, not reset */
    make_line(cells, 10, "bbb");
    sb_push_line(sb, cells, 10);
    sb_close(sb);

    collect got = {0};
    ASSERT_EQ_INT((long long)sb_read_log("t-resume", collect_cb, &got), 2);
}

TEST(torn_write_recovered) {
    scrollback *sb = sb_open("t-torn", 100, 0);
    vt_cell cells[10];
    make_line(cells, 10, "good-1");
    sb_push_line(sb, cells, 10);
    make_line(cells, 10, "good-2");
    sb_push_line(sb, cells, 10);
    sb_close(sb);

    /* Simulate a crash mid-write: append garbage bytes. */
    char path[512];
    snprintf(path, sizeof path, "%s/.agent-terminal/sessions/t-torn/scrollback.log", g_home);
    int fd = open(path, O_WRONLY | O_APPEND);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(write(fd, "\x30\x00\x00\x00GARBAGE-partial-record", 27) == 27);
    close(fd);

    /* Reader stops at the corrupt tail. */
    collect got = {0};
    ASSERT_EQ_INT((long long)sb_read_log("t-torn", collect_cb, &got), 2);

    /* Reopen for writing: torn tail truncated, appends stay valid. */
    sb = sb_open("t-torn", 100, 0);
    ASSERT_EQ_INT((long long)sb_total_lines(sb), 2);
    make_line(cells, 10, "good-3");
    sb_push_line(sb, cells, 10);
    sb_close(sb);
    collect got2 = {0};
    ASSERT_EQ_INT((long long)sb_read_log("t-torn", collect_cb, &got2), 3);
    ASSERT_TRUE(strstr(got2.lines[2], "good-3") != NULL);
}

TEST(rotation_caps_disk) {
    /* Tiny 4 KB cap forces several rotations. */
    scrollback *sb = sb_open("t-rot", 100, 4096);
    vt_cell cells[64];
    char text[64];
    for (int i = 0; i < 500; i++) {
        snprintf(text, sizeof text, "line-%04d-padding-padding-padding", i);
        make_line(cells, 64, text);
        sb_push_line(sb, cells, 64);
    }
    sb_close(sb);

    char path[512], path1[512];
    snprintf(path, sizeof path, "%s/.agent-terminal/sessions/t-rot/scrollback.log", g_home);
    snprintf(path1, sizeof path1, "%s/scrollback.log.1", "");
    snprintf(path1, sizeof path1, "%s/.agent-terminal/sessions/t-rot/scrollback.log.1", g_home);
    struct stat st;
    ASSERT_TRUE(stat(path, &st) == 0);
    ASSERT_TRUE(st.st_size <= 4096 + 8300); /* cap + one max record slack */
    ASSERT_TRUE(stat(path1, &st) == 0);     /* rotation happened */

    /* Reader sees old generation then new, in order; total < 500 (rotation
     * dropped the oldest) but the LAST line must be present. */
    collect got = {0};
    int64_t n = sb_read_log("t-rot", collect_cb, &got);
    ASSERT_TRUE(n > 0 && n < 500);
    ASSERT_TRUE(strstr(got.lines[(got.count - 1) % 64], "line-0499") != NULL);
}

TEST(fetch_from_ring) {
    scrollback *sb = sb_open("t-fetch", 8, 0); /* tiny ring: 8 lines */
    vt_cell cells[16];
    char text[32];
    for (int i = 0; i < 20; i++) {
        snprintf(text, sizeof text, "L%02d", i);
        make_line(cells, 16, text);
        sb_push_line(sb, cells, 16);
    }
    sb_line_ref refs[16];
    /* Ring holds seq 12..19; ask from 0 → get what's retained. */
    uint32_t got = sb_fetch(sb, 0, 16, refs, 16);
    ASSERT_EQ_INT(got, 8);
    ASSERT_EQ_INT((long long)refs[0].seq, 12);
    ASSERT_TRUE(strstr(refs[0].text, "L12") != NULL);
    /* Ask from 18 → 2 lines. */
    got = sb_fetch(sb, 18, 16, refs, 16);
    ASSERT_EQ_INT(got, 2);
    sb_close(sb);
}

TEST(sgr_survives_roundtrip) {
    scrollback *sb = sb_open("t-sgr", 100, 0);
    vt_cell cells[8];
    memset(cells, 0, sizeof cells);
    cells[0] = (vt_cell){.cp = 'R', .fg = VT_COLOR_IDX(1), .bg = VT_COLOR_DEFAULT,
                         .attrs = VT_ATTR_BOLD};
    cells[1] = (vt_cell){.cp = 'G', .fg = VT_COLOR_RGB(0x00FF00), .bg = VT_COLOR_DEFAULT};
    sb_push_line(sb, cells, 8);
    sb_close(sb);
    collect got = {0};
    sb_read_log("t-sgr", collect_cb, &got);
    ASSERT_TRUE(strstr(got.lines[0], "\x1b[1m") != NULL);        /* bold */
    ASSERT_TRUE(strstr(got.lines[0], "\x1b[38;5;1m") != NULL);   /* indexed */
    ASSERT_TRUE(strstr(got.lines[0], "\x1b[38;2;0;255;0m") != NULL); /* rgb */
    ASSERT_TRUE(strstr(got.lines[0], "\x1b[0m") != NULL);        /* reset at end */
}

TEST(combining_survives_roundtrip) {
    /* serialize_line() in scrollback.c is a SECOND, independent serializer
     * alongside vt_render.c's render_grid(). A combining mark emitted by one
     * and dropped by the other means scrollback silently loses marks that the
     * live screen shows, so this must be asserted here and not only in
     * test_vt.c. */
    scrollback *sb = sb_open("t-comb", 100, 0);
    vt_cell cells[8];
    memset(cells, 0, sizeof cells);
    /* e + U+0301 (2-byte mark), and a wide base + U+032D (2-byte mark) with
     * its spacer, which serialize_line skips. */
    cells[0] = (vt_cell){.cp = 'e', .fg = VT_COLOR_DEFAULT, .bg = VT_COLOR_DEFAULT,
                         .comb = 0x0301};
    cells[1] = (vt_cell){.cp = 0x4E2D, .fg = VT_COLOR_DEFAULT, .bg = VT_COLOR_DEFAULT,
                         .attrs = VT_ATTR_WIDE, .comb = 0x032D};
    cells[2] = (vt_cell){.cp = 0, .fg = VT_COLOR_DEFAULT, .bg = VT_COLOR_DEFAULT,
                         .attrs = VT_ATTR_WIDE_SPACER};
    /* A 3-byte mark, to cover the other encoding branch. U+1AB0 is a
     * combining doubled circumflex. */
    cells[3] = (vt_cell){.cp = 'z', .fg = VT_COLOR_DEFAULT, .bg = VT_COLOR_DEFAULT,
                         .comb = 0x1AB0};
    sb_push_line(sb, cells, 8);
    sb_close(sb);

    collect got = {0};
    ASSERT_EQ_INT((long long)sb_read_log("t-comb", collect_cb, &got), 1);
    /* Each mark must appear immediately after its own base, so assert on the
     * base+mark byte pair rather than on the mark alone. */
    ASSERT_TRUE(strstr(got.lines[0], "e\xcc\x81") != NULL);           /* U+0301 */
    ASSERT_TRUE(strstr(got.lines[0], "\xe4\xb8\xad\xcc\xad") != NULL); /* U+032D on wide */
    ASSERT_TRUE(strstr(got.lines[0], "z\xe1\xaa\xb0") != NULL);        /* U+1AB0, 3-byte */
    /* The spacer contributes no bytes of its own: the wide base's mark is
     * followed directly by the next cell's base. */
    ASSERT_TRUE(strstr(got.lines[0], "\xcc\xad" "z") != NULL);
}

TEST(list_logs) {
    char buf[4096];
    int n = sb_list_logs(buf, sizeof buf);
    ASSERT_TRUE(n >= 5); /* the sessions created above */
}

TEST(pane_zero_is_the_plain_log) {
    /* The refactor's compatibility claim: pane 0 writes the same
     * scrollback.log sb_open always wrote, provable by reading one through
     * the other in both directions. If pane 0 got its own filename, every
     * existing log — and `history`, which reads by session name only — would
     * silently come up empty. */
    scrollback *sb = sb_open_pane("t-pane0", 0, 100, 0);
    ASSERT_TRUE(sb != NULL);
    vt_cell cells[20];
    make_line(cells, 20, "via-pane-zero");
    sb_push_line(sb, cells, 20);
    sb_close(sb);

    collect got = {0};
    ASSERT_EQ_INT((long long)sb_read_log("t-pane0", collect_cb, &got), 1);
    ASSERT_TRUE(strstr(got.lines[0], "via-pane-zero") != NULL);

    /* And the reverse: sb_open appends to the same file, same seq stream. */
    sb = sb_open("t-pane0", 100, 0);
    ASSERT_TRUE(sb != NULL);
    ASSERT_EQ_INT((long long)sb_total_lines(sb), 1); /* resumed, not fresh */
    sb_close(sb);
}

TEST(pane_logs_are_isolated) {
    scrollback *p0 = sb_open_pane("t-panes", 0, 100, 0);
    scrollback *p3 = sb_open_pane("t-panes", 3, 100, 0);
    ASSERT_TRUE(p0 != NULL && p3 != NULL);
    vt_cell cells[20];
    make_line(cells, 20, "zero-only");
    sb_push_line(p0, cells, 20);
    make_line(cells, 20, "three-only");
    sb_push_line(p3, cells, 20);
    /* Independent seq streams, not interleaved halves of one. */
    ASSERT_EQ_INT((long long)sb_total_lines(p0), 1);
    ASSERT_EQ_INT((long long)sb_total_lines(p3), 1);
    sb_close(p0);
    sb_close(p3);

    /* The session-level reader sees pane 0 only: pane histories are separate
     * documents, not lines to merge (their seqs would collide). */
    collect got = {0};
    ASSERT_EQ_INT((long long)sb_read_log("t-panes", collect_cb, &got), 1);
    ASSERT_TRUE(strstr(got.lines[0], "zero-only") != NULL);

    /* Reopening pane 3 resumes its own numbering. */
    p3 = sb_open_pane("t-panes", 3, 100, 0);
    ASSERT_TRUE(p3 != NULL);
    ASSERT_EQ_INT((long long)sb_total_lines(p3), 1);
    sb_close(p3);
}

int main(void) {
    setup_home();
    RUN(push_and_read_back);
    RUN(seq_resumes_after_reopen);
    RUN(torn_write_recovered);
    RUN(rotation_caps_disk);
    RUN(fetch_from_ring);
    RUN(sgr_survives_roundtrip);
    RUN(combining_survives_roundtrip);
    RUN(list_logs);
    RUN(pane_zero_is_the_plain_log);
    RUN(pane_logs_are_isolated);
    TEST_MAIN_END();
}
