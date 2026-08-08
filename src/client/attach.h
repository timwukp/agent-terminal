#ifndef AT_ATTACH_H
#define AT_ATTACH_H

#include <stdbool.h>
#include <stdint.h>

/* Connect to the daemon socket. auto_start: fork/exec agent-terminald if
 * nothing is listening. Returns connected fd (HELLO already negotiated)
 * or -1. */
int daemon_connect(int auto_start);

/* Restart generation reported by the most recent successful HELLO. Zero on a
 * daemon that predates the field, which is indistinguishable from a daemon that
 * has never reloaded — and harmlessly so, since only a *change* is meaningful. */
uint32_t daemon_generation(void);

/* Daemon pid from the most recent successful HELLO. Stable across an in-place
 * restart, which is the point: it is the generation that moves. */
uint32_t daemon_pid(void);
/* True when the connected daemon's HELLO_OK advertised pane support. */
bool daemon_has_panes(void);

/* Attach to (or create, if argv != NULL) a session and pump until detach,
 * session exit, or unrecoverable connection loss. Returns exit code. */
int attach_run(const char *name, char *const argv[], int argc);

#endif
