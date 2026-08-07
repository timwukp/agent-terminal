/* handoff.h — graceful restart: re-exec this daemon in place, keeping every
 * PTY child alive.
 *
 * The mechanism is execve(2), not fd passing. execve keeps the pid, so the
 * daemon stays the parent of every child (waitpid keeps working) and — the
 * load-bearing part — every fd without FD_CLOEXEC stays open across it, so a
 * PTY master is never closed. No close means no carrier loss on the slave side,
 * which means no SIGHUP to the child's process group, which is exactly how
 * today's restart kills children. O_NONBLOCK lives on the open file
 * description, so it survives too.
 *
 * What this deliberately does NOT do is survive a *crash*. If the daemon takes
 * a SIGSEGV or a SIGKILL, no code of ours runs, nothing holds the master fds,
 * and the children get their SIGHUP. Covering that needs a second process that
 * is both the fd holder and the child's parent — a different architecture, not
 * a bigger version of this file. README says so in those words. */
#ifndef AT_HANDOFF_H
#define AT_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

/* True while a state file is being replayed into fresh sessions. Suppresses
 * scrollback pushes and PTY writes that the replay would otherwise trigger
 * a second time. */
bool handoff_importing(void);

/* How many times this process has handed itself over. Logged at startup so a
 * reload is distinguishable from a cold start in a log file, which is the only
 * evidence available when the pid deliberately does not change. */
uint32_t handoff_generation(void);

/* Path of the state file, for tests and diagnostics. */
const char *handoff_state_path(void);

/* Remember where the state file lives and how to name our own binary. Must be
 * called before handoff_import, which needs the runtime dir to verify the
 * inherited lock. */
void handoff_init(const char *runtime_dir, const char *argv0, bool verbose);

/* The lock fd to hand to the next image. Set once the lock is held, which may
 * be either side of handoff_import. */
void handoff_set_lock_fd(int fd);

/* Ask for a restart, from a signal-pipe read or a client's MSG_RELOAD. Stops
 * the event loop; the handoff itself runs after loop_run returns, never
 * underneath a poll dispatch that the exec would never return into. */
void handoff_request(void);

/* Consume a pending request. True at most once per handoff_request. */
bool handoff_take_request(void);

/* Serialize every live session, clear FD_CLOEXEC on the fds the next image
 * must inherit, and execv our own binary. Does not return on success.
 * Returns -1 with errno set on failure, having restored FD_CLOEXEC — the
 * caller can keep running. */
int handoff_exec(void);

/* Replay a state file written by handoff_exec: rebuild each session around its
 * inherited master fd and screen blob, then unlink the file. Returns the number
 * of sessions restored, or -1 if the file was unusable (already logged; the
 * daemon must then start clean rather than exit). Fills *listen_fd with the
 * inherited listener, or -1 if there was none.
 *
 * Deliberately tolerant: a truncated or corrupt file loses sessions, which is
 * bad, but refusing to start is worse — the operator would then have neither
 * their sessions nor a daemon. */
int handoff_import(const char *state_path, int *listen_fd, int *lock_fd);

#endif
