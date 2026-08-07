/* handoff.c — export state, re-exec in place, import state.
 *
 * State file format (little-endian; written and read only by this file):
 *
 *   offset size field
 *   0      8    magic "ATHANDF\0"
 *   8      2    version (HANDOFF_VERSION)
 *   10     2    session count
 *   12     4    listen fd, or 0xFFFFFFFF for none
 *   16     4    lock fd,   or 0xFFFFFFFF for none
 *   20     4    generation (incremented by the importing image)
 *   24     ...  session records
 *
 * Session record:
 *   u8 name_len, name bytes, i32 master_fd, i32 pid, u16 cols, u16 rows,
 *   u32 blob_len, blob bytes
 *
 * There is deliberately no checksum. The file lives 0600 inside a 0700
 * runtime dir this daemon owns, exists for the microseconds between write and
 * execv, and is unlinked on read; a CRC would protect against nothing the
 * length validation below does not already catch, and would invite treating a
 * checksum pass as proof the fds are real. The fds are validated
 * independently — see adopt_master. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "handoff.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "common/proto.h"
#include "common/xutil.h"
#include "lockfile.h"
#include "loop.h"
#include "server.h"
#include "session.h"

#define HANDOFF_MAGIC   "ATHANDF"      /* 7 chars + implicit NUL = 8 bytes */
#define HANDOFF_VERSION 1
#define HANDOFF_HDR     24

/* A snapshot of the largest grid the engine allows (1000x1000) with truecolor
 * fg+bg changing every cell measures ~43 bytes/cell, so ~41 MiB. Round up for
 * headroom and refuse anything past it: a blob that large means a corrupt
 * length field, not a real screen. Refusing costs one session's screen; a
 * multi-gigabyte allocation costs the daemon. */
#define HANDOFF_BLOB_MAX (64u << 20)

static char g_state_path[600];
static char g_runtime_dir[512];
static char g_exe_path[PATH_MAX];
static int g_lock_fd = -1;
static bool g_verbose;
static bool g_importing;
static bool g_requested;
static uint32_t g_generation;

bool handoff_importing(void) { return g_importing; }
uint32_t handoff_generation(void) { return g_generation; }

void handoff_request(void) {
    g_requested = true;
    loop_quit();
}

bool handoff_take_request(void) {
    bool r = g_requested;
    g_requested = false;
    return r;
}

/* Resolve our own binary to an absolute path *now*, at startup, because by the
 * time a reload arrives argv[0] may be relative to a cwd that has since been
 * removed. /proc/self/exe is preferred where it exists: it survives the binary
 * being renamed or replaced by an upgrade, which is the main reason to reload
 * in the first place. */
static void resolve_exe(const char *argv0) {
    ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof g_exe_path - 1);
    if (n > 0) {
        g_exe_path[n] = '\0';
        return;
    }
    if (argv0 && realpath(argv0, g_exe_path)) return;
    /* Last resort: whatever we were invoked as. execv will fail loudly if it
     * no longer resolves, and handoff_exec reports that rather than exiting. */
    strncpy(g_exe_path, argv0 ? argv0 : "agent-terminald", sizeof g_exe_path - 1);
    g_exe_path[sizeof g_exe_path - 1] = '\0';
}

void handoff_init(const char *runtime_dir, const char *argv0, bool verbose) {
    strncpy(g_runtime_dir, runtime_dir, sizeof g_runtime_dir - 1);
    snprintf(g_state_path, sizeof g_state_path, "%s/handoff.state", runtime_dir);
    g_verbose = verbose;
    resolve_exe(argv0);
}

void handoff_set_lock_fd(int fd) { g_lock_fd = fd; }

const char *handoff_state_path(void) { return g_state_path; }

/* ---- fd inheritance ---- */

/* execve keeps every fd that is not FD_CLOEXEC, and nothing else about a
 * descriptor changes — O_NONBLOCK included, because it lives on the open file
 * description rather than the descriptor. */
static int set_cloexec(int fd, bool on) {
    int fl = fcntl(fd, F_GETFD);
    if (fl < 0) return -1;
    int want = on ? (fl | FD_CLOEXEC) : (fl & ~FD_CLOEXEC);
    return fcntl(fd, F_SETFD, want);
}

/* ---- export ---- */

typedef struct {
    const session *s;
    char *blob;
    size_t blob_len;
} export_rec;

static bool wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }

static bool write_state(FILE *f, const export_rec *recs, uint16_t n, int listen_fd) {
    uint8_t hdr[HANDOFF_HDR];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, HANDOFF_MAGIC, 8); /* strlen 7 + NUL, memset covers the NUL */
    put_u16(hdr + 8, HANDOFF_VERSION);
    put_u16(hdr + 10, n);
    put_u32(hdr + 12, (uint32_t)listen_fd);
    put_u32(hdr + 16, (uint32_t)g_lock_fd);
    put_u32(hdr + 20, g_generation + 1);
    if (!wr(f, hdr, sizeof hdr)) return false;

    for (uint16_t i = 0; i < n; i++) {
        const session *s = recs[i].s;
        size_t nlen = strlen(s->name);
        uint8_t rec[1 + 4 + 4 + 2 + 2 + 4];
        uint8_t b = (uint8_t)nlen;
        if (!wr(f, &b, 1) || !wr(f, s->name, nlen)) return false;
        put_u32(rec, (uint32_t)s->child.master_fd);
        put_u32(rec + 4, (uint32_t)s->child.pid);
        put_u16(rec + 8, s->cols);
        put_u16(rec + 10, s->rows);
        put_u32(rec + 12, (uint32_t)recs[i].blob_len);
        if (!wr(f, rec, 16)) return false;
        if (recs[i].blob_len && !wr(f, recs[i].blob, recs[i].blob_len)) return false;
    }
    return true;
}

int handoff_exec(void) {
    export_rec recs[MAX_SESSIONS];
    uint16_t n = 0;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *s = session_at(i);
        if (!s) continue;
        /* A session whose child is gone has nothing to keep alive, and a
         * session with no master fd cannot be handed over. Neither state is
         * reachable today (reaping frees the slot), so this is a guard, not a
         * case: dropping such a session would silently lose it. */
        if (s->child.pid <= 0 || s->child.master_fd < 0) {
            log_msg(LOG_WARN, "handoff: session '%s' has no live child, not carried over",
                    s->name);
            continue;
        }
        recs[n].s = s;
        recs[n].blob = NULL;
        recs[n].blob_len = s->vt ? vt_snapshot(s->vt, &recs[n].blob) : 0;
        if (recs[n].blob_len > HANDOFF_BLOB_MAX) {
            /* Keep the child, lose the screen. The alternative — abort the
             * reload — leaves the operator with a daemon they cannot restart. */
            log_msg(LOG_WARN, "handoff: session '%s' snapshot %zu bytes exceeds cap, "
                              "screen not carried over", s->name, recs[n].blob_len);
            free(recs[n].blob);
            recs[n].blob = NULL;
            recs[n].blob_len = 0;
        }
        n++;
    }

    /* Durability before the point of no return: the ring's un-flushed tail is
     * process memory, and the new image rebuilds scrollback by re-reading the
     * on-disk log. Without this the last second of every session's history
     * would be dropped by a *successful* reload. */
    session_flush_all();

    /* Clients are not serialized. They already retry ATTACH on a 250 ms→4 s
     * backoff and re-snapshot when it succeeds (attach.c), so closing them is
     * strictly less state to get wrong and exercises a path that is already
     * tested. Their sockets are FD_CLOEXEC and would close at execv anyway;
     * doing it here makes the client see a clean EOF rather than a half-written
     * frame. */
    int listen_fd = server_prepare_handoff();

    /* Point of no return begins here: everything below either execs or undoes
     * itself. */
    bool cleared_listen = listen_fd >= 0 && set_cloexec(listen_fd, false) == 0;
    bool cleared_lock = g_lock_fd >= 0 && set_cloexec(g_lock_fd, false) == 0;
    for (uint16_t i = 0; i < n; i++)
        if (set_cloexec(recs[i].s->child.master_fd, false) != 0)
            log_msg(LOG_WARN, "handoff: session '%s': cannot clear FD_CLOEXEC on fd %d (%s)",
                    recs[i].s->name, recs[i].s->child.master_fd, strerror(errno));

    int rc = -1;
    /* O_EXCL: a leftover state file means a previous reload died between write
     * and exec. Reusing it would replay stale fd numbers into a live process. */
    unlink(g_state_path);
    int sfd = open(g_state_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    FILE *f = sfd >= 0 ? fdopen(sfd, "w") : NULL;
    if (!f) {
        if (sfd >= 0) close(sfd);
        log_msg(LOG_ERR, "handoff: cannot write %s: %s", g_state_path, strerror(errno));
        goto done;
    }
    if (!write_state(f, recs, n, cleared_listen ? listen_fd : -1) || fflush(f) != 0) {
        log_msg(LOG_ERR, "handoff: short write to %s: %s", g_state_path, strerror(errno));
        fclose(f);
        unlink(g_state_path);
        goto done;
    }
    fclose(f);

    log_msg(LOG_INFO, "handoff: re-exec %s with %u session%s", g_exe_path, n,
            n == 1 ? "" : "s");

    {
        char *argv[5];
        int argc = 0;
        argv[argc++] = g_exe_path;
        argv[argc++] = (char *)"--handoff";
        argv[argc++] = g_state_path;
        if (g_verbose) argv[argc++] = (char *)"-v";
        argv[argc] = NULL;
        /* -f is intentionally absent: the new image must not daemonize. It is
         * already the daemon — double-forking here would abandon every child
         * to a new parent and drop the lock's last descriptor. handoff mode
         * implies foreground for exactly that reason (see main.c). */
        execv(g_exe_path, argv);
    }

    log_msg(LOG_ERR, "handoff: execv %s failed: %s", g_exe_path, strerror(errno));
    unlink(g_state_path);

done:
    /* Failed before exec: put every descriptor back the way it was so the
     * still-running daemon keeps its close-on-exec guarantees for the next
     * child it spawns. */
    for (uint16_t i = 0; i < n; i++) set_cloexec(recs[i].s->child.master_fd, true);
    if (cleared_listen) set_cloexec(listen_fd, true);
    if (cleared_lock) set_cloexec(g_lock_fd, true);
    for (uint16_t i = 0; i < n; i++) free(recs[i].blob);
    if (rc != 0 && errno == 0) errno = EIO;
    return rc;
}

/* ---- import ---- */

static bool rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

/* An fd number carried across execv is not evidence by itself: numbers are
 * reused, so a corrupt or stale state file could name fd 1 and we would then
 * treat the daemon's own stdout as a PTY master. TIOCGWINSZ succeeding proves
 * the descriptor is a terminal, which for this daemon means a PTY master it
 * opened. Standard descriptors are refused outright — the daemon's stdio is
 * never a session's PTY. */
static bool adopt_master(int fd, const char *name) {
    if (fd <= 2) {
        log_msg(LOG_WARN, "handoff: session '%s': refusing fd %d as a PTY master", name, fd);
        return false;
    }
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0) {
        log_msg(LOG_WARN, "handoff: session '%s': fd %d is not a terminal (%s), "
                          "child was lost", name, fd, strerror(errno));
        return false;
    }
    if (set_cloexec(fd, true) != 0)
        log_msg(LOG_WARN, "handoff: session '%s': cannot re-set FD_CLOEXEC on fd %d",
                name, fd);
    return true;
}

int handoff_import(const char *state_path, int *listen_fd, int *lock_fd) {
    *listen_fd = -1;
    *lock_fd = -1;

    FILE *f = fopen(state_path, "r");
    if (!f) {
        log_msg(LOG_ERR, "handoff: cannot read %s: %s", state_path, strerror(errno));
        return -1;
    }
    /* Unlink immediately, while the descriptor keeps the contents reachable. A
     * file that survives a crash mid-import would be replayed into a *second*
     * process, handing the same fd numbers to someone who does not own them. */
    unlink(state_path);

    uint8_t hdr[HANDOFF_HDR];
    if (!rd(f, hdr, sizeof hdr) || memcmp(hdr, HANDOFF_MAGIC, 8) != 0) {
        log_msg(LOG_ERR, "handoff: %s is not a state file", state_path);
        fclose(f);
        return -1;
    }
    uint16_t ver = get_u16(hdr + 8);
    if (ver != HANDOFF_VERSION) {
        log_msg(LOG_ERR, "handoff: state file version %u, this daemon speaks %d",
                ver, HANDOFF_VERSION);
        fclose(f);
        return -1;
    }
    uint16_t want = get_u16(hdr + 10);
    int lfd = (int)get_u32(hdr + 12);
    int kfd = (int)get_u32(hdr + 16);
    /* Adopted verbatim, not incremented: the *writer* already stored its own
     * generation plus one (write_state). Doing it again here made every reload
     * advance the counter by two, which no assertion noticed because the client
     * only ever compares for an increase. One reload must be one generation, or
     * the number cannot be reasoned about. */
    g_generation = get_u32(hdr + 20);

    if (want > MAX_SESSIONS) {
        log_msg(LOG_ERR, "handoff: state file claims %u sessions, cap is %d",
                want, MAX_SESSIONS);
        fclose(f);
        return -1;
    }

    if (kfd >= 0 && lock_adopt(kfd, g_runtime_dir) == 0) {
        *lock_fd = kfd;
    } else if (kfd >= 0) {
        log_msg(LOG_WARN, "handoff: inherited lock fd %d unusable (%s)", kfd,
                strerror(errno));
    }
    if (lfd >= 0) *listen_fd = lfd;

    /* Suppress the two side effects a replayed screen would otherwise cause:
     * scrollback lines pushed a second time, and DSR/CPR answers written into
     * the child's stdin. The blob addresses rows absolutely and never scrolls,
     * so neither is expected — which is exactly why the guard belongs here and
     * not in a comment. */
    g_importing = true;

    int restored = 0;
    for (uint16_t i = 0; i < want; i++) {
        uint8_t nlen;
        char name[SESSION_NAME_MAX + 1];
        if (!rd(f, &nlen, 1) || nlen == 0 || nlen > SESSION_NAME_MAX) {
            log_msg(LOG_WARN, "handoff: truncated at record %u of %u", i, want);
            break;
        }
        if (!rd(f, name, nlen)) {
            log_msg(LOG_WARN, "handoff: truncated name at record %u", i);
            break;
        }
        name[nlen] = '\0';

        uint8_t rec[16];
        if (!rd(f, rec, sizeof rec)) {
            log_msg(LOG_WARN, "handoff: truncated record for '%s'", name);
            break;
        }
        int master_fd = (int)get_u32(rec);
        pid_t pid = (pid_t)(int32_t)get_u32(rec + 4);
        uint16_t cols = get_u16(rec + 8), rows = get_u16(rec + 10);
        uint32_t blob_len = get_u32(rec + 12);

        if (blob_len > HANDOFF_BLOB_MAX) {
            log_msg(LOG_ERR, "handoff: '%s' claims a %u-byte screen; state file "
                             "is corrupt, stopping here", name, blob_len);
            break;
        }
        uint8_t *blob = blob_len ? xmalloc(blob_len) : NULL;
        if (blob_len && !rd(f, blob, blob_len)) {
            log_msg(LOG_WARN, "handoff: truncated screen for '%s'", name);
            free(blob);
            break;
        }

        if (!adopt_master(master_fd, name) || pid <= 0) {
            free(blob);
            continue; /* record was readable, so keep going with the rest */
        }
        session *s = session_import(name, master_fd, pid, cols, rows, blob, blob_len);
        free(blob);
        if (!s) {
            log_msg(LOG_ERR, "handoff: cannot restore '%s': %s", name, strerror(errno));
            continue;
        }
        restored++;
    }

    g_importing = false;
    fclose(f);
    log_msg(LOG_INFO, "handoff: restored %d of %u session%s (generation %u)",
            restored, want, want == 1 ? "" : "s", g_generation);
    return restored;
}
