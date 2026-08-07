/* agent-terminald — session daemon. Owns PTYs and (from M2) screen state;
 * survives any client or hosting-terminal crash, and re-execs itself in place
 * on SIGHUP so an upgrade or restart keeps every child alive (handoff.c). */
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
#include "handoff.h"
#include "lockfile.h"
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
        /* SIGHUP means "restart yourself, keep the children". Deliberately
         * deferred to after loop_run returns rather than acted on here: the
         * handoff closes clients and re-execs, and doing that underneath a poll
         * dispatch would return into state the exec never comes back from. */
        if (sigs[i] == SIGHUP) handoff_request();
    }
}

/* Every disposition below is reset to SIG_DFL by execve — the signal *mask*
 * survives, the handlers do not. So this runs again in the new image, and a
 * daemon that skipped it would take the default action on the next SIGHUP and
 * die, taking every child with it. */
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
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (loop_add_fd(g_sigpipe[0], POLLIN, sigpipe_readable, NULL) != 0)
        die("event loop full at startup");
}

int main(int argc, char **argv) {
    /* Same hazard as the client, and worse here: setup_signals() creates the
     * signal self-pipe, and a daemon started with fd 0 closed would make it
     * fd 0. It also runs before the re-exec handoff, so the new image gets the
     * same guarantee. See fd_sanitize_std(). */
    int fd_fixed = fd_sanitize_std();

    bool foreground = false;
    bool verbose = false;
    const char *state_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
            foreground = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--handoff") == 0 && i + 1 < argc) {
            state_path = argv[++i];
        } else {
            die("usage: agent-terminald [-f] [-v]");
        }
    }
    if (verbose) log_set_level(LOG_DEBUG);
    /* Logged rather than silent: being started with a std fd closed says
     * something is wrong with the supervisor, and the symptom it causes
     * otherwise looks like a daemon bug. */
    if (fd_fixed > 0)
        log_msg(LOG_WARN, "reopened %d closed std fd(s) onto /dev/null at startup",
                fd_fixed);

    char rundir[512];
    if (at_runtime_dir(rundir, sizeof rundir) != 0)
        die("cannot resolve runtime dir: %s", strerror(errno));
    char sock[512];
    if (at_socket_path(sock, sizeof sock) != 0)
        die("cannot resolve runtime dir: %s", strerror(errno));

    setup_signals();
    handoff_init(rundir, argv[0], verbose);

    /* --handoff means we ARE the daemon already: same pid, same children, and
     * the lock and listener arrive as inherited fds. Daemonizing here would
     * double-fork away from the very children this path exists to preserve. */
    int lock_fd = -1, listen_fd = -1;
    int restored = 0;
    if (state_path) {
        foreground = true;
        restored = handoff_import(state_path, &listen_fd, &lock_fd);
        if (restored < 0) {
            log_msg(LOG_WARN, "handoff: state unusable, starting with no sessions");
            restored = 0;
        }
    }

    /* No inherited lock — a cold start, or a handoff whose lock fd did not
     * survive. Take it now; a second daemon must not get past this line. */
    if (lock_fd < 0) {
        lock_fd = lock_acquire(rundir);
        if (lock_fd < 0) {
            if (errno == EADDRINUSE) {
                long other = lock_read_pid(rundir);
                if (other)
                    die("daemon already running (pid %ld); `agent-terminal reload` "
                        "restarts it in place", other);
                die("daemon already running");
            }
            die("cannot lock %s/daemon.lock: %s", rundir, strerror(errno));
        }
    }
    handoff_set_lock_fd(lock_fd);

    if (server_init(sock, listen_fd) != 0) {
        if (errno == EADDRINUSE) die("daemon already running on %s", sock);
        die("cannot listen on %s: %s", sock, strerror(errno));
    }

    if (!foreground) {
        /* Double-fork daemonize; stdio to /dev/null (launchd/systemd users
         * should pass -f and let the service manager own the process).
         *
         * The flock survives this because it belongs to the open file
         * description that fork duplicated, not to the process — which is why
         * lockfile.c uses flock and not fcntl(F_SETLK), whose record locks the
         * intermediate parent's _exit() would drop. The pid written into the
         * file does not survive, so refresh it. */
        pid_t pid = fork();
        if (pid < 0) die("fork: %s", strerror(errno));
        if (pid > 0) _exit(0);
        setsid();
        pid = fork();
        if (pid < 0) _exit(1);
        if (pid > 0) _exit(0);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
        lock_note_pid(lock_fd);
        handoff_init(rundir, argv[0], verbose); /* /proc/self/exe: new pid */
    }

    if (state_path)
        log_msg(LOG_INFO, "agent-terminald reloaded on %s (generation %u, %d session%s)",
                sock, handoff_generation(), restored, restored == 1 ? "" : "s");
    else
        log_msg(LOG_INFO, "agent-terminald listening on %s", sock);

    for (;;) {
        loop_set_tick(1000, session_flush_all); /* 1s durability window */
        loop_run();
        if (!handoff_take_request()) break;
        /* handoff_exec does not return on success. On failure it has already
         * restored every FD_CLOEXEC flag it cleared, so serving on is safe —
         * and much better than exiting, which would kill the children this
         * path exists to preserve. Clients were disconnected either way and
         * reconnect on their own backoff. */
        if (handoff_exec() != 0)
            log_msg(LOG_ERR, "reload failed (%s); continuing without restarting",
                    strerror(errno));
    }

    /* SIGTERM/SIGINT: children die with us, so preserve their screens first. */
    session_flush_screens_all();
    server_shutdown();
    lock_release(lock_fd, rundir);
    return 0;
}
