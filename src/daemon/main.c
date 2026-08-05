/* agent-terminald — session daemon. Owns PTYs and (from M2) screen state;
 * survives any client or hosting-terminal crash. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/path.h"
#include "common/xutil.h"
#include "loop.h"
#include "server.h"
#include "session.h"

static int g_sigpipe[2] = {-1, -1};

static void on_signal(int sig) {
    uint8_t b = (uint8_t)sig;
    ssize_t r = write(g_sigpipe[1], &b, 1);
    (void)r;
}

static void sigpipe_readable(int fd, short revents, void *ud) {
    (void)revents; (void)ud;
    uint8_t sigs[64];
    ssize_t n = read(fd, sigs, sizeof sigs);
    for (ssize_t i = 0; i < n; i++) {
        if (sigs[i] == SIGCHLD) session_reap_children();
        if (sigs[i] == SIGTERM || sigs[i] == SIGINT) loop_quit();
    }
}

static void setup_signals(void) {
    if (pipe(g_sigpipe) != 0) die("pipe: %s", strerror(errno));
    for (int i = 0; i < 2; i++) {
        fcntl(g_sigpipe[i], F_SETFL, O_NONBLOCK);
        fcntl(g_sigpipe[i], F_SETFD, FD_CLOEXEC);
    }
    struct sigaction sa = {.sa_handler = on_signal, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    loop_add_fd(g_sigpipe[0], POLLIN, sigpipe_readable, NULL);
}

int main(int argc, char **argv) {
    bool foreground = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0)
            foreground = true;
        else if (strcmp(argv[i], "-v") == 0)
            log_set_level(LOG_DEBUG);
        else
            die("usage: agent-terminald [-f] [-v]");
    }

    char sock[512];
    if (at_socket_path(sock, sizeof sock) != 0)
        die("cannot resolve runtime dir: %s", strerror(errno));

    setup_signals();

    if (server_init(sock) != 0) {
        if (errno == EADDRINUSE) die("daemon already running on %s", sock);
        die("cannot listen on %s: %s", sock, strerror(errno));
    }

    if (!foreground) {
        /* Double-fork daemonize; stdio to /dev/null (launchd/systemd users
         * should pass -f and let the service manager own the process). */
        pid_t pid = fork();
        if (pid < 0) die("fork: %s", strerror(errno));
        if (pid > 0) _exit(0);
        setsid();
        pid = fork();
        if (pid < 0) _exit(1);
        if (pid > 0) _exit(0);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
    }

    log_msg(LOG_INFO, "agent-terminald listening on %s", sock);
    loop_set_tick(1000, session_flush_all); /* 1s durability window */
    loop_run();
    /* SIGTERM/SIGINT: children die with us, so preserve their screens first. */
    session_flush_screens_all();
    server_shutdown();
    return 0;
}
