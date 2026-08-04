/* pty.c — ALL macOS/Linux PTY divergence is contained in this file.
 *
 * Explicit posix_openpt path rather than forkpty(3), so both platforms run
 * the exact same sequence and there is no libutil dependency. */
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
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
        execvp(argv[0], argv);
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
