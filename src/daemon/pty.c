/* pty.c — ALL macOS/Linux PTY divergence is contained in this file.
 *
 * Explicit posix_openpt path rather than forkpty(3), so both platforms run
 * the exact same sequence and there is no libutil dependency. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "pty.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int pty_resize(int master_fd, uint16_t cols, uint16_t rows) {
    struct winsize ws = {.ws_col = cols, .ws_row = rows};
    return ioctl(master_fd, TIOCSWINSZ, &ws);
}

int pty_spawn(pty_child *out, char *const argv[], uint16_t cols, uint16_t rows,
              const char *term_env) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) goto fail;

    /* Single-threaded daemon: ptsname's static buffer is safe; copy anyway. */
    const char *sname = ptsname(master);
    if (!sname) goto fail;
    char slave_path[128];
    strncpy(slave_path, sname, sizeof slave_path - 1);
    slave_path[sizeof slave_path - 1] = '\0';

    pid_t pid = fork();
    if (pid < 0) goto fail;

    if (pid == 0) {
        /* child */
        if (setsid() < 0) _exit(127);
        int slave = open(slave_path, O_RDWR);
        if (slave < 0) _exit(127);
        /* setsid() ran first, so TIOCSCTTY succeeds on macOS too (EPERM
         * only occurs with an existing controlling tty). */
        if (ioctl(slave, TIOCSCTTY, 0) < 0) _exit(127);

        struct winsize ws = {.ws_col = cols, .ws_row = rows};
        ioctl(slave, TIOCSWINSZ, &ws); /* before exec: app never sees 0x0 */

        if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0) _exit(127);
        if (slave > 2) close(slave);
        close(master);

        /* Undo daemon signal dispositions for the app. */
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);

        if (term_env) setenv("TERM", term_env, 1);
        /* The child inherits the daemon's environment, so TERM_PROGRAM /
         * TERM_PROGRAM_VERSION describe whatever terminal the daemon was
         * launched from (or nothing under launchd) — either way not the
         * terminal the app is actually talking to. It is talking to us. */
        setenv("TERM_PROGRAM", "agent-terminal", 1);
        unsetenv("TERM_PROGRAM_VERSION");
        unsetenv("TERM_SESSION_ID");
        execvp(argv[0], argv);
        /* The exec failed and this child IS the session: whatever it writes
         * to the slave is the session's screen, and the final-screen flush
         * preserves it into scrollback. Without this the failure was
         * completely silent — a session created for a command that is not on
         * the daemon's PATH (a launchd daemon gets only /usr/bin:/bin:...)
         * just vanished: the client saw a blank snapshot and
         * "[session exited: 127]", `ls` was empty, `history` was empty.
         * Plain write()s to fd 2 (the slave, via the dup2 above): no stdio
         * buffering to flush in a forked child, nothing async-signal-unsafe. */
        {
            const char *msg[] = {"agent-terminald: exec ", argv[0], ": ",
                                 strerror(errno), "\r\n(daemon PATH: ",
                                 getenv("PATH") ? getenv("PATH") : "(unset)",
                                 ")\r\n"};
            for (size_t mi = 0; mi < sizeof msg / sizeof *msg; mi++) {
                ssize_t w = write(2, msg[mi], strlen(msg[mi]));
                (void)w;
            }
        }
        _exit(127);
    }

    /* parent */
    int fl = fcntl(master, F_GETFL);
    if (fl >= 0) fcntl(master, F_SETFL, fl | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    out->master_fd = master;
    out->pid = pid;
    return 0;

fail:;
    int saved = errno;
    close(master);
    errno = saved;
    return -1;
}
