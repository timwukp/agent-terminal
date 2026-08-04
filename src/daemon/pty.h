#ifndef AT_PTY_H
#define AT_PTY_H

#include <stdint.h>
#include <sys/types.h>

typedef struct {
    int master_fd; /* O_NONBLOCK | FD_CLOEXEC */
    pid_t pid;
} pty_child;

/* Spawn argv on a new PTY. Child gets TERM=term_env, a fresh session,
 * the slave as controlling tty and stdin/out/err, and the initial winsize
 * applied BEFORE exec so the app never sees a 0x0 window.
 * Returns 0 on success, -1 with errno set. */
int pty_spawn(pty_child *out, char *const argv[], uint16_t cols, uint16_t rows,
              const char *term_env);

int pty_resize(int master_fd, uint16_t cols, uint16_t rows);

#endif
