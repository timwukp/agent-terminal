/* agent-terminal — thin client CLI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "attach.h"
#include "common/path.h"
#include "common/proto.h"
#include "common/scrollback.h"
#include "common/xutil.h"

#include "at_version.h" /* generated into $(O)/include by the Makefile */

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  agent-terminal new    [-s name] [-- cmd args...]   create + attach\n"
            "  agent-terminal attach  -s name                     attach to session\n"
            "  agent-terminal ls                                  list sessions\n"
            "  agent-terminal history -s name                     dump scrollback (works for dead sessions)\n"
            "  agent-terminal kill    -s name                     kill session\n"
            "  agent-terminal reload                              restart the daemon, keep sessions\n"
            "  agent-terminal version                             client + daemon versions\n"
            "\ndetach without killing: Ctrl-\\ then Ctrl-d\n");
    exit(2);
}

static int cmd_ls(void) {
    int fd = daemon_connect(0);
    if (fd < 0) { printf("no sessions (daemon not running)\n"); return 0; }

    /* Prefer the length-prefixed v2 list (adds pane count / zoom). A daemon
     * without it silently skips the unknown request — bounded wait, then
     * fall back to v1. Two requests on one connection would be simpler, but
     * v1 daemons answer only the second, and telling "skipped v2" from "slow
     * v1 answer" apart needs the timeout anyway. */
    bool v2 = daemon_has_panes();
    uint8_t hdr[PROTO_HDR_SIZE] = {0, 0, 0, 0,
                                   v2 ? MSG_LIST_SESSIONS2 : MSG_LIST_SESSIONS};
    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) { close(fd); return 1; }

    uint8_t rhdr[PROTO_HDR_SIZE];
    size_t got = 0;
    while (got < sizeof rhdr) {
        ssize_t r = read(fd, rhdr + got, sizeof rhdr - got);
        if (r <= 0) { close(fd); return 1; }
        got += (size_t)r;
    }
    uint32_t plen = get_u32(rhdr);
    if (rhdr[4] != (v2 ? MSG_SESSION_LIST2 : MSG_SESSION_LIST) ||
        plen > PROTO_MAX_PAYLOAD) { close(fd); return 1; }
    uint8_t *p = xmalloc(plen ? plen : 1);
    got = 0;
    while (got < plen) {
        ssize_t r = read(fd, p + got, plen - got);
        if (r <= 0) { free(p); close(fd); return 1; }
        got += (size_t)r;
    }
    close(fd);

    if (plen < 2) { free(p); return 1; }
    uint16_t count = get_u16(p);
    if (count == 0) printf("no sessions\n");
    size_t off = 2;
    for (uint16_t i = 0; i < count && off < plen; i++) {
        size_t entry_end = plen;
        if (v2) {
            if (off + 2 > plen) break;
            uint16_t elen = get_u16(p + off); off += 2;
            if (off + elen > plen) break;
            entry_end = off + elen; /* unknown tail fields skip cleanly */
        }
        if (off + 1 > entry_end) break;
        uint8_t nlen = p[off++];
        if (off + nlen + 14 > entry_end) break;
        char name[256];
        memcpy(name, p + off, nlen);
        name[nlen] = '\0';
        off += nlen;
        uint16_t cols = get_u16(p + off); off += 2;
        uint16_t rows = get_u16(p + off); off += 2;
        uint8_t alive = p[off++];
        uint8_t ncli = p[off++];
        int32_t pid = (int32_t)get_u32(p + off); off += 4;
        int32_t status = (int32_t)get_u32(p + off); off += 4;
        uint8_t npanes = 1, zoomed = 0;
        if (v2 && off + 2 <= entry_end) {
            npanes = p[off];
            zoomed = p[off + 1];
        }
        if (alive) {
            printf("%s: %ux%u, pid %d, %u client%s", name, cols, rows, pid, ncli,
                   ncli == 1 ? "" : "s");
            if (npanes > 1)
                printf(", %u panes%s", npanes, zoomed ? " (zoomed)" : "");
            printf("\n");
        } else
            printf("%s: dead (exit %d)\n", name, status);
        if (v2) off = entry_end;
    }
    free(p);
    return 0;
}

static void history_line(void *ud, uint64_t seq, const char *text, uint32_t len) {
    (void)ud; (void)seq;
    fwrite(text, 1, len, stdout);
    fputc('\n', stdout);
}

/* history reads the on-disk log directly: no daemon required, so it works
 * for dead sessions and even after a daemon crash. The daemon's 1s flush
 * tick bounds how much recent output a live read can miss. */
static int cmd_history(const char *name) {
    int64_t n = sb_read_log(name, history_line, NULL);
    if (n < 0) {
        fprintf(stderr, "agent-terminal: no scrollback found for '%s'\n", name);
        return 1;
    }
    return 0;
}

/* Must wait for the daemon's reply before closing. Closing straight after
 * write() made the daemon see EOF and tear the connection down before it
 * dispatched the buffered frame, so the session always survived while the
 * client still printed "killed" and exited 0 — a race the client always lost.
 * The reply is also the only thing that can tell us whether the session
 * existed, so the exit code has to come from it rather than from write(). */
static int cmd_kill(const char *name) {
    int fd = daemon_connect(0);
    if (fd < 0) return 1;
    size_t nlen = strlen(name); /* main() already caps this at 63 */
    uint8_t frame[PROTO_HDR_SIZE + 1 + 255];
    put_u32(frame, (uint32_t)(1 + nlen));
    frame[4] = MSG_KILL_SESSION;
    frame[5] = (uint8_t)nlen;
    /* Length-prefixed wire field, not a C string (see attach.c). */
    memcpy(frame + 6, name, nlen); /* NOLINT(bugprone-not-null-terminated-result) */
    ssize_t total = (ssize_t)(PROTO_HDR_SIZE + 1 + nlen);
    if (write(fd, frame, (size_t)total) != total) {
        fprintf(stderr, "agent-terminal: failed to kill '%s'\n", name);
        close(fd);
        return 1;
    }

    /* The daemon answers a kill with a fresh SESSION_LIST, or ERR if the
     * session was unknown. */
    uint8_t rhdr[PROTO_HDR_SIZE];
    size_t got = 0;
    while (got < sizeof rhdr) {
        ssize_t r = read(fd, rhdr + got, sizeof rhdr - got);
        if (r <= 0) {
            fprintf(stderr, "agent-terminal: no reply from daemon\n");
            close(fd);
            return 1;
        }
        got += (size_t)r;
    }
    uint8_t type = rhdr[4];
    uint32_t plen = get_u32(rhdr);
    if (plen > PROTO_MAX_PAYLOAD) { close(fd); return 1; }
    uint8_t *p = xmalloc(plen ? plen : 1);
    got = 0;
    while (got < plen) {
        ssize_t r = read(fd, p + got, plen - got);
        if (r <= 0) { free(p); close(fd); return 1; }
        got += (size_t)r;
    }
    close(fd);

    if (type == MSG_ERR) {
        uint16_t mlen;
        const char *emsg;
        if (proto_err_text(p, plen, NULL, &emsg, &mlen))
            fprintf(stderr, "agent-terminal: %.*s\n", (int)mlen, emsg);
        else
            fprintf(stderr, "agent-terminal: cannot kill '%s'\n", name);
        free(p);
        return 1;
    }
    free(p);
    if (type != MSG_SESSION_LIST) {
        fprintf(stderr, "agent-terminal: unexpected reply killing '%s'\n", name);
        return 1;
    }
    printf("killed '%s'\n", name);
    return 0;
}

/* Ask the daemon to re-exec itself in place, then prove it worked.
 *
 * "Prove" is the whole difficulty. The daemon's pid does not change — that is
 * the mechanism, not a bug — so the usual evidence is unavailable, and the reply
 * to MSG_RELOAD necessarily arrives *before* the restart because every socket
 * closes as it begins. So: record the generation counter, send the request, then
 * reconnect until HELLO reports a higher one. A daemon that failed to re-exec
 * keeps serving with the old generation and this reports the failure rather than
 * a cheerful nothing. */
static int cmd_reload(void) {
    int fd = daemon_connect(0);
    if (fd < 0) return 1;
    uint32_t before_gen = daemon_generation();
    uint32_t before_pid = daemon_pid();

    uint8_t frame[PROTO_HDR_SIZE] = {0, 0, 0, 0, MSG_RELOAD};
    if (write(fd, frame, sizeof frame) != (ssize_t)sizeof frame) {
        fprintf(stderr, "agent-terminal: cannot send reload\n");
        close(fd);
        return 1;
    }
    /* Read until the connection drops. The ack may or may not arrive before the
     * daemon closes us; either way EOF is the signal that the handoff started. */
    uint8_t drain[64];
    while (read(fd, drain, sizeof drain) > 0) { /* discard */ }
    close(fd);

    /* Re-exec plus replaying every session's screen is milliseconds, but a
     * loaded machine with many sessions can take longer, so poll for 5 s. */
    for (int i = 0; i < 50; i++) {
        usleep(100 * 1000);
        int nfd = daemon_connect(0);
        if (nfd < 0) continue;
        uint32_t gen = daemon_generation();
        uint32_t pid = daemon_pid();
        close(nfd);
        if (gen > before_gen) {
            if (pid != before_pid)
                /* Not the failure mode this command guards against, but worth
                 * saying: a different pid means something restarted the daemon
                 * from outside, so children were not preserved. */
                printf("daemon reloaded, but pid changed %u → %u: sessions were "
                       "not preserved\n", before_pid, pid);
            else
                printf("daemon reloaded in place (pid %u, generation %u)\n", pid, gen);
            return 0;
        }
    }
    fprintf(stderr, "agent-terminal: daemon did not report a reload "
                    "(still generation %u); check its log\n", before_gen);
    return 1;
}

int main(int argc, char **argv) {
    /* Before anything that can allocate a descriptor: attach_run() creates a
     * SIGWINCH self-pipe, and pipe() takes the lowest free fds, so a missing
     * fd 0 would turn stdin into that pipe. See fd_sanitize_std(). */
    fd_sanitize_std();
    if (argc < 2) usage();
    const char *verb = argv[1];
    const char *name = NULL;
    int cmd_start = -1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            cmd_start = i + 1;
            break;
        } else {
            usage();
        }
    }
    if (name && strlen(name) > 63) die("session name too long (max 63)");
    /* Names index a directory under ~/.agent-terminal/sessions/, so a name
     * with a '/' or a leading '.' escaped that tree or aliased another
     * session: `history -s .` used to print a different session's output.
     * Rejected here as well as in the daemon because `history` never talks to
     * the daemon — it opens the log file itself. */
    if (name && !at_valid_session_name(name))
        die("invalid session name '%s': no '/', no leading '.'", name);

    if (strcmp(verb, "version") == 0 || strcmp(verb, "--version") == 0) {
        /* Client version from the build; daemon identity from a live HELLO.
         * Exists because a version skew is otherwise invisible: a stale
         * daemon answers the socket and silently lacks newer messages. The
         * daemon line reuses what HELLO_OK already carries — pid, restart
         * generation, capability bits — so this needs no protocol change
         * and works against any daemon back to v1. */
        printf("agent-terminal %s\n", AT_VERSION);
        int fd = daemon_connect(-1);
        if (fd < 0) {
            printf("daemon: not running\n");
            return 0;
        }
        printf("daemon: pid %u, generation %u, panes %s\n",
               daemon_pid(), daemon_generation(),
               daemon_has_panes() ? "yes" : "no");
        close(fd);
        return 0;
    }
    if (strcmp(verb, "ls") == 0) return cmd_ls();
    if (strcmp(verb, "reload") == 0) return cmd_reload();
    if (strcmp(verb, "history") == 0) {
        if (!name) usage();
        return cmd_history(name);
    }
    if (strcmp(verb, "kill") == 0) {
        if (!name) usage();
        return cmd_kill(name);
    }
    if (strcmp(verb, "attach") == 0 || strcmp(verb, "a") == 0) {
        if (!name) usage();
        return attach_run(name, NULL, 0);
    }
    if (strcmp(verb, "new") == 0) {
        if (!name) name = "main";
        char *const *cmd_argv = cmd_start > 0 ? &argv[cmd_start] : NULL;
        int cmd_argc = cmd_start > 0 ? argc - cmd_start : 0;
        static char *empty[] = {NULL};
        return attach_run(name, cmd_argv ? cmd_argv : empty, cmd_argc);
    }
    usage();
}
