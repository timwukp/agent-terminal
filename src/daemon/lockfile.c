/* lockfile.c — the daemon's single-instance lock.
 *
 * Before this file, mutual exclusion was a probe-connect() to the socket
 * followed by unlink + bind (server.c). That is racy in a way the restart
 * handoff cannot tolerate: two daemons starting at the same moment both find
 * nothing answering, both unlink, both bind, and the loser keeps running
 * invisibly — still the parent of its children and still holding their PTY
 * masters, with no socket anyone can reach it through. A handoff that re-execs
 * "the" daemon has no defined meaning in that state.
 *
 * flock(2) rather than an O_EXCL pidfile, because O_EXCL leaves a stale file
 * after a SIGKILL and every reader then has to guess whether the recorded pid
 * is our daemon or an unrelated process that has since reused the number.
 * flock is held by the kernel against the open file description, so it
 * vanishes the instant the holder dies however it dies, and — the property
 * this PR needs — it is preserved across execve() since the fd is.
 *
 * flock rather than fcntl(F_SETLK): fcntl record locks are per-process and are
 * dropped when the process closes *any* descriptor for the file, so the
 * double-fork daemonize path in main.c would release the lock the moment the
 * intermediate parent _exit()s. flock locks belong to the shared open file
 * description the fork duplicated, so they survive exactly as long as one
 * descriptor for it remains open. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "lockfile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/xutil.h"

static int lock_path(char *out, size_t outsz, const char *runtime_dir) {
    if ((size_t)snprintf(out, outsz, "%s/daemon.lock", runtime_dir) >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* Record our pid, for humans and for `agent-terminal reload` to signal.
 * Advisory only: the lock, not this number, is what makes exclusion correct. */
static void write_pid(int fd) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%ld\n", (long)getpid());
    if (n <= 0) return;
    if (ftruncate(fd, 0) != 0) return;
    ssize_t w = pwrite(fd, buf, (size_t)n, 0);
    (void)w; /* best effort: a short write costs a diagnostic, not the lock */
}

int lock_acquire(const char *runtime_dir) {
    char path[600];
    if (lock_path(path, sizeof path, runtime_dir) != 0) return -1;

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        close(fd);
        /* EWOULDBLOCK is the only expected failure and means a live daemon.
         * Normalize it so callers do not have to know which of EWOULDBLOCK or
         * EAGAIN this platform spells it as. */
        errno = (saved == EWOULDBLOCK || saved == EAGAIN) ? EADDRINUSE : saved;
        return -1;
    }
    write_pid(fd);
    return fd;
}

int lock_adopt(int fd, const char *runtime_dir) {
    if (fd < 0) { errno = EBADF; return -1; }
    char path[600];
    if (lock_path(path, sizeof path, runtime_dir) != 0) return -1;

    /* Prove this fd is the lock file. An inherited fd number is not evidence
     * of anything: numbers are reused, so fcntl(F_GETFL) succeeding tells us
     * only that *something* occupies the slot. st_dev/st_ino identify the
     * file itself. */
    struct stat a, b;
    if (fstat(fd, &a) != 0) return -1;
    if (stat(path, &b) != 0) return -1;
    if (a.st_dev != b.st_dev || a.st_ino != b.st_ino) { errno = EINVAL; return -1; }

    /* And prove we still hold it. Re-locking through the same open file
     * description is a no-op conversion that always succeeds; if some other
     * process had taken the lock in the gap, this fails instead of letting two
     * daemons believe they are the only one. */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        errno = (saved == EWOULDBLOCK || saved == EAGAIN) ? EADDRINUSE : saved;
        return -1;
    }
    /* execve preserved the fd, so CLOEXEC — which handoff cleared to get it
     * here — must be put back before any PTY child is spawned. */
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return -1;
    write_pid(fd); /* same pid across execve, but the file may have been reset */
    return 0;
}

void lock_note_pid(int fd) {
    if (fd >= 0) write_pid(fd);
}

long lock_read_pid(const char *runtime_dir) {
    char path[600];
    if (lock_path(path, sizeof path, runtime_dir) != 0) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *end = NULL;
    long pid = strtol(buf, &end, 10);
    return (end && end != buf && pid > 0) ? pid : 0;
}

void lock_release(int fd, const char *runtime_dir) {
    if (fd < 0) return;
    char path[600];
    /* Unlink before releasing: a waiter that opens the path after our unlink
     * gets a fresh inode and locks that instead, which is correct because we
     * are on our way out. Releasing first would let a waiter lock the inode we
     * then unlink from under it, leaving it holding a lock on nothing. */
    if (lock_path(path, sizeof path, runtime_dir) == 0) unlink(path);
    flock(fd, LOCK_UN);
    close(fd);
}
