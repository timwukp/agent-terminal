/* test_path.c — session-name validation and path resolution.
 *
 * Exists because a session name is interpolated into a filesystem path and
 * then mkdir'd. Unvalidated, `-s ../escape` created a directory outside
 * ~/.agent-terminal/sessions/ and wrote a scrollback log there, and the
 * distinct names "." and "./" shared one log, so `history -s .` printed
 * another session's output. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "runner.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/path.h"
#include "common/scrollback.h"

static char g_home[256];

static void setup_home(void) {
    snprintf(g_home, sizeof g_home, "/tmp/at_path_test_%d", (int)getpid());
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", g_home, g_home);
    if (system(cmd) != 0) exit(1);
    setenv("HOME", g_home, 1);
    unsetenv("XDG_RUNTIME_DIR");
}

TEST(ordinary_names_accepted) {
    ASSERT_TRUE(at_valid_session_name("main"));
    ASSERT_TRUE(at_valid_session_name("work"));
    ASSERT_TRUE(at_valid_session_name("agent-1"));
    ASSERT_TRUE(at_valid_session_name("build_2026.08"));   /* dot, not leading */
    ASSERT_TRUE(at_valid_session_name("a"));
    ASSERT_TRUE(at_valid_session_name("x..y"));            /* dots, not a component */
    ASSERT_TRUE(at_valid_session_name("~root"));           /* no shell here to expand it */
    ASSERT_TRUE(at_valid_session_name(" leading space"));
    ASSERT_TRUE(at_valid_session_name("日本語"));           /* UTF-8 bytes are all >= 0x80 */
}

TEST(traversal_rejected) {
    ASSERT_TRUE(!at_valid_session_name(".."));
    ASSERT_TRUE(!at_valid_session_name("../escape"));
    ASSERT_TRUE(!at_valid_session_name("../../../../../../tmp/pwned"));
    ASSERT_TRUE(!at_valid_session_name("a/../b"));
    ASSERT_TRUE(!at_valid_session_name("a/b"));       /* nested, not traversal, still not one component */
    ASSERT_TRUE(!at_valid_session_name("/tmp/abs")); /* leading slash: snprintf would not even join */
    ASSERT_TRUE(!at_valid_session_name("trailing/"));
}

TEST(aliasing_and_hidden_rejected) {
    /* "." and "./" both resolved to the sessions dir itself, so two sessions
     * shared one scrollback.log and each could read the other's output. */
    ASSERT_TRUE(!at_valid_session_name("."));
    ASSERT_TRUE(!at_valid_session_name("./"));
    /* sb_list_logs() skips dotted dirents, so a hidden session would exist on
     * disk yet never be listed. */
    ASSERT_TRUE(!at_valid_session_name(".hidden"));
}

TEST(empty_and_control_rejected) {
    ASSERT_TRUE(!at_valid_session_name(NULL));
    ASSERT_TRUE(!at_valid_session_name(""));
    ASSERT_TRUE(!at_valid_session_name("nl\nname"));   /* would split sb_list_logs output */
    ASSERT_TRUE(!at_valid_session_name("nul\x01here"));
}

/* Names that lie about themselves on screen. Written as \x escapes on purpose:
 * a test file carrying raw RIGHT-TO-LEFT OVERRIDE bytes would render its own
 * source reversed in an editor, which is the attack it is about. Each string is
 * split before the following letter because "\xE2\x80\xAAb" would parse the 'b'
 * as part of the hex escape — that split is required, not style. */
TEST(misrepresenting_names_rejected) {
    /* UAX #9 explicit formatting — the closed set that REORDERS neighbours. A
     * browser renders "Kill proj<U+202E>gol.hs? Its child process ends." as
     * "Kill proj.sdne ssecorp dlihc stI ?sh.log": the kill prompt says
     * something other than what it means. */
    ASSERT_TRUE(!at_valid_session_name("proj\xE2\x80\xAE" "gol.hs")); /* U+202E RLO */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x80\xAA" "b"));         /* U+202A LRE */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x80\xAB" "b"));         /* U+202B RLE */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x80\xAC" "b"));         /* U+202C PDF */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x80\xAD" "b"));         /* U+202D LRO */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x80\x8E" "b"));         /* U+200E LRM */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x80\x8F" "b"));         /* U+200F RLM */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x81\xA6" "b"));         /* U+2066 LRI */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x81\xA7" "b"));         /* U+2067 RLI */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x81\xA8" "b"));         /* U+2068 FSI */
    ASSERT_TRUE(!at_valid_session_name("a\xE2\x81\xA9" "b"));         /* U+2069 PDI */
    ASSERT_TRUE(!at_valid_session_name("a\xD8\x9C" "b"));             /* U+061C ALM */

    /* Invisible — "deploy" and "deploy<U+200B>" measure the same width to the
     * pixel in the sidebar, and the row is what you click to type into. */
    ASSERT_TRUE(!at_valid_session_name("deploy\xE2\x80\x8B"));    /* U+200B ZWSP */
    ASSERT_TRUE(!at_valid_session_name("dep\xE2\x80\x8C" "loy")); /* U+200C ZWNJ */
    ASSERT_TRUE(!at_valid_session_name("dep\xE2\x80\x8D" "loy")); /* U+200D ZWJ */
    ASSERT_TRUE(!at_valid_session_name("dep\xEF\xBB\xBF" "loy")); /* U+FEFF BOM */
    ASSERT_TRUE(!at_valid_session_name("dep\xE2\x81\xA0" "loy")); /* U+2060 WJ */
    ASSERT_TRUE(!at_valid_session_name("dep\xC2\xAD" "loy"));     /* U+00AD SOFT HYPHEN */
    ASSERT_TRUE(!at_valid_session_name("dep\xCD\x8F" "loy"));     /* U+034F CGJ */
    ASSERT_TRUE(!at_valid_session_name("dep\xE1\xA0\x8E" "loy")); /* U+180E MONGOLIAN VS */
    ASSERT_TRUE(!at_valid_session_name("dep\xEF\xB8\x8F" "loy")); /* U+FE0F VARIATION SEL-16 */
    ASSERT_TRUE(!at_valid_session_name("dep\xEF\xBF\xB9" "loy")); /* U+FFF9 interlinear */
    ASSERT_TRUE(!at_valid_session_name("dep\xF3\xA0\x80\xA1" "loy")); /* U+E0021 tag char */
    ASSERT_TRUE(!at_valid_session_name("two\xE2\x80\xA8" "lines"));   /* U+2028 LINE SEP */

    /* C1 controls and DEL: invisible, and some terminals still act on them. */
    ASSERT_TRUE(!at_valid_session_name("a\xC2\x9B" "b")); /* U+009B CSI */
    ASSERT_TRUE(!at_valid_session_name("a\x7F" "b"));     /* DEL */
}

/* Malformed UTF-8 is not merely untidy. The GUI decodes names with
 * String::from_utf8_lossy, so every bad byte becomes U+FFFD: two different
 * names display as one string, and the bytes the GUI sends back on kill or
 * attach are no longer the name the daemon stored — such a session cannot be
 * killed from the sidebar at all. */
TEST(malformed_utf8_rejected) {
    ASSERT_TRUE(!at_valid_session_name("a\xC3"));                 /* truncated 2-byte */
    ASSERT_TRUE(!at_valid_session_name("a\xE6\x97"));             /* truncated 3-byte */
    ASSERT_TRUE(!at_valid_session_name("a\xF0\x9F\x98"));         /* truncated 4-byte */
    ASSERT_TRUE(!at_valid_session_name("a\x80" "b"));             /* lone continuation */
    ASSERT_TRUE(!at_valid_session_name("a\xC3\xC3" "b"));         /* lead, not continuation */
    ASSERT_TRUE(!at_valid_session_name("a\xF8\x88\x80\x80\x80")); /* 5-byte lead */
    ASSERT_TRUE(!at_valid_session_name("a\xED\xA0\x80"));         /* U+D800 surrogate */
    ASSERT_TRUE(!at_valid_session_name("a\xF5\x80\x80\x80"));     /* beyond U+10FFFF */
    /* Overlong forms are the interesting ones: this is '/' spelled in two
     * bytes, which the plain `*p == '/'` byte test above cannot see. Without
     * the shortest-form check it would reach the filesystem as a separator. */
    ASSERT_TRUE(!at_valid_session_name("a\xC0\xAF" "b"));     /* overlong '/' */
    ASSERT_TRUE(!at_valid_session_name("a\xE0\x80\xAF" "b")); /* overlong '/', 3 bytes */
    ASSERT_TRUE(!at_valid_session_name("a\xC0\x80" "b"));     /* overlong NUL */

    /* A sequence truncated by the terminator must not be read past. Heap so
     * that the ASan build has a redzone immediately after the last byte — a
     * decoder that trusts its length instead of the NUL dies here. */
    char *edge = strdup("ok\xF0\x9F\x98");
    ASSERT_TRUE(edge != NULL);
    ASSERT_TRUE(!at_valid_session_name(edge));
    free(edge);
}

/* The other side of the same rule: rejecting format characters must not cost
 * anyone a real name in their own script. */
TEST(legitimate_unicode_still_accepted) {
    ASSERT_TRUE(at_valid_session_name("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"));  /* 日本語 */
    ASSERT_TRUE(at_valid_session_name("\xE5\xB0\x88\xE6\xA1\x88-A"));            /* 專案-A */
    ASSERT_TRUE(at_valid_session_name("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"));      /* Hebrew */
    ASSERT_TRUE(at_valid_session_name("\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A"));      /* Arabic */
    ASSERT_TRUE(at_valid_session_name("\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82"));
    ASSERT_TRUE(at_valid_session_name("build-\xF0\x9F\x9A\x80")); /* U+1F680, 4-byte emoji */
    ASSERT_TRUE(at_valid_session_name("score-\xF0\x9D\x84\x9E")); /* U+1D11E, 4-byte non-emoji */
    ASSERT_TRUE(at_valid_session_name("caf\xC3\xA9"));            /* U+00E9, 2-byte */
    ASSERT_TRUE(at_valid_session_name("e\xCC\x81tage"));          /* combining acute, VISIBLE */
}

/* The gate that actually matters: even if a caller forgets to validate,
 * session_dir() must refuse rather than mkdir outside the tree. Both entry
 * points from a name to the filesystem are covered — the daemon's sb_open and
 * the client's sb_read_log. */
TEST(session_dir_refuses_traversal) {
    static const char *bad[] = {"..", "../escape", "a/b", ".", "/tmp/abs"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        errno = 0;
        ASSERT_TRUE(sb_open(bad[i], 100, 0) == NULL);
        ASSERT_EQ_INT(errno, EINVAL);
        errno = 0;
        ASSERT_EQ_INT((long long)sb_read_log(bad[i], NULL, NULL), -1);
        ASSERT_EQ_INT(errno, EINVAL);
    }
    /* Nothing was created above the sessions dir. `escape` is the name that
     * landed in ~/.agent-terminal/ before the fix. */
    char probe[512];
    struct stat st;
    snprintf(probe, sizeof probe, "%s/.agent-terminal/escape", g_home);
    ASSERT_TRUE(stat(probe, &st) != 0);
    snprintf(probe, sizeof probe, "%s/.agent-terminal/sessions/scrollback.log", g_home);
    ASSERT_TRUE(stat(probe, &st) != 0);
}

TEST(valid_name_still_opens) {
    /* Guards against a validator so strict it breaks normal use. */
    scrollback *sb = sb_open("ok-name", 100, 0);
    ASSERT_TRUE(sb != NULL);
    sb_close(sb);
    char probe[512];
    struct stat st;
    snprintf(probe, sizeof probe, "%s/.agent-terminal/sessions/ok-name/scrollback.log", g_home);
    ASSERT_TRUE(stat(probe, &st) == 0);
}

int main(void) {
    setup_home();
    RUN(ordinary_names_accepted);
    RUN(traversal_rejected);
    RUN(aliasing_and_hidden_rejected);
    RUN(empty_and_control_rejected);
    RUN(misrepresenting_names_rejected);
    RUN(malformed_utf8_rejected);
    RUN(legitimate_unicode_still_accepted);
    RUN(session_dir_refuses_traversal);
    RUN(valid_name_still_opens);
    TEST_MAIN_END();
}
