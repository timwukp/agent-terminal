/* test_vt.c — table-driven VT engine tests: bytes in → expected grid out.
 * Also property-tests boundary chunking: every case is re-fed 1 byte at a
 * time and must produce an identical grid. */
#include "runner.h"

#include <stdlib.h>

#include "vt/vt.h"

#define ROWS 6
#define COLS 10

static size_t put_utf8(char *out, uint32_t cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* Render a row as UTF-8 text (cp==0 → '.', spacer → '_') for assertions.
 * A cell's combining mark is emitted right after its base, so an expectation
 * string shows the grapheme a terminal would draw. Without that, an assertion
 * on a base character cannot tell an attached mark from a dropped one. */
static void row_text(const vt *v, uint16_t row, char *out, size_t outsz) {
    const vt_cell *line = vt_line(v, row);
    size_t o = 0;
    uint16_t rows, cols;
    vt_get_size(v, &rows, &cols);
    for (uint16_t c = 0; c < cols; c++) {
        uint32_t cp = line[c].cp;
        /* Worst case for one cell: 4-byte base + 3-byte BMP mark, plus NUL. */
        if (o + 8 > outsz) break;
        if (line[c].attrs & VT_ATTR_WIDE_SPACER) { out[o++] = '_'; continue; }
        if (cp == 0 && line[c].comb == 0) { out[o++] = '.'; continue; }
        o += put_utf8(out + o, cp ? cp : ' ');
        if (line[c].comb) o += put_utf8(out + o, line[c].comb);
    }
    out[o] = '\0';
}

typedef struct {
    const char *name;
    const char *input;      /* bytes fed to vt_feed */
    int check_row;          /* row to assert */
    const char *expect;     /* expected row_text */
    int cur_row, cur_col;   /* expected cursor, -1 = don't check */
} vt_case;

static const vt_case CASES[] = {
    {"plain text", "hello", 0, "hello.....", 0, 5},
    {"cr lf", "ab\r\ncd", 1, "cd........", 1, 2},
    {"cup", "\x1b[3;4Hx", 2, "...x......", 2, 4},
    {"cup clamped", "\x1b[99;99Hz", ROWS - 1, ".........z", ROWS - 1, COLS - 1},
    {"cuu cud cuf cub", "\x1b[3;3H\x1b[A\x1b[B\x1b[2C\x1b[Dx", 2, "...x......", 2, 4},
    {"el0", "abcdef\x1b[1;3H\x1b[K", 0, "ab........", 0, 2},
    {"el1", "abcdef\x1b[1;3H\x1b[1K", 0, "...def....", 0, 2},
    {"el2", "abcdef\x1b[1;3H\x1b[2K", 0, "..........", 0, 2},
    {"ed0", "ab\r\ncd\x1b[1;1H\x1b[J", 1, "..........", 0, 0},
    {"ed2", "ab\r\ncd\x1b[2J", 1, "..........", -1, -1},
    {"ich", "abcd\x1b[1;2H\x1b[2@", 0, "a..bcd....", 0, 1},
    {"dch", "abcdef\x1b[1;2H\x1b[2P", 0, "adef......", 0, 1},
    {"ech", "abcdef\x1b[1;2H\x1b[3X", 0, "a...ef....", 0, 1},
    {"il", "aa\r\nbb\r\ncc\x1b[2;1H\x1b[L", 2, "bb........", 1, 0},
    {"dl", "aa\r\nbb\r\ncc\x1b[2;1H\x1b[M", 1, "cc........", 1, 0},
    {"autowrap", "0123456789X", 1, "X.........", 1, 1},
    {"wrap disabled", "\x1b[?7l0123456789XY\x1b[?7h", 0, "012345678Y", 0, COLS - 1},
    {"tab", "\tx", 0, "........x.", 0, 9},
    {"backspace", "abc\bX", 0, "abX.......", 0, 3},
    {"sgr passthrough text", "\x1b[1;31mred\x1b[0m", 0, "red.......", 0, 3},
    /* region 2-4; B at row 3 (bottom), LF scrolls region: B moves to row 2 */
    {"scroll region up", "\x1b[2;4rA\x1b[4;1HB\nC", 2, "B.........",  -1, -1},
    {"ri at top scrolls down", "a\r\nb\x1b[1;1H\x1bMx", 1, "a.........", 0, 1},
    {"nel", "ab\x1b" "Ecd", 1, "cd........", 1, 2},
    {"dec graphics", "\x1b(0qqq\x1b(B", 0, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80.......", 0, 3},
    {"utf8 cjk wide", "\xe4\xb8\xad", 0, "\xe4\xb8\xad_........", 0, 2},
    {"wide at margin wraps", "\x1b[1;10H\xe4\xb8\xad", 1, "\xe4\xb8\xad_........", 1, 2},
    /* Was "combining dropped" in v1: the mark now attaches to its base. The
     * cursor still lands at column 2, because a mark advances nothing. */
    {"combining attaches", "e\xcc\x81x", 0, "e\xcc\x81x........", 0, 2},
    {"combining after wide char", "\xe4\xb8\xad\xcc\x81", 0,
     "\xe4\xb8\xad\xcc\x81_........", 0, 2},
    /* A mark with no base has nothing to attach to and is dropped. */
    {"combining with no base", "\xcc\x81x", 0, "x.........", 0, 1},
    /* Only the first mark on a base is stored; the second is dropped. */
    {"second combiner dropped", "e\xcc\x81\xcc\x82x", 0, "e\xcc\x81x........", 0, 2},
    /* At the right margin the cursor stays put with pending_wrap set, so
     * cur.col does not identify the base — last-written-cell tracking does. */
    {"combining at right margin", "012345678X\xcc\x81", 0, "012345678X\xcc\x81", 0, 9},
    /* Marks outside the BMP do not fit a 16-bit cell field and are dropped;
     * U+1D167 is a combining musical notation mark. */
    {"non-bmp combiner dropped", "e\xf0\x9d\x85\xa7x", 0, "ex........", 0, 2},
    /* Zero-width but not attachable: each is consumed and dropped, and must
     * not detach a mark that follows it from the base before it. */
    {"zwj not attached", "e\xe2\x80\x8dx", 0, "ex........", 0, 2},
    {"variation selector not attached", "e\xef\xb8\x8fx", 0, "ex........", 0, 2},
    {"bidi control not attached", "e\xe2\x80\xaax", 0, "ex........", 0, 2},
    {"zwj between base and mark", "e\xe2\x80\x8d\xcc\x81x", 0, "e\xcc\x81x........", 0, 2},
    /* Cursor movement between a base and a mark detaches it. */
    {"cr detaches combiner", "e\r\xcc\x81", 0, "e.........", 0, 0},
    {"cup detaches combiner", "e\x1b[1;5H\xcc\x81", 0, "e.........", 0, 4},
    {"backspace detaches combiner", "e\b\xcc\x81", 0, "e.........", 0, 0},
    {"tab detaches combiner", "e\t\xcc\x81", 0, "e.........", 0, 8},
    /* Erasing a base drops the mark it carried. */
    {"erase drops combiner", "e\xcc\x81\x1b[1;1H\x1b[K", 0, "..........", 0, 0},
    /* Overwriting a base with a plain char must not inherit its mark. */
    {"overwrite drops combiner", "e\xcc\x81\x1b[1;1Hz", 0, "z.........", 0, 1},
    /* REP repeats the whole grapheme, not just the base. */
    {"rep repeats grapheme", "e\xcc\x81\x1b[2b", 0,
     "e\xcc\x81" "e\xcc\x81" "e\xcc\x81" ".......", 0, 3},
    {"invalid utf8 fffd", "\xff", 0, "\xef\xbf\xbd.........", 0, 1},
    {"overlong rejected", "\xc0\xafz", 0, "\xef\xbf\xbd\xef\xbf\xbdz.......", -1, -1},
    {"osc title ignored on grid", "\x1b]2;my title\x07x", 0, "x.........", 0, 1},
    {"osc esc-st terminated", "\x1b]0;t\x1b\\y", 0, "y.........", 0, 1},
    /* 0x9c is C1 ST in a 7/8-bit locale but a plain UTF-8 continuation byte
     * here, and it appears in 4.5% of all codepoints (U+672C 本, U+00DC Ü,
     * U+D55C 한, ...). Treating it as a control drops the character and, worse,
     * ends an OSC string early so the rest of a title leaks onto the grid. */
    {"utf8 cont byte 9c not ST", "\xe6\x9c\xac", 0, "\xe6\x9c\xac_........", 0, 2},
    {"9c in 2-byte seq", "\xc3\x9c" "z", 0, "\xc3\x9cz........", 0, 2},
    {"9c in 4-byte seq", "\xf0\x9f\x8c\x9c", 0, "\xf0\x9f\x8c\x9c_........", 0, 2},
    {"9c inside osc title", "\x1b]0;\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\x1b\\y", 0, "y.........", 0, 1},
    {"csi ignore garbage", "\x1b[1:2:3mx", 0, "x.........", 0, 1},
    {"can aborts csi", "\x1b[3\x18x", 0, "x.........", 0, 1},
    {"rep", "ab\x1b[3b", 0, "abbbb.....", 0, 5},
    {"vpa", "\x1b[4dx", 3, "x.........", 3, 1},
    {"cha", "abc\x1b[2Gx", 0, "axc.......", 0, 2},
    {"decsc decrc", "ab\x1b" "7\r\nxy\x1b" "8Z", 0, "abZ.......", 0, 3},
    {"decaln", "\x1b#8", 3, "EEEEEEEEEE", -1, -1},
    {"ris clears", "abc\x1b" "c", 0, "..........", 0, 0},
};

TEST(table_cases) {
    for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        const vt_case *tc = &CASES[i];
        vt *v = vt_new(ROWS, COLS, NULL, NULL);
        vt_feed(v, (const uint8_t *)tc->input, strlen(tc->input));
        char got[COLS * 8 + 1];
        row_text(v, (uint16_t)tc->check_row, got, sizeof got);
        t_checks++;
        if (strcmp(got, tc->expect) != 0) {
            t_failures++;
            fprintf(stderr, "FAIL [%s] row %d: got '%s' want '%s'\n", tc->name,
                    tc->check_row, got, tc->expect);
        }
        if (tc->cur_row >= 0) {
            uint16_t cr, cc;
            vt_get_cursor(v, &cr, &cc, NULL);
            t_checks++;
            if (cr != tc->cur_row || cc != tc->cur_col) {
                t_failures++;
                fprintf(stderr, "FAIL [%s] cursor: got %u,%u want %d,%d\n",
                        tc->name, cr, cc, tc->cur_row, tc->cur_col);
            }
        }
        vt_free(v);
    }
}

TEST(chunking_property) {
    /* Feeding byte-by-byte must equal feeding all at once — the boundary-
     * split property that fuzzing also targets. */
    for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        const vt_case *tc = &CASES[i];
        vt *whole = vt_new(ROWS, COLS, NULL, NULL);
        vt *split = vt_new(ROWS, COLS, NULL, NULL);
        size_t len = strlen(tc->input);
        vt_feed(whole, (const uint8_t *)tc->input, len);
        for (size_t j = 0; j < len; j++)
            vt_feed(split, (const uint8_t *)tc->input + j, 1);
        for (uint16_t r = 0; r < ROWS; r++) {
            char a[COLS * 8 + 1], b[COLS * 8 + 1];
            row_text(whole, r, a, sizeof a);
            row_text(split, r, b, sizeof b);
            t_checks++;
            if (strcmp(a, b) != 0) {
                t_failures++;
                fprintf(stderr, "FAIL chunking [%s] row %u: '%s' != '%s'\n",
                        tc->name, r, a, b);
            }
        }
        vt_free(whole);
        vt_free(split);
    }
}

TEST(alt_screen_roundtrip) {
    vt *v = vt_new(ROWS, COLS, NULL, NULL);
    vt_feed(v, (const uint8_t *)"primary", 7);
    vt_feed(v, (const uint8_t *)"\x1b[?1049h", 8);
    ASSERT_TRUE(vt_get_modes(v) & VT_MODE_ALTSCREEN);
    char row[COLS * 8 + 1];
    row_text(v, 0, row, sizeof row);
    ASSERT_TRUE(strcmp(row, "..........") == 0); /* alt starts clear */
    vt_feed(v, (const uint8_t *)"alt!", 4);
    vt_feed(v, (const uint8_t *)"\x1b[?1049l", 8);
    ASSERT_TRUE(!(vt_get_modes(v) & VT_MODE_ALTSCREEN));
    row_text(v, 0, row, sizeof row);
    ASSERT_TRUE(strcmp(row, "primary...") == 0); /* primary restored */
    uint16_t cr, cc;
    vt_get_cursor(v, &cr, &cc, NULL);
    ASSERT_EQ_INT(cc, 7); /* 1049 restores saved cursor */
    vt_free(v);
}

static char g_resp[256];
static size_t g_resp_len;
static void resp_cb(void *ud, const char *buf, size_t len) {
    (void)ud;
    if (g_resp_len + len < sizeof g_resp) {
        memcpy(g_resp + g_resp_len, buf, len);
        g_resp_len += len;
    }
}

TEST(query_responses) {
    vt_callbacks cb = {.on_response = resp_cb};
    vt *v = vt_new(ROWS, COLS, &cb, NULL);

    g_resp_len = 0;
    vt_feed(v, (const uint8_t *)"\x1b[6n", 4); /* CPR at 1,1 */
    ASSERT_EQ_INT(g_resp_len, 6);
    ASSERT_EQ_MEM(g_resp, "\x1b[1;1R", 6);

    g_resp_len = 0;
    vt_feed(v, (const uint8_t *)"\x1b[3;5H\x1b[6n", 10);
    ASSERT_EQ_MEM(g_resp, "\x1b[3;5R", 6);

    g_resp_len = 0;
    vt_feed(v, (const uint8_t *)"\x1b[c", 3); /* DA1 */
    ASSERT_TRUE(g_resp_len > 0 && g_resp[0] == '\x1b');

    g_resp_len = 0;
    vt_feed(v, (const uint8_t *)"\x1b[?2004h\x1b[?2004$p", 17); /* DECRQM */
    ASSERT_EQ_MEM(g_resp, "\x1b[?2004;1$y", 11);
    vt_free(v);
}

TEST(scrollback_emitted) {
    static int sb_lines;
    sb_lines = 0;
    vt_callbacks cb = {0};
    cb.on_scrollback_line = NULL;
    vt *v = vt_new(2, 5, &cb, NULL);
    /* Fill and overflow a 2-row screen; no callback set → must not crash. */
    vt_feed(v, (const uint8_t *)"a\r\nb\r\nc\r\nd", 10);
    char row[32];
    row_text(v, 0, row, sizeof row);
    ASSERT_TRUE(strcmp(row, "c....") == 0);
    vt_free(v);
    (void)sb_lines;
}

static int g_sb_calls;
static void sb_count_cb(void *ud, const vt_cell *cells, uint16_t n) {
    (void)ud; (void)cells; (void)n;
    g_sb_calls++;
}

/* Replaying a snapshot must be inert: it may not provoke a device response and
 * may not push scrollback.
 *
 * Both matter for the restart handoff. A response would be an answer to a query
 * the app made *before* the restart and already received, so the duplicate
 * arrives as unsolicited keyboard input — a literal `[12;40R` typed into a shell
 * prompt. A scrollback push would duplicate history that the pre-restart engine
 * already stored and `history`/copy-mode both read.
 *
 * The daemon does guard both callbacks during import (handoff_importing() in
 * session.c), but that guard is defense in depth and cannot be observed by an
 * integration test: with the guard removed, a full restart still shows zero
 * invocations of either callback and identical scrollback counts. This test pins
 * the reason why — the property lives in vt_snapshot, and it is the property
 * that would silently regress. A snapshot that started emitting a query, or one
 * whose row painting scrolled the grid instead of addressing rows absolutely,
 * would break the handoff and nothing else in the suite would notice.
 *
 * The scene deliberately includes a query and enough rows to overflow the grid
 * so that both mechanisms are live in the source engine. */
TEST(snapshot_replay_is_inert) {
    vt_callbacks cb = {.on_response = resp_cb, .on_scrollback_line = sb_count_cb};
    vt *v = vt_new(4, 10, &cb, NULL);

    g_resp_len = 0;
    g_sb_calls = 0;
    /* strlen, not a hand-counted literal: a count one byte short silently ate
     * the `n` that completes the CPR query, so the precondition below failed for
     * a reason that had nothing to do with the property under test. */
    const char *scene = "r1\r\nr2\r\nr3\r\nr4\r\nr5\r\nr6\x1b[6n";
    vt_feed(v, (const uint8_t *)scene, strlen(scene));
    /* Precondition: the scene really did exercise both callbacks, or the
     * assertions below would hold for a snapshot of an empty screen. */
    ASSERT_TRUE(g_resp_len > 0);
    ASSERT_TRUE(g_sb_calls > 0);

    char *blob = NULL;
    size_t n = vt_snapshot(v, &blob);
    ASSERT_TRUE(n > 0);

    vt *w = vt_new(4, 10, &cb, NULL);
    g_resp_len = 0;
    g_sb_calls = 0;
    vt_feed(w, (const uint8_t *)blob, n);
    ASSERT_EQ_INT((long long)g_resp_len, 0);
    ASSERT_EQ_INT(g_sb_calls, 0);

    /* Feeding the same blob twice must also be inert: a failed handoff that
     * retried would otherwise duplicate history. */
    vt_feed(w, (const uint8_t *)blob, n);
    ASSERT_EQ_INT((long long)g_resp_len, 0);
    ASSERT_EQ_INT(g_sb_calls, 0);

    free(blob);
    vt_free(v);
    vt_free(w);
}

TEST(resize_preserves_content) {
    vt *v = vt_new(ROWS, COLS, NULL, NULL);
    vt_feed(v, (const uint8_t *)"keepme", 6);
    vt_resize(v, 4, 20);
    char row[128];
    row_text(v, 0, row, sizeof row);
    ASSERT_EQ_MEM(row, "keepme", 6);
    vt_resize(v, 2, 3);
    row_text(v, 0, row, sizeof row);
    ASSERT_TRUE(strcmp(row, "kee") == 0);
    vt_free(v);
}

TEST(snapshot_contains_content_and_modes) {
    vt *v = vt_new(ROWS, COLS, NULL, NULL);
    vt_feed(v, (const uint8_t *)"snap\x1b[?2004h\x1b[?25l", 4 + 8 + 6);
    char *blob = NULL;
    size_t n = vt_snapshot(v, &blob);
    ASSERT_TRUE(n > 0 && blob != NULL);
    ASSERT_TRUE(memmem(blob, n, "snap", 4) != NULL);
    ASSERT_TRUE(memmem(blob, n, "\x1b[?2004h", 8) != NULL); /* paste re-armed */
    ASSERT_TRUE(memmem(blob, n, "\x1b[?25l", 6) != NULL);   /* cursor hidden */
    free(blob);
    vt_free(v);
}

/* Compare two engines cell-by-cell and mode-by-mode after a snapshot
 * round-trip. Shared by the scene cases below.
 *
 * Row text alone is far too weak an oracle: it renders codepoints and marks but
 * not colours, attributes, tabstops, charset selection or the saved-cursor
 * slot, so a snapshot that dropped every one of those still compared equal.
 * This walks the public cell fields instead, and probes the state that has no
 * getter by feeding a follow-up sequence into both engines and comparing the
 * observable effect. */
static void expect_round_trip(const char *scene, size_t scene_len, const char *probe) {
    vt *v = vt_new(ROWS, COLS, NULL, NULL);
    vt_feed(v, (const uint8_t *)scene, scene_len);
    char *blob = NULL;
    size_t n = vt_snapshot(v, &blob);
    ASSERT_TRUE(n > 0);

    vt *w = vt_new(ROWS, COLS, NULL, NULL);
    vt_feed(w, (const uint8_t *)blob, n);

    /* A follow-up sequence exercises state no getter exposes: a TAB lands on a
     * restored tabstop, a printed character carries the restored pen and
     * charset, ESC 8 loads the restored DECSC slot. Fed to both engines, any
     * divergence shows up in the cell comparison below. */
    if (probe) {
        vt_feed(v, (const uint8_t *)probe, strlen(probe));
        vt_feed(w, (const uint8_t *)probe, strlen(probe));
    }

    for (uint16_t r = 0; r < ROWS; r++) {
        for (uint16_t c = 0; c < COLS; c++) {
            const vt_cell *a = &vt_line(v, r)[c];
            const vt_cell *b = &vt_line(w, r)[c];
            /* An untouched cell holds cp 0; the snapshot paints it as a space.
             * Visually identical, so normalize rather than demand equality. */
            uint32_t acp = a->cp ? a->cp : ' ', bcp = b->cp ? b->cp : ' ';
            t_checks++;
            if (acp != bcp || a->fg != b->fg || a->bg != b->bg ||
                a->attrs != b->attrs || a->comb != b->comb) {
                t_failures++;
                fprintf(stderr,
                        "FAIL round-trip cell %u,%u: cp %u/%u fg %u/%u bg %u/%u "
                        "attrs %u/%u comb %u/%u\n",
                        r, c, acp, bcp, a->fg, b->fg, a->bg, b->bg, a->attrs,
                        b->attrs, a->comb, b->comb);
            }
        }
    }

    uint16_t r1, c1, r2, c2;
    bool vis1, vis2;
    vt_get_cursor(v, &r1, &c1, &vis1);
    vt_get_cursor(w, &r2, &c2, &vis2);
    ASSERT_EQ_INT(r1, r2);
    ASSERT_EQ_INT(c1, c2);
    ASSERT_EQ_INT(vis1, vis2);
    /* Every tracked mode bit, not just one: re-arming DECAWM but losing
     * bracketed paste or mouse reporting is silent breakage on reattach. */
    ASSERT_EQ_INT((long long)vt_get_modes(v), (long long)vt_get_modes(w));

    free(blob);
    vt_free(v);
    vt_free(w);
}

TEST(snapshot_feeds_back_identically) {
    /* Feed a busy screen, snapshot it, feed the snapshot into a fresh vt:
     * grids must match. This is the reattach correctness property. */
    const char *scene =
        "\x1b[2J\x1b[1;1Hone\x1b[2;3H\x1b[1;32mtwo\x1b[0m\x1b[4;1H\xe4\xb8\xad文"
        /* A combining mark on a narrow base and one on a wide base: the
         * snapshot must re-emit both after their base, or the restored grid
         * loses the mark while row_text still compares equal on the base. */
        "\x1b[5;1He\xcc\x81" "x\xe4\xb8\xad\xcc\x82"
        "\x1b[2;4r\x1b[?2004h\x1b[3;2H";
    expect_round_trip(scene, strlen(scene), NULL);
}

TEST(snapshot_restores_pen) {
    /* The pen the app left set must apply to the NEXT character it prints.
     * Without it the character arrives in default colours — and every existing
     * grid assertion still passes, because the cells already on screen carry
     * their own colours. */
    const char *scene = "\x1b[1;4;38;5;196;48;2;10;20;30mred";
    expect_round_trip(scene, strlen(scene), "Z");
}

TEST(snapshot_restores_tabstops) {
    /* TBC(3) clears all, then HTS plants stops at columns 3 and 6 (1-based).
     * The probe TABs twice from home, so a restored engine with default
     * every-8 stops lands in a different column. */
    const char *scene = "\x1b[3g\x1b[1;3H\x1bH\x1b[1;6H\x1bH\x1b[1;1H";
    expect_round_trip(scene, strlen(scene), "\tA\tB");
}

TEST(snapshot_restores_charset) {
    /* G1 designated as DEC graphics and selected with SO. The probe prints
     * 'q', which must render as a horizontal line, not the letter q. */
    const char *scene = "\x1b)0\x0e";
    expect_round_trip(scene, strlen(scene), "qqq");
}

TEST(snapshot_restores_saved_cursor) {
    /* DECSC at 3,5 with a distinctive pen, then the cursor moves away. The
     * probe DECRCs: a restored engine with an empty save slot jumps to 1,1 and
     * prints in default colours instead. */
    const char *scene = "\x1b[3;5H\x1b[33m\x1b\x37\x1b[0m\x1b[6;1H";
    expect_round_trip(scene, strlen(scene), "\x1b\x38QQ");
}

TEST(snapshot_restores_pending_wrap) {
    /* Cursor parked on the last column with a deferred wrap. The probe prints
     * one character, which must wrap to the next row. A restored engine
     * without pending_wrap overwrites the last column instead — one character
     * in the wrong place, and the row below stays blank. */
    const char *scene = "0123456789";
    expect_round_trip(scene, strlen(scene), "W");
}

TEST(snapshot_mid_sequence_completes) {
    /* Snapshot taken with a CSI half-parsed. The remaining bytes arrive from
     * the child after the restart, so the restored parser must be mid-CSI for
     * the sequence to complete. Without the pending-byte replay the receiver is
     * in ground: it would print "2;3Hmid" as literal text across row 1. */
    const char *scene = "abc\x1b[";
    expect_round_trip(scene, strlen(scene), "2;3Hmid");
}

TEST(snapshot_mid_utf8_completes) {
    /* Same property one layer down: two of the three bytes of U+4E2D have
     * arrived. The restored UTF-8 DFA must be mid-character, or the trailing
     * byte decodes as U+FFFD and the wide char never appears. */
    const char *scene = "ab\xe4\xb8";
    expect_round_trip(scene, strlen(scene), "\xad");
}

TEST(snapshot_pending_bytes_bounded) {
    /* A sequence longer than the pending buffer must not be replayed at all:
     * a truncated prefix can decode as a different sequence. Feed an absurd
     * parameter run, snapshot, and assert the blob does not end mid-CSI by
     * checking the restored engine prints the follow-up as ordinary text. */
    vt *v = vt_new(ROWS, COLS, NULL, NULL);
    vt_feed(v, (const uint8_t *)"\x1b[", 2);
    for (int i = 0; i < 6000; i++) vt_feed(v, (const uint8_t *)"1;", 2);
    char *blob = NULL;
    size_t n = vt_snapshot(v, &blob);
    ASSERT_TRUE(n > 0);
    /* The overflowed tail is dropped, so the blob carries no unterminated CSI
     * introducer of its own making. */
    ASSERT_TRUE(memmem(blob, n, "1;1;1;", 6) == NULL);
    vt *w = vt_new(ROWS, COLS, NULL, NULL);
    vt_feed(w, (const uint8_t *)blob, n);
    vt_feed(w, (const uint8_t *)"ok", 2);
    char row[COLS * 8 + 1];
    row_text(w, 0, row, sizeof row);
    for (char *p = row; *p; p++) if (*p == ' ') *p = '.';
    ASSERT_TRUE(strncmp(row, "ok", 2) == 0);
    free(blob);
    vt_free(v);
    vt_free(w);
}

TEST(snapshot_replays_partial_osc) {
    /* An unterminated OSC must be replayed, so the restored parser is still
     * inside the string and swallows the rest of the body. The probe must NOT
     * terminate the OSC: a terminator converges both engines back to ground and
     * the assertion holds either way. With the replay dropped, the restored
     * engine sits in ground and prints the tail as visible text — the measured
     * failure is `hiMORE` on row 0 where the original shows `hi`. */
    const char *scene = "hi\x1b]0;never ends";
    expect_round_trip(scene, strlen(scene), "MORE");
}

TEST(snapshot_omits_c0_from_replay) {
    /* A C0 control arriving mid-sequence dispatches immediately and leaves the
     * parser state untouched, so it is not part of the state being restored —
     * but its effect is already baked into the snapshotted grid. Replaying it
     * would re-run that effect: the LF inside this half-typed CSI moves the
     * cursor down a second time (measured: restored cursor row 3 instead of 2).
     *
     * The probe must complete the CSI *relatively* — `m` is SGR, which consumes
     * the parameters without touching the cursor — and then print. An absolute
     * CUP would overwrite the drifted cursor and hide the divergence entirely,
     * which is how the first version of this test passed against the mutation. */
    const char *scene = "top\r\nsecond\x1b[1;\n";
    expect_round_trip(scene, strlen(scene), "31mZ");
}

TEST(hostile_input_no_crash) {
    /* Pathological parameters and truncated sequences must be safe. */
    vt *v = vt_new(ROWS, COLS, NULL, NULL);
    const char *hostile[] = {
        "\x1b[9999999;9999999H", "\x1b[99999999999999999m", "\x1b[;;;;;;;;;;;;;;;;;;;m",
        "\x1b[?99999999h", "\x1b[999999L", "\x1b[999999@", "\x1b[999999P",
        "\x1b]0;", "\x1b]", "\x1b[", "\x1b", "\x1bP1;2;3|payload never ends",
        "\x1b[38;5m", "\x1b[38;2;1m", "\x1b[38m",
    };
    for (size_t i = 0; i < sizeof hostile / sizeof *hostile; i++)
        vt_feed(v, (const uint8_t *)hostile[i], strlen(hostile[i]));
    /* 4KB of random-ish binary */
    uint8_t junk[4096];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 131 + 7);
    vt_feed(v, junk, sizeof junk);
    ASSERT_TRUE(1); /* reaching here without ASan report is the assertion */
    vt_free(v);
}

int main(void) {
    RUN(table_cases);
    RUN(chunking_property);
    RUN(alt_screen_roundtrip);
    RUN(query_responses);
    RUN(scrollback_emitted);
    RUN(snapshot_replay_is_inert);
    RUN(resize_preserves_content);
    RUN(snapshot_contains_content_and_modes);
    RUN(snapshot_feeds_back_identically);
    RUN(snapshot_restores_pen);
    RUN(snapshot_restores_tabstops);
    RUN(snapshot_restores_charset);
    RUN(snapshot_restores_saved_cursor);
    RUN(snapshot_restores_pending_wrap);
    RUN(snapshot_mid_sequence_completes);
    RUN(snapshot_mid_utf8_completes);
    RUN(snapshot_pending_bytes_bounded);
    RUN(snapshot_replays_partial_osc);
    RUN(snapshot_omits_c0_from_replay);
    RUN(hostile_input_no_crash);
    TEST_MAIN_END();
}
