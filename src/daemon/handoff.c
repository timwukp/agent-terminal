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
 * Session record (version 2 — grew panes; a v1 file is refused, which only
 * matters for the exec-window of a binary swap mid-reload):
 *   u8 name_len, name bytes, u16 view_cols, u16 view_rows,
 *   u8 active_id, u8 last_id, u8 next_id, u8 npanes,
 *   layout: i8 root, LAYOUT_NODES x {u8 flags(bit0 in_use, bit1 leaf,
 *     bit2 stacked), i8 pane_idx, i8 child0, i8 child1, u16 x, u16 y,
 *     u16 cols, u16 rows},
 *   then npanes x pane record:
 *     u8 slot, u8 id, i32 master_fd, i32 pid, u16 cols, u16 rows,
 *     u16 x, u16 y, u32 blob_len, blob bytes
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
#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath: macOS has no /proc/self/exe */
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "common/path.h"
#include "common/proto.h"
#include "common/xutil.h"
#include "lockfile.h"
#include "loop.h"
#include "server.h"
#include "session.h"

#define HANDOFF_MAGIC   "ATHANDF"      /* 7 chars + implicit NUL = 8 bytes */
#define HANDOFF_VERSION 2
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
#ifdef __APPLE__
    /* No /proc on macOS. _NSGetExecutablePath is the equivalent, and it is
     * not optional here: the autospawned daemon's argv[0] is the bare string
     * "agent-terminald", which realpath resolves against the CWD — so every
     * reload failed with ENOENT and macOS users of the autospawn path simply
     * could not upgrade in place. Found by an end-user UAT reload drill. */
    char raw[PATH_MAX];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) == 0 && realpath(raw, g_exe_path))
        return;
#else
    ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof g_exe_path - 1);
    if (n > 0) {
        g_exe_path[n] = '\0';
        return;
    }
#endif
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
    const pane *panes[MAX_PANES_PER_SESSION]; /* live panes, slot order */
    char *blobs[MAX_PANES_PER_SESSION];
    size_t blob_lens[MAX_PANES_PER_SESSION];
    uint8_t slots[MAX_PANES_PER_SESSION];     /* their array slots */
    uint8_t npanes;
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
        uint8_t b = (uint8_t)nlen;
        if (!wr(f, &b, 1) || !wr(f, s->name, nlen)) return false;

        uint8_t shdr[8];
        put_u16(shdr, s->view_cols);
        put_u16(shdr + 2, s->view_rows);
        shdr[4] = s->active_id;
        shdr[5] = s->last_id;
        shdr[6] = s->next_id;
        shdr[7] = recs[i].npanes;
        if (!wr(f, shdr, sizeof shdr)) return false;

        uint8_t lrec[1 + LAYOUT_NODES * 12];
        lrec[0] = (uint8_t)s->lt.root;
        for (int j = 0; j < LAYOUT_NODES; j++) {
            const layout_node *ln = &s->lt.nodes[j];
            uint8_t *q = lrec + 1 + (size_t)j * 12;
            q[0] = (uint8_t)((ln->in_use ? 1 : 0) | (ln->leaf ? 2 : 0) |
                             (ln->stacked ? 4 : 0));
            q[1] = (uint8_t)ln->pane_idx;
            q[2] = (uint8_t)ln->child[0];
            q[3] = (uint8_t)ln->child[1];
            put_u16(q + 4, ln->x);
            put_u16(q + 6, ln->y);
            put_u16(q + 8, ln->cols);
            put_u16(q + 10, ln->rows);
        }
        if (!wr(f, lrec, sizeof lrec)) return false;

        for (uint8_t j = 0; j < recs[i].npanes; j++) {
            const pane *p = recs[i].panes[j];
            uint8_t prec[2 + 4 + 4 + 2 + 2 + 2 + 2 + 4];
            prec[0] = recs[i].slots[j];
            prec[1] = p->id;
            put_u32(prec + 2, (uint32_t)p->child.master_fd);
            put_u32(prec + 6, (uint32_t)p->child.pid);
            put_u16(prec + 10, p->cols);
            put_u16(prec + 12, p->rows);
            put_u16(prec + 14, p->x);
            put_u16(prec + 16, p->y);
            put_u32(prec + 18, (uint32_t)recs[i].blob_lens[j]);
            if (!wr(f, prec, sizeof prec)) return false;
            if (recs[i].blob_lens[j] &&
                !wr(f, recs[i].blobs[j], recs[i].blob_lens[j]))
                return false;
        }
    }
    return true;
}

int handoff_exec(void) {
    export_rec recs[MAX_SESSIONS];
    uint16_t n = 0;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *s = session_at(i);
        if (!s) continue;
        recs[n].s = s;
        recs[n].npanes = 0;
        for (int j = 0; j < MAX_PANES_PER_SESSION; j++) {
            pane *p = &s->panes[j];
            if (!p->in_use) continue;
            /* A pane whose child is gone has nothing to keep alive, and one
             * with no master fd cannot be handed over. Neither state is
             * reachable today (reaping frees the pane), so this is a guard,
             * not a case: dropping it silently would lose the pane. */
            if (p->child.pid <= 0 || p->child.master_fd < 0) {
                log_msg(LOG_WARN, "handoff: session '%s' pane %u has no live child, "
                                  "not carried over", s->name, p->id);
                continue;
            }
            uint8_t k = recs[n].npanes;
            recs[n].panes[k] = p;
            recs[n].slots[k] = (uint8_t)j;
            recs[n].blobs[k] = NULL;
            recs[n].blob_lens[k] = p->vt ? vt_snapshot(p->vt, &recs[n].blobs[k]) : 0;
            if (recs[n].blob_lens[k] > HANDOFF_BLOB_MAX) {
                /* Keep the child, lose the screen. The alternative — abort
                 * the reload — leaves a daemon that cannot restart. */
                log_msg(LOG_WARN, "handoff: session '%s' pane %u snapshot %zu bytes "
                                  "exceeds cap, screen not carried over",
                        s->name, p->id, recs[n].blob_lens[k]);
                free(recs[n].blobs[k]);
                recs[n].blobs[k] = NULL;
                recs[n].blob_lens[k] = 0;
            }
            recs[n].npanes++;
        }
        if (recs[n].npanes == 0) {
            log_msg(LOG_WARN, "handoff: session '%s' has no live child, not carried over",
                    s->name);
            continue;
        }
        n++;
    }

    /* Durability before the point of no return: the ring's un-flushed tail is
     * process memory, and the new image rebuilds the ring by re-reading the
     * on-disk log (sb_open_pane's refill). Without this the last second of
     * every session's history would be dropped by a *successful* reload.
     * This comment described the intended contract before the refill existed
     * — the new image re-read the log only to resume the sequence NUMBER, so
     * the whole ring came back empty and MSG_SCROLLBACK_REQ could serve
     * nothing. The flush and the refill are two halves of one guarantee;
     * neither is optional. */
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
        for (uint8_t j = 0; j < recs[i].npanes; j++)
            if (set_cloexec(recs[i].panes[j]->child.master_fd, false) != 0)
                log_msg(LOG_WARN, "handoff: session '%s': cannot clear FD_CLOEXEC on fd %d (%s)",
                        recs[i].s->name, recs[i].panes[j]->child.master_fd,
                        strerror(errno));

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
    for (uint16_t i = 0; i < n; i++)
        for (uint8_t j = 0; j < recs[i].npanes; j++)
            set_cloexec(recs[i].panes[j]->child.master_fd, true);
    if (cleared_listen) set_cloexec(listen_fd, true);
    if (cleared_lock) set_cloexec(g_lock_fd, true);
    for (uint16_t i = 0; i < n; i++)
        for (uint8_t j = 0; j < recs[i].npanes; j++)
            free(recs[i].blobs[j]);
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

        /* The state file is an input like any other, and this is the one path
         * by which a name reaches session_import_begin without having passed
         * the wire edge: a file written by a build with a looser rule, or an
         * edited one, would reintroduce a name handle_new now refuses. The
         * session is skipped and reading continues — the records after it are
         * still well-formed, and dropping them would turn one bad name into a
         * lost reload.
         *
         * Checked HERE, before the first message that could quote the name,
         * rather than next to the import call: a log line is a display surface
         * too, so "truncated layout for '<name>'" would reorder the line it
         * appears in, which is precisely what the name was built to do. Every
         * message below quotes `disp`, which is the name only when it passed. */
        bool name_ok = at_valid_session_name(name);
        if (!name_ok)
            log_msg(LOG_ERR, "handoff: record %u of %u has an invalid session "
                             "name; skipping it", i, want);
        const char *disp = name_ok ? name : "<invalid name>";

        uint8_t shdr[8];
        if (!rd(f, shdr, sizeof shdr)) {
            log_msg(LOG_WARN, "handoff: truncated record for '%s'", disp);
            break;
        }
        uint16_t view_cols = get_u16(shdr), view_rows = get_u16(shdr + 2);
        uint8_t active_id = shdr[4], last_id = shdr[5], next_id = shdr[6];
        uint8_t npanes = shdr[7];
        if (npanes == 0 || npanes > MAX_PANES_PER_SESSION) {
            log_msg(LOG_ERR, "handoff: '%s' claims %u panes; state file is "
                             "corrupt, stopping here", disp, npanes);
            break;
        }

        uint8_t lrec[1 + LAYOUT_NODES * 12];
        if (!rd(f, lrec, sizeof lrec)) {
            log_msg(LOG_WARN, "handoff: truncated layout for '%s'", disp);
            break;
        }
        layout lt;
        memset(&lt, 0, sizeof lt);
        lt.root = (int8_t)lrec[0];
        if (lt.root < -1 || lt.root >= LAYOUT_NODES) lt.root = 0;
        for (int j = 0; j < LAYOUT_NODES; j++) {
            const uint8_t *q = lrec + 1 + (size_t)j * 12;
            layout_node *ln = &lt.nodes[j];
            ln->in_use = (q[0] & 1) != 0;
            ln->leaf = (q[0] & 2) != 0;
            ln->stacked = (q[0] & 4) != 0;
            ln->pane_idx = (int8_t)q[1];
            ln->child[0] = (int8_t)q[2];
            ln->child[1] = (int8_t)q[3];
            /* Child indices come from our own writer, but the file could be
             * torn: clamp so reflow cannot index out of the node array. This
             * bounds the INDEX only — it does not make the graph a tree, so
             * `child[0] == self` still survives this loop. Termination is
             * reflow_node's depth cap (layout.c), not this clamp. */
            if (ln->child[0] < 0 || ln->child[0] >= LAYOUT_NODES) ln->child[0] = 0;
            if (ln->child[1] < 0 || ln->child[1] >= LAYOUT_NODES) ln->child[1] = 0;
            if (ln->pane_idx >= MAX_PANES_PER_SESSION) ln->pane_idx = 0;
            ln->x = get_u16(q + 4);
            ln->y = get_u16(q + 6);
            ln->cols = get_u16(q + 8);
            ln->rows = get_u16(q + 10);
        }

        session *s = name_ok ? session_import_begin(name, view_cols, view_rows,
                                                    active_id, last_id, next_id, &lt)
                             : NULL;
        if (!s && name_ok)
            log_msg(LOG_ERR, "handoff: cannot restore '%s': %s", name,
                    strerror(errno));

        bool file_ok = true;
        for (uint8_t j = 0; j < npanes; j++) {
            uint8_t prec[22];
            if (!rd(f, prec, sizeof prec)) {
                log_msg(LOG_WARN, "handoff: truncated pane record for '%s'", disp);
                file_ok = false;
                break;
            }
            uint8_t slot = prec[0], id = prec[1];
            int master_fd = (int)get_u32(prec + 2);
            pid_t pid = (pid_t)(int32_t)get_u32(prec + 6);
            uint16_t cols = get_u16(prec + 10), rows = get_u16(prec + 12);
            uint32_t blob_len = get_u32(prec + 18);

            if (blob_len > HANDOFF_BLOB_MAX) {
                log_msg(LOG_ERR, "handoff: '%s' pane %u claims a %u-byte screen; "
                                 "state file is corrupt, stopping here",
                        disp, id, blob_len);
                file_ok = false;
                break;
            }
            uint8_t *blob = blob_len ? xmalloc(blob_len) : NULL;
            if (blob_len && !rd(f, blob, blob_len)) {
                log_msg(LOG_WARN, "handoff: truncated screen for '%s'", disp);
                free(blob);
                file_ok = false;
                break;
            }

            char label[SESSION_NAME_MAX + 16];
            snprintf(label, sizeof label, "%s pane %u", disp, id);
            if (!s || !adopt_master(master_fd, label) || pid <= 0) {
                free(blob);
                continue; /* record was readable, so keep going with the rest */
            }
            if (!session_import_pane(s, slot, id, master_fd, pid, cols, rows,
                                     blob, blob_len))
                log_msg(LOG_ERR, "handoff: cannot restore '%s' pane %u: %s",
                        name, id, strerror(errno));
            free(blob);
        }

        if (s && session_import_finish(s)) restored++;
        else if (s)
            log_msg(LOG_ERR, "handoff: '%s' restored no usable panes", name);
        if (!file_ok) break;
    }

    g_importing = false;
    fclose(f);
    log_msg(LOG_INFO, "handoff: restored %d of %u session%s (generation %u)",
            restored, want, want == 1 ? "" : "s", g_generation);
    return restored;
}
