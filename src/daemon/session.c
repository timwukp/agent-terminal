#include "session.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/proto.h"
#include "common/xutil.h"
#include "composite.h"
#include "handoff.h"
#include "loop.h"
#include "server.h"

static session g_sessions[MAX_SESSIONS];

static void pane_pty_readable(int fd, short revents, void *ud);
static void session_apply_layout(session *s);
static void session_send_layout(session *s);
static void session_leave_composite(session *s);

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
        if (s->panes[i].in_use && s->panes[i].id == s->active_id)
            return &s->panes[i];
    /* active_id names a pane that just closed: fall back to any live pane
     * (the close path re-points active_id, so this is a guard). */
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use) return &s->panes[i];
    return NULL; /* unreachable for an in-use session; callers may assume */
}

pane *session_pane_by_id(session *s, uint8_t id) {
    if (id == 255) return session_active_pane(s);
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use && s->panes[i].id == id) return &s->panes[i];
    return NULL;
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
static pane *pane_alloc(session *s, uint8_t id, uint16_t cols, uint16_t rows) {
    pane *p = NULL;
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (!s->panes[i].in_use) { p = &s->panes[i]; break; }
    if (!p) { errno = ENOSPC; return NULL; }

    memset(p, 0, sizeof *p);
    p->sess = s;
    /* Wire id, not the array index. Pane 0 is the session's first pane
     * forever (its scrollback file must stay byte-identical to pre-pane
     * logs); split panes draw 1..254 round-robin so an id is never reused
     * while a close naming it could still be in flight. */
    p->id = id;
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
    layout_init(&s->lt, 0); /* pane slot 0 fills the view */
    layout_reflow(&s->lt, s->view_cols, s->view_rows);
    s->active_id = 0;
    s->last_id = 0;
    s->next_id = 1;
    s->zoomed_id = 255;
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

    pane *p = pane_alloc(s, 0, s->view_cols, s->view_rows);
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
    /* Single-pane convenience used by tests; the handoff path drives the
     * three-phase API directly. */
    layout lt;
    layout_init(&lt, 0);
    session *s = session_import_begin(name, cols, rows, 0, 0, 1, &lt);
    if (!s) return NULL;
    session_import_pane(s, 0, 0, master_fd, pid, cols, rows, blob, blob_len);
    if (!session_import_finish(s)) return NULL;
    return s;
}

session *session_import_begin(const char *name, uint16_t view_cols,
                              uint16_t view_rows, uint8_t active_id,
                              uint8_t last_id, uint8_t next_id,
                              const layout *lt) {
    session *s = session_alloc(name, view_cols, view_rows);
    if (!s) return NULL;
    s->lt = *lt;
    s->active_id = active_id;
    s->last_id = last_id;
    s->next_id = next_id ? next_id : 1;
    s->zoomed_id = 255; /* zoom is ephemeral view state; a reload drops it */
    s->in_use = true; /* so session_find sees it during pane import */
    return s;
}

pane *session_import_pane(session *s, uint8_t slot, uint8_t id, int master_fd,
                          pid_t pid, uint16_t cols, uint16_t rows,
                          const uint8_t *blob, size_t blob_len) {
    if (slot >= MAX_PANES_PER_SESSION || s->panes[slot].in_use) {
        errno = EINVAL;
        return NULL;
    }
    /* pane_alloc scans for a free slot from 0; the state file names an exact
     * slot because the layout tree stores pane INDICES. Claim neighbours
     * temporarily? No — just allocate directly into the named slot. */
    pane *p = &s->panes[slot];
    memset(p, 0, sizeof *p);
    p->sess = s;
    p->id = id;
    p->cols = cols;
    p->rows = rows;
    p->child.master_fd = -1;
    p->child.pid = -1;

    vt_callbacks cb = {
        .on_response = vt_cb_response,
        .on_scrollback_line = vt_cb_scrollback,
        .on_title = vt_cb_title,
        .on_bell = vt_cb_bell,
        .on_passthrough = NULL,
    };
    p->vt = vt_new(p->rows, p->cols, &cb, p);
    if (!p->vt) { errno = ENOMEM; return NULL; }
    p->sb = sb_open_pane(s->name, p->id, 0, 0);
    if (!p->sb)
        log_msg(LOG_WARN, "session '%s': scrollback disabled (%s)", s->name,
                strerror(errno));

    p->child.master_fd = master_fd;
    p->child.pid = pid;
    p->in_use = true;

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
        return p;
    }
    /* Re-apply geometry: the state file's cols/rows are authoritative, and the
     * PTY already has them, but a client that resized during the gap did not
     * reach us. Harmless when they match — pty_resize is idempotent. */
    pty_resize(master_fd, p->cols, p->rows);
    log_msg(LOG_INFO, "session '%s': restored pane %u pid %d on fd %d (%ux%u, "
                      "%zu-byte screen)", s->name, id, (int)pid, master_fd,
            p->cols, p->rows, blob_len);
    return p;
}

bool session_import_finish(session *s) {
    int live = 0;
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use) live++;
    if (live == 0) {
        s->in_use = false;
        return false;
    }
    /* Panes the import dropped (unusable fd) leave dangling leaves in the
     * tree; prune them so reflow and compositing see only live panes. */
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (!s->panes[i].in_use)
            layout_close(&s->lt, s->view_cols, s->view_rows, (int8_t)i);
    if (session_pane_by_id(s, s->active_id) == NULL) {
        pane *fb = session_active_pane(s);
        if (fb) s->active_id = fb->id;
    }
    layout_reflow(&s->lt, s->view_cols, s->view_rows);
    session_apply_layout(s);
    return true;
}

void session_kill(session *s) {
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use && s->panes[i].child.pid > 0)
            kill(s->panes[i].child.pid, SIGHUP);
    session_free_slot(s); /* also disconnects every attached client */
}

/* A pane's child was reaped. With one pane that is the session exiting,
 * exactly as before panes existed; with more, the pane closes, its sibling
 * absorbs the rectangle, and capable clients hear MSG_PANE_EXITED. */
static void session_pane_reaped(session *s, pane *p, int8_t slot) {
    uint8_t id = p->id;
    int live = 0;
    for (int k = 0; k < MAX_PANES_PER_SESSION; k++)
        if (s->panes[k].in_use) live++;

    if (live <= 1) {
        uint8_t payload[4];
        put_u32(payload, (uint32_t)p->exit_status);
        for (int k = 0; k < MAX_CLIENTS_PER_SESSION; k++)
            if (s->clients[k])
                client_send(s->clients[k], MSG_SESSION_EXITED, payload, 4);
        pane_free_slot(p);
        /* Freeing here means `ls` never reports a session as dead — the
         * client's "dead (exit N)" branch is unreachable, and that is
         * intentional. pane_free_slot flushed the final screen to
         * scrollback first, so `history` still recovers it. */
        session_free_slot(s);
        return;
    }

    uint8_t payload[5];
    payload[0] = id;
    put_u32(payload + 1, (uint32_t)p->exit_status);
    for (int k = 0; k < MAX_CLIENTS_PER_SESSION; k++)
        if (s->clients[k] && client_wants_panes(s->clients[k]))
            client_send(s->clients[k], MSG_PANE_EXITED, payload, 5);

    /* Free the slot BEFORE reflow so the survivor's resize sees final
     * geometry; layout_close reflows the sibling into the parent's rect. */
    pane_free_slot(p);
    layout_close(&s->lt, s->view_cols, s->view_rows, slot);
    if (s->active_id == id)
        s->active_id = s->last_id != id ? s->last_id : 0;
    if (session_pane_by_id(s, s->active_id) == NULL) {
        pane *fb = session_active_pane(s);
        if (fb) s->active_id = fb->id;
    }
    session_apply_layout(s);
    if (!session_should_composite(s)) session_leave_composite(s);
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
                session_pane_reaped(s, p, (int8_t)j);
            }
        }
    }
}

/* Compose and broadcast one frame for a session, if anything is dirty.
 * Damage is cleared once, after every client got the same bytes — a
 * per-client clear would blank the frame for the others. */
static void session_composite_one(session *s) {
    bool full = s->layout_dirty;
    char *frame = NULL;
    size_t n = composite_frame(s, full, &frame);
    if (s->layout_dirty) {
        session_send_layout(s);
        s->layout_dirty = false;
    }
    if (!n) { free(frame); return; }
    for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++)
        if (s->clients[i])
            client_send(s->clients[i], MSG_OUTPUT, frame, (uint32_t)n);
    free(frame);
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use && s->panes[i].vt)
            vt_damage_clear(s->panes[i].vt);
    s->last_frame_ms = now_ms();
}

void session_composite_all(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *s = &g_sessions[i];
        if (!s->in_use || !session_should_composite(s)) continue;
        bool dirty = s->layout_dirty;
        for (int j = 0; j < MAX_PANES_PER_SESSION && !dirty; j++)
            if (s->panes[j].in_use && s->panes[j].vt && vt_any_dirty(s->panes[j].vt))
                dirty = true;
        if (dirty) session_composite_one(s);
    }
}

/* Echo-latency bound for the composited path. A pure 20 ms tick adds up to
 * 20 ms to keystroke echo at >=2 panes — perceptible to a fast typist and
 * certain to be misattributed to the shell. The bucket composites straight
 * from the read path when the last frame is at least this old; the tick
 * catches whatever the bucket skips. */
#define COMPOSITE_MIN_INTERVAL_MS 8

static void pane_pty_readable(int fd, short revents, void *ud) {
    pane *p = ud;
    session *s = p->sess;
    (void)revents;
    bool composite = session_should_composite(s);
    uint8_t buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            /* VT engine tracks screen state (snapshot source on reattach). */
            if (p->vt) vt_feed(p->vt, buf, (size_t)n);
            if (!composite) {
                /* Single pane: raw tee, perfect fidelity. This path must
                 * survive compositing unchanged — the byte-identical guard
                 * test (test_panes_compat.sh) pins it. */
                for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++)
                    if (s->clients[i])
                        client_send(s->clients[i], MSG_OUTPUT, buf, (uint32_t)n);
            }
            if ((size_t)n < sizeof buf) break;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        /* EOF or EIO: child side closed. Reap will finish the cleanup;
         * stop watching now to avoid a hot loop. */
        loop_del_fd(fd);
        break;
    }
    if (composite && now_ms() - s->last_frame_ms >= COMPOSITE_MIN_INTERVAL_MS)
        session_composite_one(s);
}

/* Push the layout into each pane: resize engines + PTYs to their rectangles
 * and mark a full repaint. Called after split/close/resize at >=2 panes. */
static void session_apply_layout(session *s) {
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++) {
        pane *p = &s->panes[i];
        if (!p->in_use) continue;
        uint16_t x, y, cols, rows;
        if (s->zoomed_id != 255 && p->id == s->zoomed_id) {
            /* Zoom overlay: the tree keeps its rectangles; only this pane's
             * applied geometry is the full view. Other panes keep their tree
             * rects (they are not rendered while zoomed, but their PTY size
             * stays sane for the running apps). */
            x = 0; y = 0; cols = s->view_cols; rows = s->view_rows;
        } else if (!layout_pane_rect(&s->lt, (int8_t)i, &x, &y, &cols, &rows)) continue;
        p->x = x; p->y = y;
        if (cols != p->cols || rows != p->rows) {
            p->cols = cols;
            p->rows = rows;
            if (p->vt) vt_resize(p->vt, rows, cols);
            if (p->child.master_fd >= 0) pty_resize(p->child.master_fd, cols, rows);
        }
    }
    s->layout_dirty = true;
}

/* MSG_LAYOUT payload for capable clients. */
static void session_send_layout(session *s) {
    uint8_t payload[6 + MAX_PANES_PER_SESSION * 9];
    size_t off = 6;
    uint8_t n = 0;
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++) {
        pane *p = &s->panes[i];
        if (!p->in_use) continue;
        payload[off] = p->id;
        put_u16(payload + off + 1, p->x);
        put_u16(payload + off + 3, p->y);
        put_u16(payload + off + 5, p->cols);
        put_u16(payload + off + 7, p->rows);
        off += 9;
        n++;
    }
    put_u16(payload, s->view_cols);
    put_u16(payload + 2, s->view_rows);
    payload[4] = s->active_id;
    payload[5] = n;
    for (int k = 0; k < MAX_CLIENTS_PER_SESSION; k++)
        if (s->clients[k] && client_wants_panes(s->clients[k]))
            client_send(s->clients[k], MSG_LAYOUT, payload, (uint32_t)off);
}

/* Dropping from composited to single-pane: the tick stops compositing, so
 * the transition itself must broadcast the final state — MSG_LAYOUT for
 * capable clients, and a full single-pane repaint (leaving the composite's
 * autowrap-off is what would otherwise make the survivor unwrappable). */
static void session_leave_composite(session *s) {
    session_send_layout(s);
    s->layout_dirty = false;
    pane *p = session_active_pane(s);
    if (!p || !p->vt) return;
    char *blob = NULL;
    size_t n = vt_snapshot(p->vt, &blob);
    /* ?7h before the repaint: the composite ran with autowrap off, and the
     * snapshot only re-asserts the mode at its tail. Belt and braces. */
    static const char rearm[] = "\x1b[?7h";
    for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++)
        if (s->clients[i]) {
            client_send(s->clients[i], MSG_OUTPUT, rearm, sizeof rearm - 1);
            if (n) client_send(s->clients[i], MSG_OUTPUT, blob, (uint32_t)n);
        }
    free(blob);
    vt_damage_clear(p->vt);
}

/* Drop the zoom overlay (if any) and restore tree geometry. Safe to call
 * when not zoomed. */
static void session_unzoom(session *s) {
    if (s->zoomed_id == 255) return;
    s->zoomed_id = 255;
    session_apply_layout(s);
}

pane *session_split(session *s, uint8_t target, bool stacked) {
    session_unzoom(s); /* splitting under a zoom would be invisible geometry */
    pane *tp = session_pane_by_id(s, target);
    if (!tp) { errno = EINVAL; return NULL; }
    int8_t tslot = (int8_t)(tp - s->panes);

    int8_t nslot = -1;
    for (int8_t i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (!s->panes[i].in_use) { nslot = i; break; }
    if (nslot < 0) { errno = ENOSPC; return NULL; }

    /* The tree mutates only if minimums hold at CURRENT geometry. */
    if (!layout_split(&s->lt, s->view_cols, s->view_rows, tslot, nslot, stacked)) {
        errno = EINVAL;
        return NULL;
    }

    /* Allocate a wire id no live pane holds. 254 candidates for <= 5 live
     * split panes, so the loop always terminates. */
    uint8_t id;
    do {
        id = s->next_id;
        s->next_id = (uint8_t)(s->next_id == 254 ? 1 : s->next_id + 1);
    } while (session_pane_by_id(s, id) != NULL);

    uint16_t x = 0, y = 0, cols = 0, rows = 0;
    layout_pane_rect(&s->lt, nslot, &x, &y, &cols, &rows);
    pane *np = pane_alloc(s, id, cols, rows);
    if (!np) {
        int saved = errno;
        layout_close(&s->lt, s->view_cols, s->view_rows, nslot);
        errno = saved;
        return NULL;
    }

    const char *sh = getenv("SHELL");
    char *argv[2] = {(char *)(sh && *sh ? sh : "/bin/sh"), NULL};
    if (pty_spawn(&np->child, argv, cols, rows, "xterm-256color") != 0) {
        int saved = errno;
        pane_alloc_undo(np);
        layout_close(&s->lt, s->view_cols, s->view_rows, nslot);
        errno = saved;
        return NULL;
    }
    np->in_use = true;
    if (loop_add_fd(np->child.master_fd, POLLIN, pane_pty_readable, np) != 0) {
        int saved = errno;
        log_msg(LOG_ERR, "session '%s': no event-loop slot for pane fd %d",
                s->name, np->child.master_fd);
        kill(np->child.pid, SIGHUP);
        close(np->child.master_fd);
        np->child.master_fd = -1;
        pane_alloc_undo(np);
        layout_close(&s->lt, s->view_cols, s->view_rows, nslot);
        errno = saved ? saved : ENOSPC;
        return NULL;
    }

    s->last_id = s->active_id;
    s->active_id = id;
    session_apply_layout(s);
    log_msg(LOG_INFO, "session '%s': pane %u pid %d on fd %d (%ux%u at %u,%u)",
            s->name, id, (int)np->child.pid, np->child.master_fd,
            np->cols, np->rows, np->x, np->y);
    return np;
}

bool session_close_pane(session *s, uint8_t id) {
    pane *p = session_pane_by_id(s, id);
    if (!p) return false;
    session_unzoom(s);
    uint8_t real_id = p->id;
    int8_t slot = (int8_t)(p - s->panes);

    int live = 0;
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++)
        if (s->panes[i].in_use) live++;

    if (p->child.pid > 0) kill(p->child.pid, SIGHUP);
    if (live <= 1) {
        session_free_slot(s); /* last pane: the session dies with it */
        return true;
    }
    pane_free_slot(p);
    layout_close(&s->lt, s->view_cols, s->view_rows, slot);
    if (s->active_id == real_id)
        s->active_id = s->last_id != real_id ? s->last_id : 0;
    if (session_pane_by_id(s, s->active_id) == NULL) {
        pane *fb = session_active_pane(s);
        if (fb) s->active_id = fb->id;
    }
    session_apply_layout(s);
    if (!session_should_composite(s)) session_leave_composite(s);
    return true;
}

/* Geometrically nearest pane in a direction: among panes whose rectangle
 * lies strictly beyond the current pane's edge, pick the one whose center
 * is closest (edge distance first, then perpendicular offset — the tmux
 * feel: straight across beats diagonal). */
static pane *pane_in_direction(session *s, pane *cur, uint8_t mode) {
    int ccx = cur->x + cur->cols / 2, ccy = cur->y + cur->rows / 2;
    pane *best = NULL;
    long best_key = 0;
    for (int i = 0; i < MAX_PANES_PER_SESSION; i++) {
        pane *p = &s->panes[i];
        if (!p->in_use || p == cur) continue;
        int pcx = p->x + p->cols / 2, pcy = p->y + p->rows / 2;
        long primary, secondary;
        switch (mode) {
        case 4: /* up */
            if (p->y + p->rows > cur->y) continue;
            primary = cur->y - (p->y + p->rows);
            secondary = labs((long)pcx - ccx);
            break;
        case 5: /* down */
            if (p->y < cur->y + cur->rows) continue;
            primary = p->y - (cur->y + cur->rows);
            secondary = labs((long)pcx - ccx);
            break;
        case 6: /* right */
            if (p->x < cur->x + cur->cols) continue;
            primary = p->x - (cur->x + cur->cols);
            secondary = labs((long)pcy - ccy);
            break;
        default: /* 7 = left */
            if (p->x + p->cols > cur->x) continue;
            primary = cur->x - (p->x + p->cols);
            secondary = labs((long)pcy - ccy);
            break;
        }
        long key = primary * 4096 + secondary;
        if (!best || key < best_key) { best = p; best_key = key; }
    }
    return best;
}

bool session_select_pane(session *s, uint8_t mode, uint8_t id) {
    pane *cur = session_active_pane(s);
    if (!cur) return false;

    if (mode == 8) {
        /* Zoom toggle on the active pane. A no-op at one pane: the view is
         * already the whole screen, and arming zoom state there would only
         * surprise the next split. */
        if (s->zoomed_id != 255) {
            session_unzoom(s);
            return true;
        }
        if (!session_should_composite(s)) return true;
        s->zoomed_id = cur->id;
        session_apply_layout(s);
        return true;
    }

    pane *next = NULL;
    if (mode == 0) {
        next = session_pane_by_id(s, id);
    } else if (mode == 3) {
        next = session_pane_by_id(s, s->last_id);
    } else if (mode >= 4 && mode <= 7) {
        next = pane_in_direction(s, cur, mode);
        if (!next) return true; /* nothing that way: keep focus, no error */
    } else {
        /* next/prev in slot order, wrapping. */
        int8_t start = (int8_t)(cur - s->panes);
        int step = mode == 1 ? 1 : MAX_PANES_PER_SESSION - 1;
        for (int k = 1; k <= MAX_PANES_PER_SESSION; k++) {
            int8_t i = (int8_t)((start + k * step) % MAX_PANES_PER_SESSION);
            if (s->panes[i].in_use) { next = &s->panes[i]; break; }
        }
    }
    if (!next || next == cur) return next == cur;
    session_unzoom(s); /* focus moved: a zoom on the old pane hides the new one */
    s->last_id = s->active_id;
    s->active_id = next->id;
    s->layout_dirty = true; /* cursor + input modes move; full frame re-arms */
    return true;
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
            /* Geometry policy: last-resize-wins at one pane (unchanged from
             * the pre-pane daemon, guarded byte-for-byte); at >=2 panes the
             * SMALLEST attached client wins, or the composite would paint
             * outside a smaller client's screen. Applying smallest-wins
             * universally would change single-pane bytes — a separate PR. */
            if (session_should_composite(s)) {
                uint16_t mc = cols, mr = rows;
                for (int k = 0; k < MAX_CLIENTS_PER_SESSION; k++) {
                    if (!s->clients[k] || s->clients[k] == c) continue;
                    uint16_t oc, or_;
                    client_geometry(s->clients[k], &oc, &or_);
                    if (oc && oc < mc) mc = oc;
                    if (or_ && or_ < mr) mr = or_;
                }
                session_resize(s, mc, mr);
            } else {
                session_resize(s, cols, rows);
            }
            /* Real grid snapshot: repaint blob restores screen content,
             * cursor, and tracked modes exactly as the app left them. At
             * >=2 panes the equivalent is a full composite frame — the same
             * kind of byte blob, so the client cannot tell the difference. */
            pane *p = session_active_pane(s);
            char *blob = NULL;
            size_t blob_len;
            if (session_should_composite(s)) {
                blob_len = composite_frame(s, true, &blob);
                session_send_layout(s);
            } else {
                blob_len = (p && p->vt) ? vt_snapshot(p->vt, &blob) : 0;
            }
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
    if (!session_should_composite(s)) {
        /* One pane fills the whole view: geometries coincide, and this path
         * must stay byte-identical to the pre-pane daemon. */
        pane *p = session_active_pane(s);
        if (!p) return;
        p->cols = cols;
        p->rows = rows;
        /* Grid reflow first, then TIOCSWINSZ (SIGWINCH makes the app repaint
         * into the new geometry). */
        if (p->vt) vt_resize(p->vt, rows, cols);
        if (p->child.master_fd >= 0) pty_resize(p->child.master_fd, cols, rows);
        return;
    }
    /* On a shrink the tree cannot satisfy, rectangles clamp to >=1x1 and
     * still render: destroying a pane on resize is data loss. */
    layout_reflow(&s->lt, cols, rows);
    session_apply_layout(s);
}
