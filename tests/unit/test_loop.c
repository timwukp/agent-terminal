/* test_loop.c — event-loop slot accounting: capacity, duplicates, reuse.
 *
 * Exists because loop_add_fd's -1 was ignored at three call sites while the
 * cap (256) was smaller than what panes need (418): the first symptom of
 * running out would have been a session whose child's output nobody reads.
 * These tests pin the contract the daemon now relies on; none of them call
 * loop_run, so the fds here are just numbers — loop_add_fd stores them
 * without validating, which keeps this test free of rlimit games. */
#include "runner.h"

#include "daemon/loop.h"

/* Mirrors LOOP_MAX_FDS in loop.c, which is deliberately private (nothing
 * outside the loop should size to it). If the cap changes this constant must
 * change with it — that is the point: the pane math (64 sessions × 6 panes +
 * 32 clients + listener + signal pipe = 418) must stay under it, and a silent
 * shrink below that is exactly what this test exists to catch. */
#define EXPECT_CAP 1024

/* Fake fd numbers far above anything the test process has open, so an
 * accidental future loop_run in this binary could not touch a real fd. */
#define FD_BASE 100000

static void cb_nop(int fd, short revents, void *ud) {
    (void)fd; (void)revents; (void)ud;
}

TEST(fills_to_capacity_then_rejects) {
    int added = 0;
    while (added < EXPECT_CAP + 8) {
        if (loop_add_fd(FD_BASE + added, POLLIN, cb_nop, NULL) != 0) break;
        added++;
    }
    ASSERT_EQ_INT(added, EXPECT_CAP);
    /* Past the cap every add fails, and fails cleanly (no partial insert):
     * deleting the fd that was just rejected must be a no-op — if the failed
     * add left a phantom entry, this del would free it and the re-add below
     * would succeed against a full table. */
    ASSERT_EQ_INT(loop_add_fd(FD_BASE + EXPECT_CAP, POLLIN, cb_nop, NULL), -1);
    loop_del_fd(FD_BASE + EXPECT_CAP);
    ASSERT_EQ_INT(loop_add_fd(FD_BASE + EXPECT_CAP, POLLIN, cb_nop, NULL), -1);
}

TEST(rejects_duplicate_fd) {
    /* The table is full of FD_BASE.. from the previous test; a duplicate of a
     * present fd must fail even when — especially when — slots are scarce. */
    ASSERT_EQ_INT(loop_add_fd(FD_BASE, POLLIN, cb_nop, NULL), -1);
}

TEST(del_frees_exactly_one_slot) {
    loop_del_fd(FD_BASE + 7);
    /* One deletion, one slot: the duplicate check still holds for others... */
    ASSERT_EQ_INT(loop_add_fd(FD_BASE + 1, POLLIN, cb_nop, NULL), -1);
    /* ...the freed fd can return... */
    ASSERT_EQ_INT(loop_add_fd(FD_BASE + 7, POLLIN, cb_nop, NULL), 0);
    /* ...and the table is full again. */
    ASSERT_EQ_INT(loop_add_fd(FD_BASE + EXPECT_CAP + 1, POLLIN, cb_nop, NULL), -1);
}

TEST(del_of_absent_fd_is_noop) {
    loop_del_fd(FD_BASE - 1); /* never added */
    ASSERT_EQ_INT(loop_add_fd(FD_BASE + EXPECT_CAP + 2, POLLIN, cb_nop, NULL), -1);
}

int main(void) {
    RUN(fills_to_capacity_then_rejects);
    RUN(rejects_duplicate_fd);
    RUN(del_frees_exactly_one_slot);
    RUN(del_of_absent_fd_is_noop);
    TEST_MAIN_END();
}
