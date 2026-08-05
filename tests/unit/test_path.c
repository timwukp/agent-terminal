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
    RUN(session_dir_refuses_traversal);
    RUN(valid_name_still_opens);
    TEST_MAIN_END();
}
