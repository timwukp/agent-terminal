#include "session.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/proto.h"
#include "common/xutil.h"
#include "handoff.h"
#include "loop.h"
#include "server.h"

static session g_sessions[MAX_SESSIONS];

static void pane_pty_readable(int fd, short revents, void *ud);

/* ---- VT callbacks ---- */

/* Both callbacks below are suppressed while a restart handoff replays a screen.
 *
 * Measured, so the strength of the claim is not overstated: with both guards
 * removed, a full reload invokes neither callback even once, and scrollback line
 * counts are identical before and after. The blob addresses rows absolutely and
 * never scrolls, and vt_snapshot emits no queries, so neither side effect is
 * reachable today — `snapshot_replay_is_inert` in tests/unit/test_vt.c pins that
 * property where it actually lives.
 *
 * These are therefore defense in depth, kept because the failure modes are
 * severe and silent, and because a future change to vt_snapshot (say, emitting a
 * DA1 to re-probe, or painting rows by scrolling) would otherwise turn a screen
 * restore into corrupted history and injected keystrokes. */

static void pane_stdin(pane *p, const uint8_t *data, uint32_t len);

static void vt_cb_response(void *ud, const char *buf, size_t len) {
    /* Query answers (DA/DSR/CPR) go back to the application via the PTY. A
     * duplicate would answer a question the app asked before the restart and
     * already received, arriving as unsolicited keyboard input — a literal
     * `[12;40R` typed into a shell prompt.
     *
     * ud is the pane, NOT the session: a background pane's DSR must be
     * answered into that pane's own PTY. Routing through the session would
     * answer it into whichever pane holds the keyboard, injecting the reply
     * into the focused app as typed input. */
    if (handoff_importing()) return;
    pane *p = ud;
    pane_stdin(p, (const uint8_t *)buf, (uint32_t)len);
}

static void vt_cb_scrollback(void *ud, const vt_cell *cells, uint16_t n) {
    /* Any line a replay pushed off the top would be a line the pre-restart
     * engine already stored, duplicating history that `history` and copy-mode
     * both read. */
    if (handoff_importing()) return;
    pane *p = ud;
    sb_push_line(p->sb, cells, n);
}

static void vt_cb_bell(void *ud) { (void)ud; }

static void vt_cb_title(void *ud, const char *utf8, size_t len) {
    (void)ud; (void)utf8; (void)len; /* passthrough already forwards it */
}

session *session_find(const char *name) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].in_use && strcmp(g_sessions[i].name, name) == 0)
            return &g_sessions[i];
    return NULL;
}

pane *session_active_pane(session *s) {
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use) return &s->panes[i];
    return NULL; /* unreachable for an in-use session; callers may assume */
}

static void pane_flush_screen(pane *p);

void session_flush_all(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *s = &g_sessions[i];
        if (!s->in_use) continue;
        for (int j = 0; j < MAX_PANES_PER_SESSION; j++)
            if (s->panes[j].in_use) sb_flush(s->panes[j].sb);
    }
}

/* Daemon shutdown (SIGTERM/SIGINT). The children die with us, so this is the
 * last chance to preserve what is on their screens; without it a service
 * restart silently discarded every session's visible output, unlike an
 * explicit kill or a child exiting, which both flush via pane_free_slot. */
void session_flush_screens_all(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *s = &g_sessions[i];
        if (!s->in_use) continue;
        for (int j = 0; j < MAX_PANES_PER_SESSION; j++)
            if (s->panes[j].in_use) {
                pane_flush_screen(&s->panes[j]);
                sb_flush(s->panes[j].sb);
            }
    }
}

int session_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].in_use) n++;
    return n;
}

session *session_at(int idx) {
    return (idx >= 0 && idx < MAX_SESSIONS && g_sessions[idx].in_use) ? &g_sessions[idx] : NULL;
}

/* ---- pane lifecycle ---- */

/* Build a pane's engine and scrollback; everything except the child. Shared
 * by the spawn and import paths so a restored pane is indistinguishable from
 * a fresh one afterwards — the alternative, a second copy of this setup in
 * handoff.c, is how a restored session ends up with subtly different
 * callbacks or no scrollback at all. */
static pane *pane_alloc(session *s, uint16_t cols, uint16_t rows) {
    pane *p = NULL;
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (!s->panes[i].in_use) { p = &s->panes[i]; break; }
    if (!p) { errno = ENOSPC; return NULL; }

    memset(p, 0, sizeof *p);
    p->sess = s;
    /* Wire id: pane 0 is the session's first pane forever; the round-robin
     * allocator for 1..254 arrives with the split messages (5c). Today only
     * one pane ever exists, and its id must be 0 so its scrollback file is
     * byte-identical to what every existing log reader expects. */
    p->id = 0;
    p->cols = cols;
    p->rows = rows;
    p->child.master_fd = -1;
    p->child.pid = -1;

    vt_callbacks cb = {
        .on_response = vt_cb_response,
        .on_scrollback_line = vt_cb_scrollback,
        .on_title = vt_cb_title,
        .on_bell = vt_cb_bell,
        .on_passthrough = NULL, /* raw tee already forwards everything live */
    };
    p->vt = vt_new(p->rows, p->cols, &cb, p);
    if (!p->vt) { errno = ENOMEM; return NULL; }
    /* Best-effort: a pane without persistent scrollback still works.
     * sb_open_pane resumes line_seq from what is on disk, which is what makes
     * scrollback numbering continuous across a restart. */
    p->sb = sb_open_pane(s->name, p->id, 0, 0);
    if (!p->sb)
        log_msg(LOG_WARN, "session '%s': scrollback disabled (%s)", s->name,
                strerror(errno));
    return p;
}

static void pane_alloc_undo(pane *p) {
    vt_free(p->vt);
    p->vt = NULL;
    sb_close(p->sb);
    p->sb = NULL;
    p->in_use = false;
}

/* Push what is still on the visible screen into scrollback.
 *
 * scrollback only ever received lines that scrolled off, so anything still
 * on screen when a session ended was lost outright: a child that printed a
 * short fatal message and exited left `history` returning zero bytes, while
 * the identical message survived if 100 filler lines had pushed it off. For
 * this project that inverts the priority — the last screen of a crashed AI
 * agent is the single most valuable thing to recover.
 *
 * Trailing blank lines are trimmed so an idle 24-row screen does not append
 * 20 empty records per session. Panes ending on the alternate screen are
 * skipped: vt_line() exposes the active grid, and scrollback holds
 * primary-screen content only (vt.h), so flushing vim's or htop's live UI
 * here would both break that contract and bury the useful history. The cost
 * is that a pane dying inside vim also loses the primary lines still on
 * screen — vt_line() cannot reach the inactive grid. Lines that had already
 * scrolled off are unaffected either way. */
static void pane_flush_screen(pane *p) {
    if (!p->vt || !p->sb) return;
    if (vt_get_modes(p->vt) & VT_MODE_ALTSCREEN) return;
    uint16_t rows = 0, cols = 0;
    vt_get_size(p->vt, &rows, &cols);
    uint16_t last = 0; /* one past the final non-blank row */
    for (uint16_t r = 0; r < rows; r++) {
        const vt_cell *line = vt_line(p->vt, r);
        if (!line) continue;
        for (uint16_t c = 0; c < cols; c++)
            if (line[c].cp != 0 && line[c].cp != ' ') { last = (uint16_t)(r + 1); break; }
    }
    for (uint16_t r = 0; r < last; r++) {
        const vt_cell *line = vt_line(p->vt, r);
        if (line) sb_push_line(p->sb, line, cols);
    }
}

static void pane_free_slot(pane *p) {
    pane_flush_screen(p); /* before vt_free/sb_close destroy both sides */
    if (p->child.master_fd >= 0) {
        loop_del_fd(p->child.master_fd);
        close(p->child.master_fd);
        p->child.master_fd = -1;
    }
    vt_free(p->vt);
    p->vt = NULL;
    sb_close(p->sb);
    p->sb = NULL;
    p->in_use = false;
}

/* ---- session lifecycle ---- */

static session *session_alloc(const char *name, uint16_t cols, uint16_t rows) {
    if (session_find(name)) { errno = EEXIST; return NULL; }
    session *s = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!g_sessions[i].in_use) { s = &g_sessions[i]; break; }
    if (!s) { errno = ENOSPC; return NULL; }

    memset(s, 0, sizeof *s);
    strncpy(s->name, name, SESSION_NAME_MAX);
    s->view_cols = cols ? cols : 80;
    s->view_rows = rows ? rows : 24;
    return s;
}

static void session_free_slot(session *s) {
    /* Disconnect every attached client here, not only in session_kill: the
     * reap path used to free the slot with s->clients[] and each client's
     * c->attached still set. That is not a use-after-free — the slot lives in
     * static g_sessions — which is exactly what made it dangerous: once the
     * slot is reused, the stale pointer aliases a live *different* session,
     * and a client of the dead one can inject stdin into, resize, and read
     * scrollback from a session it never attached to.
     *
     * The order is load-bearing: null the table entry first, then disconnect,
     * because client_disconnect → session_detach iterates the same table this
     * loop is clearing. sb_flush inside session_detach is safe — the panes'
     * sbs are closed below, after this loop. */
    for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++)
        if (s->clients[i]) {
            struct client *c = s->clients[i];
            s->clients[i] = NULL;
            client_disconnect(c);
        }
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use) pane_free_slot(&s->panes[i]);
    s->in_use = false;
}

session *session_new(const char *name, char *const argv[], uint16_t cols, uint16_t rows) {
    session *s = session_alloc(name, cols, rows);
    if (!s) return NULL;

    pane *p = pane_alloc(s, s->view_cols, s->view_rows);
    if (!p) return NULL;

    if (pty_spawn(&p->child, argv, p->cols, p->rows, "xterm-256color") != 0) {
        int saved = errno;
        pane_alloc_undo(p);
        errno = saved;
        return NULL;
    }
    p->in_use = true;
    s->in_use = true;
    if (loop_add_fd(p->child.master_fd, POLLIN, pane_pty_readable, p) != 0) {
        /* Out of poll slots. The child exists but nothing would ever read its
         * output, so it would fill the PTY buffer and block forever with no
         * visible cause. Kill it and report, rather than hand back a session
         * that silently does not work. */
        int saved = errno;
        log_msg(LOG_ERR, "session '%s': no event-loop slot for fd %d", s->name,
                p->child.master_fd);
        kill(p->child.pid, SIGHUP);
        close(p->child.master_fd);
        p->child.master_fd = -1;
        pane_alloc_undo(p);
        s->in_use = false;
        errno = saved ? saved : ENOSPC;
        return NULL;
    }
    log_msg(LOG_INFO, "session '%s': pid %d on fd %d", s->name, (int)p->child.pid,
            p->child.master_fd);
    return s;
}

session *session_import(const char *name, int master_fd, pid_t pid, uint16_t cols,
                        uint16_t rows, const uint8_t *blob, size_t blob_len) {
    session *s = session_alloc(name, cols, rows);
    if (!s) return NULL;

    pane *p = pane_alloc(s, s->view_cols, s->view_rows);
    if (!p) return NULL;

    p->child.master_fd = master_fd;
    p->child.pid = pid;
    p->in_use = true;
    s->in_use = true;

    /* Replay the pre-restart screen. vt_feed on a snapshot blob is the same
     * round-trip a reattaching client already relies on, so there is no second
     * serialization format here to keep correct. */
    if (blob_len && p->vt) vt_feed(p->vt, blob, blob_len);

    if (loop_add_fd(master_fd, POLLIN, pane_pty_readable, p) != 0) {
        int saved = errno;
        /* Do NOT kill the child or close the master here. The child is alive
         * and predates us; dropping the fd is what sends it SIGHUP. Better to
         * leak the slot and say so than to kill a session the operator was
         * explicitly trying to preserve. */
        log_msg(LOG_ERR, "session '%s': no event-loop slot for inherited fd %d; "
                         "child %d is alive but unattended", s->name, master_fd, (int)pid);
        errno = saved ? saved : ENOSPC;
        return s;
    }
    /* Re-apply geometry: the state file's cols/rows are authoritative, and the
     * PTY already has them, but a client that resized during the gap did not
     * reach us. Harmless when they match — pty_resize is idempotent. */
    pty_resize(master_fd, p->cols, p->rows);
    log_msg(LOG_INFO, "session '%s': restored pid %d on fd %d (%ux%u, %zu-byte screen)",
            s->name, (int)pid, master_fd, p->cols, p->rows, blob_len);
    return s;
}

void session_kill(session *s) {
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use && s->panes[i].child.pid > 0)
            kill(s->panes[i].child.pid, SIGHUP);
    session_free_slot(s); /* also disconnects every attached client */
}

void session_reap_children(void) {
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session *s = &g_sessions[i];
            if (!s->in_use) continue;
            for (int j = 0; j < MAX_PANES_PER_SESSION; j++) {
                pane *p = &s->panes[j];
                if (!p->in_use || p->child.pid != pid) continue;
                p->child.pid = -1;
                p->exit_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
                log_msg(LOG_INFO, "session '%s': child exited %d", s->name,
                        p->exit_status);
                uint8_t payload[4];
                put_u32(payload, (uint32_t)p->exit_status);
                /* Until splits exist a session has exactly one pane, so its
                 * child exiting is the session exiting. With ≥2 panes this
                 * becomes MSG_PANE_EXITED + sibling-absorbs-space (5c); the
                 * session dies only with its last pane. */
                for (int k = 0; k < MAX_CLIENTS_PER_SESSION; k++)
                    if (s->clients[k])
                        client_send(s->clients[k], MSG_SESSION_EXITED, payload, 4);
                pane_free_slot(p);
                bool any = false;
                for (int k = 0; k < MAX_PANES_PER_SESSION; k++)
                    if (s->panes[k].in_use) { any = true; break; }
                /* Freeing here means `ls` never reports a session as dead —
                 * the client's "dead (exit N)" branch is unreachable, and that
                 * is intentional for v1. pane_free_slot flushed the final
                 * screen to scrollback first, so `history` still recovers it. */
                if (!any) session_free_slot(s);
            }
        }
    }
}

static void pane_pty_readable(int fd, short revents, void *ud) {
    pane *p = ud;
    session *s = p->sess;
    (void)revents;
    uint8_t buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            /* VT engine tracks screen state (snapshot source on reattach);
             * attached clients get the same bytes raw (perfect fidelity).
             * This raw tee is the single-pane fast path and must survive 5c
             * unchanged: at one pane it has perfect fidelity for free, and
             * the byte-identical guard test pins it. */
            if (p->vt) vt_feed(p->vt, buf, (size_t)n);
            for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++)
                if (s->clients[i])
                    client_send(s->clients[i], MSG_OUTPUT, buf, (uint32_t)n);
            if ((size_t)n < sizeof buf) return;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        /* EOF or EIO: child side closed. Reap will finish the cleanup;
         * stop watching now to avoid a hot loop. */
        loop_del_fd(fd);
        return;
    }
}

/* Ceiling for the blob carried inside one MSG_SNAPSHOT. A worst-case grid
 * (1000×1000, truecolor fg+bg changing per cell) serializes to ~43 bytes a
 * cell — far past PROTO_MAX_PAYLOAD (1 MiB), where proto_write_frame returns
 * false and client_send answers by disconnecting. With the client's reconnect
 * loop that is not one lost repaint, it is a reconnect→attach→disconnect loop
 * that never converges. So the snapshot frame carries at most this much and
 * the remainder rides MSG_OUTPUT: the client handles both with the same
 * write-to-fd-1 loop, so the split is invisible to it, and frames on one
 * socket cannot reorder. 768 KB leaves headroom under the 1 MiB frame cap and
 * stays below the 4 MiB out-ring high-water for typical oversized grids. */
#define SNAPSHOT_BODY_MAX (768u << 10)

void session_attach(session *s, struct client *c, uint16_t cols, uint16_t rows) {
    for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++) {
        if (s->clients[i] == c) return;
        if (!s->clients[i]) {
            s->clients[i] = c;
            /* Force geometry to the newest client (last-resize-wins). */
            session_resize(s, cols, rows);
            /* Real grid snapshot: repaint blob restores screen content,
             * cursor, and tracked modes exactly as the app left them. */
            pane *p = session_active_pane(s);
            char *blob = NULL;
            size_t blob_len = (p && p->vt) ? vt_snapshot(p->vt, &blob) : 0;
            static const char fallback[] = "\x1b[0m\x1b[2J\x1b[H";
            const char *body = blob_len ? blob : fallback;
            size_t body_len = blob_len ? blob_len : sizeof fallback - 1;

            size_t first = body_len < SNAPSHOT_BODY_MAX ? body_len : SNAPSHOT_BODY_MAX;
            size_t payload_len = 12 + first;
            uint8_t *payload = xmalloc(payload_len);
            put_u16(payload, s->view_cols);
            put_u16(payload + 2, s->view_rows);
            put_u64(payload + 4, p ? sb_total_lines(p->sb) : 0);
            memcpy(payload + 12, body, first);
            client_send(c, MSG_SNAPSHOT, payload, (uint32_t)payload_len);
            free(payload);
            /* client_send disconnects on backlog and then ignores further
             * sends itself; checking client_alive here just stops burning CPU
             * serializing chunks nobody will get. */
            for (size_t off = first; off < body_len && client_alive(c);
                 off += SNAPSHOT_BODY_MAX) {
                size_t n = body_len - off;
                if (n > SNAPSHOT_BODY_MAX) n = SNAPSHOT_BODY_MAX;
                client_send(c, MSG_OUTPUT, body + off, (uint32_t)n);
            }
            free(blob);
            return;
        }
    }
    client_disconnect(c); /* session full */
}

void session_detach(session *s, struct client *c) {
    for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++)
        if (s->clients[i] == c) s->clients[i] = NULL;
    /* Detach is a durability point. */
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use) sb_flush(s->panes[i].sb);
}

static void pane_stdin(pane *p, const uint8_t *data, uint32_t len) {
    if (p->child.master_fd < 0) return;
    /* PTY master write; kernel buffers. Short writes under flood are
     * tolerable for keyboard input in M1; M3 adds a staging ring. */
    ssize_t off = 0;
    while (off < (ssize_t)len) {
        ssize_t n = write(p->child.master_fd, data + off, len - (size_t)off);
        if (n > 0) { off += n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

void session_stdin(session *s, const uint8_t *data, uint32_t len) {
    pane *p = session_active_pane(s);
    if (p) pane_stdin(p, data, len);
}

void session_resize(session *s, uint16_t cols, uint16_t rows) {
    if (!cols || !rows || (cols == s->view_cols && rows == s->view_rows)) return;
    s->view_cols = cols;
    s->view_rows = rows;
    /* One pane fills the whole view, so the geometries coincide. The split
     * tree (5c) is what will make pane geometry diverge from view geometry. */
    pane *p = session_active_pane(s);
    if (!p) return;
    p->cols = cols;
    p->rows = rows;
    /* Grid reflow first, then TIOCSWINSZ (SIGWINCH makes the app repaint
     * into the new geometry). */
    if (p->vt) vt_resize(p->vt, rows, cols);
    if (p->child.master_fd >= 0) pty_resize(p->child.master_fd, cols, rows);
}
