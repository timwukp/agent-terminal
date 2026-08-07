/* lockfile.h — single-instance mutual exclusion for the daemon.
 *
 * Held for the daemon's whole lifetime, and inherited across the in-place
 * re-exec that handoff.c performs, so a graceful restart never has a window
 * where a second daemon can take over the socket. */
#ifndef AT_LOCKFILE_H
#define AT_LOCKFILE_H

#include <stdbool.h>
#include <stddef.h>

/* Acquire <runtime_dir>/daemon.lock exclusively and record our pid in it.
 * Returns the held fd, or -1: errno == EADDRINUSE means another daemon holds
 * the lock, anything else is a real error. The fd is FD_CLOEXEC. */
int lock_acquire(const char *runtime_dir);

/* Adopt a lock fd inherited across exec, verifying it really is the lock file
 * before trusting it, and refresh the recorded pid. Returns 0, or -1 with
 * errno set (EBADF/EINVAL) if the fd is not our lock file. */
int lock_adopt(int fd, const char *runtime_dir);

/* Rewrite the recorded pid. Needed after the double-fork daemonize, where the
 * pid that acquired the lock is not the pid that keeps it. */
void lock_note_pid(int fd);

/* Read the pid recorded in <runtime_dir>/daemon.lock without disturbing the
 * lock. Returns 0 if there is no readable pid. Advisory: the file may name a
 * daemon that has since died. */
long lock_read_pid(const char *runtime_dir);

/* Release + unlink. Only the process that holds the lock may call this. */
void lock_release(int fd, const char *runtime_dir);

#endif
