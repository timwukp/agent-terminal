#ifndef AT_ATTACH_H
#define AT_ATTACH_H

/* Connect to the daemon socket. auto_start: fork/exec agent-terminald if
 * nothing is listening. Returns connected fd (HELLO already negotiated)
 * or -1. */
int daemon_connect(int auto_start);

/* Attach to (or create, if argv != NULL) a session and pump until detach,
 * session exit, or unrecoverable connection loss. Returns exit code. */
int attach_run(const char *name, char *const argv[], int argc);

#endif
