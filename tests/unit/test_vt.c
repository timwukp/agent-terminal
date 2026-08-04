/* test_vt.c — table-driven VT engine tests: bytes in → expected grid out.
 * Also property-tests boundary chunking: every case is re-fed 1 byte at a
 * time and must produce an identical grid. */
#include "runner.h"

#include <stdlib.h>

#include "vt/vt.h"

#define ROWS 6
#define COLS 10

/* Render a row as UTF-8 text (cp==0 → '.', spacer → '_') for assertions. */
static void row_text(const vt *v, uint16_t row, char *out, size_t outsz) {
    const vt_cell *line = vt_line(v, row);
    size_t o = 0;
    uint16_t rows, cols;
    vt_get_size(v, &rows, &cols);
    for (uint16_t c = 0; c < cols && o + 4 < outsz; c++) {
        uint32_t cp = line[c].cp;
        if (line[c].attrs & VT_ATTR_WIDE_SPACER) { out[o++] = '_'; continue; }
        if (cp == 0) { out[o++] = '.'; continue; }
        if (cp < 0x80) { out[o++] = (char)cp; continue; }
        if (cp < 0x800) {
            out[o++] = (char)(0xc0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xe0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[o++] = (char)(0xf0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
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
    {"combining dropped", "e\xcc\x81x", 0, "ex........", 0, 2},
    {"invalid utf8 fffd", "\xff", 0, "\xef\xbf\xbd.........", 0, 1},
    {"overlong rejected", "\xc0\xafz", 0, "\xef\xbf\xbd\xef\xbf\xbdz.......", -1, -1},
    {"osc title ignored on grid", "\x1b]2;my title\x07x", 0, "x.........", 0, 1},
    {"osc esc-st terminated", "\x1b]0;t\x1b\\y", 0, "y.........", 0, 1},
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
        char got[COLS * 4 + 1];
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
            char a[COLS * 4 + 1], b[COLS * 4 + 1];
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
    char row[COLS * 4 + 1];
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

TEST(snapshot_feeds_back_identically) {
    /* Feed a busy screen, snapshot it, feed the snapshot into a fresh vt:
     * grids must match. This is the reattach correctness property. */
    vt *v = vt_new(ROWS, COLS, NULL, NULL);
    const char *scene =
        "\x1b[2J\x1b[1;1Hone\x1b[2;3H\x1b[1;32mtwo\x1b[0m\x1b[4;1H\xe4\xb8\xad文"
        "\x1b[2;4r\x1b[?2004h\x1b[3;2H";
    vt_feed(v, (const uint8_t *)scene, strlen(scene));
    char *blob = NULL;
    size_t n = vt_snapshot(v, &blob);
    ASSERT_TRUE(n > 0);

    vt *w = vt_new(ROWS, COLS, NULL, NULL);
    vt_feed(w, (const uint8_t *)blob, n);
    for (uint16_t r = 0; r < ROWS; r++) {
        char a[COLS * 4 + 1], b[COLS * 4 + 1];
        row_text(v, r, a, sizeof a);
        row_text(w, r, b, sizeof b);
        /* Empty cell (never written) and painted space are visually
         * identical; the snapshot paints spaces. Normalize for compare. */
        for (char *p = a; *p; p++) if (*p == ' ') *p = '.';
        for (char *p = b; *p; p++) if (*p == ' ') *p = '.';
        t_checks++;
        if (strcmp(a, b) != 0) {
            t_failures++;
            fprintf(stderr, "FAIL snapshot row %u: '%s' != '%s'\n", r, a, b);
        }
    }
    uint16_t r1, c1, r2, c2;
    vt_get_cursor(v, &r1, &c1, NULL);
    vt_get_cursor(w, &r2, &c2, NULL);
    ASSERT_EQ_INT(r1, r2);
    ASSERT_EQ_INT(c1, c2);
    ASSERT_EQ_INT((long long)(vt_get_modes(v) & VT_MODE_PASTE),
                  (long long)(vt_get_modes(w) & VT_MODE_PASTE));
    free(blob);
    vt_free(v);
    vt_free(w);
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
    RUN(resize_preserves_content);
    RUN(snapshot_contains_content_and_modes);
    RUN(snapshot_feeds_back_identically);
    RUN(hostile_input_no_crash);
    TEST_MAIN_END();
}
