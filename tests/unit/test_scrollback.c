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

/* The regression this file did not have: a reopen (every daemon restart,
 * including an in-place `reload` handoff that keeps the sessions) used to leave
 * the ring EMPTY while sb_total_lines() still reported the whole history, so
 * MSG_SCROLLBACK_REQ — which can only read the ring — served nothing and the
 * GUI's scrollbar showed no history at all. Measured before the fix on an
 * isolated daemon: 2977 lines announced, 0 servable, 61,410 bytes on disk. */
TEST(ring_refilled_after_reopen) {
    scrollback *sb = sb_open("t-refill", 8, 0); /* tiny ring: 8 lines */
    vt_cell cells[16];
    char text[32];
    for (int i = 0; i < 20; i++) {
        snprintf(text, sizeof text, "L%02d", i);
        make_line(cells, 16, text);
        sb_push_line(sb, cells, 16);
    }
    sb_close(sb);

    sb = sb_open("t-refill", 8, 0);
    ASSERT_TRUE(sb != NULL);
    ASSERT_EQ_INT((long long)sb_total_lines(sb), 20); /* unchanged by the refill */
    sb_line_ref refs[16];
    uint32_t got = sb_fetch(sb, 0, 16, refs, 16);
    /* Byte-for-byte what the live ring served in fetch_from_ring above: a
     * rebuilt ring must be indistinguishable from one filled by running. */
    ASSERT_EQ_INT(got, 8);
    ASSERT_EQ_INT((long long)refs[0].seq, 12);
    ASSERT_TRUE(strstr(refs[0].text, "L12") != NULL);
    ASSERT_EQ_INT((long long)refs[7].seq, 19);
    ASSERT_TRUE(strstr(refs[7].text, "L19") != NULL);

    /* Appending after a refill must continue the seq stream, not collide with
     * it: the refilled lines carry seqs 12..19 and next_seq is 20. */
    make_line(cells, 16, "after");
    sb_push_line(sb, cells, 16);
    got = sb_fetch(sb, 0, 16, refs, 16);
    ASSERT_EQ_INT(got, 8);
    ASSERT_EQ_INT((long long)refs[0].seq, 13);
    ASSERT_EQ_INT((long long)refs[7].seq, 20);
    ASSERT_TRUE(strstr(refs[7].text, "after") != NULL);
    sb_close(sb);
}

/* The refill parses a bounded WINDOW, not the file: scan_log hands back the
 * offset where the last mem_lines records start. With 4000 lines on disk and a
 * ring of 8, a whole-file refill would parse 4000 records to keep 8 — that cost
 * +605 ms of reload latency on a real 19.4 MB / 320k-line log. Correctness is
 * what is asserted here (the tail, and only the tail, in order); the timing
 * lives in the perf probe. */
TEST(refill_bounded_to_ring_window) {
    scrollback *sb = sb_open("t-window", 8, 0);
    vt_cell cells[16];
    char text[32];
    for (int i = 0; i < 4000; i++) {
        snprintf(text, sizeof text, "L%04d", i);
        make_line(cells, 16, text);
        sb_push_line(sb, cells, 16);
    }
    sb_close(sb);

    sb = sb_open("t-window", 8, 0);
    sb_line_ref refs[16];
    uint32_t got = sb_fetch(sb, 0, 16, refs, 16);
    ASSERT_EQ_INT(got, 8);
    ASSERT_EQ_INT((long long)refs[0].seq, 3992);
    ASSERT_TRUE(strstr(refs[0].text, "L3992") != NULL);
    ASSERT_EQ_INT((long long)refs[7].seq, 3999);
    for (uint32_t i = 1; i < got; i++) ASSERT_TRUE(refs[i].seq == refs[i - 1].seq + 1);
    sb_close(sb);
}

/* A ring wider than one generation must be filled from BOTH files, in age
 * order — the window that `.1` contributes is exactly the case a
 * current-file-only refill would silently truncate. */
TEST(refill_spans_rotation) {
    scrollback *sb = sb_open("t-refillrot", 100, 4096); /* forces rotations */
    vt_cell cells[64];
    char text[64];
    for (int i = 0; i < 500; i++) {
        snprintf(text, sizeof text, "line-%04d-padding-padding-padding", i);
        make_line(cells, 64, text);
        sb_push_line(sb, cells, 64);
    }
    sb_close(sb);

    /* How many lines the two generations still hold, measured not assumed. */
    collect on_disk = {0};
    int64_t disk_lines = sb_read_log("t-refillrot", collect_cb, &on_disk);
    ASSERT_TRUE(disk_lines > 0);

    sb = sb_open("t-refillrot", 100, 4096);
    sb_line_ref refs[128];
    uint32_t got = sb_fetch(sb, 0, 128, refs, 128);
    uint32_t want = disk_lines < 100 ? (uint32_t)disk_lines : 100;
    ASSERT_EQ_INT(got, want);
    ASSERT_EQ_INT((long long)refs[got - 1].seq, 499); /* newest survives */
    for (uint32_t i = 1; i < got; i++) ASSERT_TRUE(refs[i].seq == refs[i - 1].seq + 1);
    ASSERT_TRUE(strstr(refs[got - 1].text, "line-0499") != NULL);
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

/* ---- sb_fetch_deep: ranges older than the ring, served from the log ----
 *
 * The ring holds mem_lines; the daemon announces sb_total_lines(). Everything
 * between the two used to be announced and then unservable — #85 refilled the
 * ring so the newest mem_lines work, and these cover the rest.
 *
 * Every helper below writes each line's own seq into its text ("D0123"), so an
 * assertion on content is also an assertion on which record was read: a fetch
 * that returned the right COUNT from the wrong offset cannot pass. */
#define DEEP_LINES 3000u

static void deep_push(scrollback *sb, uint64_t from, uint64_t to) {
    vt_cell cells[16];
    char text[32];
    for (uint64_t i = from; i < to; i++) {
        snprintf(text, sizeof text, "D%04llu", (unsigned long long)i);
        make_line(cells, 16, text);
        sb_push_line(sb, cells, 16);
    }
}

/* Assert refs[i] is exactly the record whose seq is base+i, by seq AND by the
 * seq printed inside its payload. Disk-served text is not NUL-terminated (it
 * borrows the caller's buffer), so compare with a length. */
static int deep_is(const sb_line_ref *r, uint64_t seq) {
    char want[32];
    int n = snprintf(want, sizeof want, "D%04llu", (unsigned long long)seq);
    return r->seq == seq && r->len == (uint32_t)n && memcmp(r->text, want, (size_t)n) == 0;
}

/* The index has TWO producers — sb_push_line while the daemon runs, and
 * scan_log when it opens an existing log — and only the second one serves the
 * case this feature exists for, since a GUI scrolling into deep history is
 * usually doing it after a restart or a `reload`. So every check below runs
 * twice: once against the live index, then once against the same log reopened.
 * Found by mutation: with the checks running only against the live index, a
 * mutant that corrupted every scan_log offset survived all of them. */
static void deep_check_below_ring(scrollback *sb) {
    static sb_line_ref refs[1024];
    static char buf[1 << 16];

    /* First claim, and it has to come first: the ring cannot be the source.
     * sb_fetch from 0 returns the ring's oldest line, which must be strictly
     * ABOVE the whole range asked for below. Without this, "1000 lines came
     * back" would also pass against a fetch that served the ring's tail. */
    uint32_t ring = sb_fetch(sb, 0, 1, refs, 1);
    ASSERT_EQ_INT(ring, 1);
    ASSERT_EQ_INT((long long)refs[0].seq, DEEP_LINES - 100); /* 2900 > 999 */

    uint32_t got = sb_fetch_deep(sb, 0, 1000, refs, 1024, buf, sizeof buf);
    ASSERT_EQ_INT(got, 1000);
    ASSERT_TRUE(deep_is(&refs[0], 0));
    ASSERT_TRUE(deep_is(&refs[999], 999));
    /* Contiguous and ascending throughout, not just at the two ends. */
    for (uint32_t i = 0; i < got; i++) ASSERT_TRUE(deep_is(&refs[i], i));
}

TEST(deep_fetch_below_ring) {
    scrollback *sb = sb_open("t-deep", 100, 0); /* ring: 100 of 3000 lines */
    ASSERT_TRUE(sb != NULL);
    deep_push(sb, 0, DEEP_LINES);
    deep_check_below_ring(sb); /* index built by sb_push_line */
    sb_close(sb);

    sb = sb_open("t-deep", 100, 0);
    ASSERT_TRUE(sb != NULL);
    ASSERT_EQ_INT((long long)sb_total_lines(sb), DEEP_LINES);
    deep_check_below_ring(sb); /* index rebuilt by scan_log */
    sb_close(sb);
}

static void deep_check_boundaries(scrollback *sb) {
    static sb_line_ref refs[8];
    static char buf[4096];
    /* An exact index entry, one below, one above, the first record, and the
     * last record still below the ring. Off-by-one in the binary search or in
     * the derived first_seq + i*STEP shows up here and nowhere else, because a
     * fetch that starts one record late still returns a full, valid page. */
    const uint64_t at[] = {0, 1, SB_INDEX_STEP - 1, SB_INDEX_STEP, SB_INDEX_STEP + 1,
                           2 * SB_INDEX_STEP - 1, 2 * SB_INDEX_STEP,
                           4 * SB_INDEX_STEP, DEEP_LINES - 101};
    for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {
        uint32_t got = sb_fetch_deep(sb, at[k], 3, refs, 8, buf, sizeof buf);
        ASSERT_EQ_INT(got, 3);
        ASSERT_TRUE(deep_is(&refs[0], at[k]));
        ASSERT_TRUE(deep_is(&refs[1], at[k] + 1));
        ASSERT_TRUE(deep_is(&refs[2], at[k] + 2));
    }
}

TEST(deep_fetch_index_boundaries) {
    scrollback *sb = sb_open("t-deepidx", 100, 0);
    deep_push(sb, 0, DEEP_LINES);
    deep_check_boundaries(sb); /* index built by sb_push_line */
    sb_close(sb);

    sb = sb_open("t-deepidx", 100, 0);
    deep_check_boundaries(sb); /* index rebuilt by scan_log */
    sb_close(sb);
}

/* The counter assertions, run against one scrollback whose index came from
 * whichever producer the caller set up. `logsize` is the file the fetch has to
 * beat. */
static void deep_check_bounded_work(scrollback *sb, off_t logsize) {
    uint64_t bytes0 = 0, swept0 = 0;
    sb_fetch_stats(sb, &bytes0, &swept0);
    ASSERT_EQ_INT((long long)bytes0, 0); /* nothing read from disk yet */
    ASSERT_EQ_INT((long long)swept0, 0);

    static sb_line_ref refs[128];
    static char buf[8192];
    const uint64_t start = 1000;
    uint32_t got = sb_fetch_deep(sb, start, 100, refs, 128, buf, sizeof buf);
    ASSERT_EQ_INT(got, 100);
    ASSERT_TRUE(deep_is(&refs[0], start));

    uint64_t bytes = 0, swept = 0;
    sb_fetch_stats(sb, &bytes, &swept);
    /* Exact: the index places the seek at record start - (start % STEP), so the
     * sweep is that remainder and nothing more. 1000 - 512 = 488. Written as
     * arithmetic on SB_INDEX_STEP rather than as the number, so the assertion
     * follows the constant if the constant moves. */
    ASSERT_EQ_INT((long long)swept, (long long)(start % SB_INDEX_STEP));
    ASSERT_TRUE(swept < SB_INDEX_STEP); /* the bound the design promises */
    /* And the volume: 588 records of a 3000-record file. A fallback sweep from
     * offset 0 would read 1100 records plus the whole seek, i.e. past half. */
    ASSERT_TRUE((long long)bytes < logsize / 2);
    ASSERT_TRUE(bytes > 0); /* it really did go to disk */

    /* The other half of the contract: anything the ring holds is served from
     * the ring, at zero I/O. Asserted at the exact boundary — the ring's oldest
     * seq — because that is the one request a >=/> slip sends to disk, and it
     * would still return the right lines from there. The counters are the only
     * witness. */
    uint32_t r = sb_fetch_deep(sb, DEEP_LINES - 100, 100, refs, 128, buf, sizeof buf);
    ASSERT_EQ_INT(r, 100);
    ASSERT_TRUE(deep_is(&refs[0], DEEP_LINES - 100));
    uint64_t bytes2 = 0, swept2 = 0;
    sb_fetch_stats(sb, &bytes2, &swept2);
    ASSERT_EQ_INT((long long)bytes2, (long long)bytes);
    ASSERT_EQ_INT((long long)swept2, (long long)swept);
}

TEST(deep_fetch_bounded_work) {
    /* The test that makes the others worth anything. A broken index is
     * INVISIBLE to every assertion above: a wrong offset is rejected by
     * rec_len+CRC, the fallback sweeps the generation from 0, and the right
     * lines come back — after reading the whole file. So assert the WORK.
     *
     * Both producers, for the reason given above deep_check_below_ring: the
     * mutant that shifts every scan_log offset by one byte is the whole point of
     * this test, and it is invisible unless the index under test was built by
     * scan_log. */
    scrollback *sb = sb_open("t-deepwork", 100, 0);
    deep_push(sb, 0, DEEP_LINES);
    sb_flush(sb);

    char path[512];
    snprintf(path, sizeof path,
             "%s/.agent-terminal/sessions/t-deepwork/scrollback.log", g_home);
    struct stat st;
    ASSERT_TRUE(stat(path, &st) == 0);
    /* Assert the log is genuinely big first, so the margin cannot be satisfied
     * by an accidentally tiny file. 3000 records x (16 B header + 5 B payload)
     * = 63,000 B minimum. */
    ASSERT_TRUE(st.st_size >= 63000);

    deep_check_bounded_work(sb, st.st_size); /* index built by sb_push_line */
    sb_close(sb);

    sb = sb_open("t-deepwork", 100, 0);
    ASSERT_TRUE(sb != NULL);
    deep_check_bounded_work(sb, st.st_size); /* index rebuilt by scan_log */
    sb_close(sb);
}

TEST(deep_fetch_spans_generations) {
    /* One rotation, nothing dropped: .log.1 holds the older seqs, .log the
     * newer, and a single fetch must cross the seam without a gap, a repeat, or
     * a reordering. 64 KB cap over ~21 B records rotates at ~3100. */
    scrollback *sb = sb_open("t-deepgen", 100, 64 * 1024);
    deep_push(sb, 0, 4000);
    sb_close(sb);

    char path1[512];
    snprintf(path1, sizeof path1,
             "%s/.agent-terminal/sessions/t-deepgen/scrollback.log.1", g_home);
    struct stat st;
    ASSERT_TRUE(stat(path1, &st) == 0); /* rotation happened */

    sb = sb_open("t-deepgen", 100, 64 * 1024);
    /* Exactly one rotation, so nothing was dropped and the seam is interior. */
    collect all = {0};
    ASSERT_EQ_INT((long long)sb_read_log("t-deepgen", collect_cb, &all), 4000);

    static sb_line_ref refs[4096];
    static char buf[1 << 17];
    uint32_t got = sb_fetch_deep(sb, 0, 3800, refs, 4096, buf, sizeof buf);
    ASSERT_EQ_INT(got, 3800);
    for (uint32_t i = 0; i < got; i++) ASSERT_TRUE(deep_is(&refs[i], i));
    sb_close(sb);
}

TEST(deep_fetch_after_rotation_of_the_index) {
    /* The range lives entirely in .log.1. This is the test that catches
     * "rotation forgot to move the index": with idx_old empty the old
     * generation is skipped and the fetch answers with .log's seqs instead —
     * a full page of valid lines, all of them wrong. */
    scrollback *sb = sb_open("t-deeprot2", 100, 64 * 1024);
    deep_push(sb, 0, 4000);
    static sb_line_ref refs[256];
    static char buf[8192];
    uint32_t got = sb_fetch_deep(sb, 100, 200, refs, 256, buf, sizeof buf);
    ASSERT_EQ_INT(got, 200);
    for (uint32_t i = 0; i < got; i++) ASSERT_TRUE(deep_is(&refs[i], 100 + i));
    sb_close(sb);
}

TEST(deep_fetch_partial_and_empty) {
    scrollback *sb = sb_open("t-deepedge", 100, 4096); /* many rotations */
    deep_push(sb, 0, DEEP_LINES);
    static sb_line_ref refs[256];
    static char buf[8192];

    /* Repeated rotation dropped the oldest generations, so seq 0 is gone. A
     * request from 0 must clamp UP to the oldest surviving record rather than
     * returning nothing or reading past a file's start. */
    uint32_t got = sb_fetch_deep(sb, 0, 200, refs, 256, buf, sizeof buf);
    ASSERT_TRUE(got > 0);
    ASSERT_TRUE(refs[0].seq > 0);
    ASSERT_TRUE(deep_is(&refs[0], refs[0].seq));
    for (uint32_t i = 1; i < got; i++) ASSERT_TRUE(deep_is(&refs[i], refs[0].seq + i));

    /* Entirely past the newest line: well-defined empty, not a read past EOF. */
    ASSERT_EQ_INT(sb_fetch_deep(sb, DEEP_LINES + 1000, 200, refs, 256, buf, sizeof buf), 0);

    /* A text buffer far too small serves fewer lines; it is never overrun
     * (ASan is the assertion here). Each line is 5 payload bytes. */
    uint32_t few = sb_fetch_deep(sb, refs[0].seq, 200, refs, 256, buf, 12);
    ASSERT_TRUE(few >= 1 && few <= 3);
    sb_close(sb);
}

TEST(deep_fetch_torn_tail) {
    /* A crash-truncated log must not leave an index entry pointing past
     * valid_size: the scan builds the index and the truncation point on the
     * same pass, so a fetch after recovery has to stop at the last good
     * record. 700 lines puts an index entry (512) inside the surviving range. */
    scrollback *sb = sb_open("t-deeptorn", 100, 0);
    deep_push(sb, 0, 700);
    sb_close(sb);

    char path[512];
    snprintf(path, sizeof path,
             "%s/.agent-terminal/sessions/t-deeptorn/scrollback.log", g_home);
    int fd = open(path, O_WRONLY | O_APPEND);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(write(fd, "\x30\x00\x00\x00GARBAGE-partial-record", 27) == 27);
    close(fd);

    sb = sb_open("t-deeptorn", 100, 0);
    ASSERT_EQ_INT((long long)sb_total_lines(sb), 700);
    static sb_line_ref refs[1024];
    static char buf[1 << 16];
    uint32_t got = sb_fetch_deep(sb, 0, 1000, refs, 1024, buf, sizeof buf);
    ASSERT_EQ_INT(got, 700); /* every good record, and nothing past them */
    for (uint32_t i = 0; i < got; i++) ASSERT_TRUE(deep_is(&refs[i], i));
    /* From an exact index entry near the recovered tail. */
    got = sb_fetch_deep(sb, SB_INDEX_STEP, 1000, refs, 1024, buf, sizeof buf);
    ASSERT_EQ_INT(got, (long long)(700 - SB_INDEX_STEP));
    ASSERT_TRUE(deep_is(&refs[0], SB_INDEX_STEP));
    sb_close(sb);
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
    RUN(ring_refilled_after_reopen);
    RUN(refill_bounded_to_ring_window);
    RUN(refill_spans_rotation);
    RUN(deep_fetch_below_ring);
    RUN(deep_fetch_index_boundaries);
    RUN(deep_fetch_bounded_work);
    RUN(deep_fetch_spans_generations);
    RUN(deep_fetch_after_rotation_of_the_index);
    RUN(deep_fetch_partial_and_empty);
    RUN(deep_fetch_torn_tail);
    RUN(sgr_survives_roundtrip);
    RUN(combining_survives_roundtrip);
    RUN(list_logs);
    RUN(pane_zero_is_the_plain_log);
    RUN(pane_logs_are_isolated);
    TEST_MAIN_END();
}
