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

static void session_pty_readable(int fd, short revents, void *ud);

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

static void vt_cb_response(void *ud, const char *buf, size_t len) {
    /* Query answers (DA/DSR/CPR) go back to the application via the PTY. A
     * duplicate would answer a question the app asked before the restart and
     * already received, arriving as unsolicited keyboard input — a literal
     * `[12;40R` typed into a shell prompt. */
    if (handoff_importing()) return;
    session *s = ud;
    session_stdin(s, (const uint8_t *)buf, (uint32_t)len);
}

static void vt_cb_scrollback(void *ud, const vt_cell *cells, uint16_t n) {
    /* Any line a replay pushed off the top would be a line the pre-restart
     * engine already stored, duplicating history that `history` and copy-mode
     * both read. */
    if (handoff_importing()) return;
    session *s = ud;
    sb_push_line(s->sb, cells, n);
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

static void session_flush_screen(session *s);

void session_flush_all(void) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].in_use) sb_flush(g_sessions[i].sb);
}

/* Daemon shutdown (SIGTERM/SIGINT). The children die with us, so this is the
 * last chance to preserve what is on their screens; without it a service
 * restart silently discarded every session's visible output, unlike an
 * explicit kill or a child exiting, which both flush via session_free_slot. */
void session_flush_screens_all(void) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].in_use) {
            session_flush_screen(&g_sessions[i]);
            sb_flush(g_sessions[i].sb);
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

/* Claim a slot and build everything that does not involve the child: the
 * screen engine and scrollback. Shared by session_new and session_import so a
 * restored session is indistinguishable from a fresh one afterwards — the
 * alternative, a second copy of this setup in handoff.c, is how a restored
 * session ends up with subtly different callbacks or no scrollback at all. */
static session *session_alloc(const char *name, uint16_t cols, uint16_t rows) {
    if (session_find(name)) { errno = EEXIST; return NULL; }
    session *s = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!g_sessions[i].in_use) { s = &g_sessions[i]; break; }
    if (!s) { errno = ENOSPC; return NULL; }

    memset(s, 0, sizeof *s);
    strncpy(s->name, name, SESSION_NAME_MAX);
    s->cols = cols ? cols : 80;
    s->rows = rows ? rows : 24;

    vt_callbacks cb = {
        .on_response = vt_cb_response,
        .on_scrollback_line = vt_cb_scrollback,
        .on_title = vt_cb_title,
        .on_bell = vt_cb_bell,
        .on_passthrough = NULL, /* raw tee already forwards everything live */
    };
    s->vt = vt_new(s->rows, s->cols, &cb, s);
    if (!s->vt) { errno = ENOMEM; return NULL; }
    /* Best-effort: a session without persistent scrollback still works.
     * sb_open resumes line_seq from what is on disk, which is what makes
     * scrollback numbering continuous across a restart. */
    s->sb = sb_open(s->name, 0, 0);
    if (!s->sb)
        log_msg(LOG_WARN, "session '%s': scrollback disabled (%s)", s->name,
                strerror(errno));
    return s;
}

static void session_alloc_undo(session *s) {
    vt_free(s->vt);
    s->vt = NULL;
    sb_close(s->sb);
    s->sb = NULL;
    s->in_use = false;
}

session *session_new(const char *name, char *const argv[], uint16_t cols, uint16_t rows) {
    session *s = session_alloc(name, cols, rows);
    if (!s) return NULL;

    if (pty_spawn(&s->child, argv, s->cols, s->rows, "xterm-256color") != 0) {
        int saved = errno;
        session_alloc_undo(s);
        errno = saved;
        return NULL;
    }
    s->in_use = true;
    if (loop_add_fd(s->child.master_fd, POLLIN, session_pty_readable, s) != 0) {
        /* Out of poll slots. The child exists but nothing would ever read its
         * output, so it would fill the PTY buffer and block forever with no
         * visible cause. Kill it and report, rather than hand back a session
         * that silently does not work. */
        int saved = errno;
        log_msg(LOG_ERR, "session '%s': no event-loop slot for fd %d", s->name,
                s->child.master_fd);
        kill(s->child.pid, SIGHUP);
        close(s->child.master_fd);
        s->child.master_fd = -1;
        session_alloc_undo(s);
        errno = saved ? saved : ENOSPC;
        return NULL;
    }
    log_msg(LOG_INFO, "session '%s': pid %d on fd %d", s->name, (int)s->child.pid,
            s->child.master_fd);
    return s;
}

session *session_import(const char *name, int master_fd, pid_t pid, uint16_t cols,
                        uint16_t rows, const uint8_t *blob, size_t blob_len) {
    session *s = session_alloc(name, cols, rows);
    if (!s) return NULL;

    s->child.master_fd = master_fd;
    s->child.pid = pid;
    s->in_use = true;

    /* Replay the pre-restart screen. vt_feed on a snapshot blob is the same
     * round-trip a reattaching client already relies on, so there is no second
     * serialization format here to keep correct. */
    if (blob_len && s->vt) vt_feed(s->vt, blob, blob_len);

    if (loop_add_fd(master_fd, POLLIN, session_pty_readable, s) != 0) {
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
    pty_resize(master_fd, s->cols, s->rows);
    log_msg(LOG_INFO, "session '%s': restored pid %d on fd %d (%ux%u, %zu-byte screen)",
            s->name, (int)pid, master_fd, s->cols, s->rows, blob_len);
    return s;
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
 * 20 empty records per session. Sessions ending on the alternate screen are
 * skipped: vt_line() exposes the active grid, and scrollback holds
 * primary-screen content only (vt.h), so flushing vim's or htop's live UI
 * here would both break that contract and bury the useful history. The cost
 * is that a session dying inside vim also loses the primary lines still on
 * screen — vt_line() cannot reach the inactive grid. Lines that had already
 * scrolled off are unaffected either way. */
static void session_flush_screen(session *s) {
    if (!s->vt || !s->sb) return;
    if (vt_get_modes(s->vt) & VT_MODE_ALTSCREEN) return;
    uint16_t rows = 0, cols = 0;
    vt_get_size(s->vt, &rows, &cols);
    uint16_t last = 0; /* one past the final non-blank row */
    for (uint16_t r = 0; r < rows; r++) {
        const vt_cell *line = vt_line(s->vt, r);
        if (!line) continue;
        for (uint16_t c = 0; c < cols; c++)
            if (line[c].cp != 0 && line[c].cp != ' ') { last = (uint16_t)(r + 1); break; }
    }
    for (uint16_t r = 0; r < last; r++) {
        const vt_cell *line = vt_line(s->vt, r);
        if (line) sb_push_line(s->sb, line, cols);
    }
}

static void session_free_slot(session *s) {
    session_flush_screen(s); /* before vt_free/sb_close destroy both sides */
    if (s->child.master_fd >= 0) {
        loop_del_fd(s->child.master_fd);
        close(s->child.master_fd);
        s->child.master_fd = -1;
    }
    vt_free(s->vt);
    s->vt = NULL;
    sb_close(s->sb);
    s->sb = NULL;
    s->in_use = false;
}

void session_kill(session *s) {
    if (s->child.pid > 0) kill(s->child.pid, SIGHUP);
    for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++)
        if (s->clients[i]) {
            struct client *c = s->clients[i];
            s->clients[i] = NULL;
            client_disconnect(c);
        }
    session_free_slot(s);
}

void session_reap_children(void) {
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session *s = &g_sessions[i];
            if (!s->in_use || s->child.pid != pid) continue;
            s->child.pid = -1;
            s->exit_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            log_msg(LOG_INFO, "session '%s': child exited %d", s->name, s->exit_status);
            uint8_t payload[4];
            put_u32(payload, (uint32_t)s->exit_status);
            for (int j = 0; j < MAX_CLIENTS_PER_SESSION; j++)
                if (s->clients[j])
                    client_send(s->clients[j], MSG_SESSION_EXITED, payload, 4);
            /* Freeing here means `ls` never reports a session as dead — the
             * client's "dead (exit N)" branch is unreachable, and that is
             * intentional for v1. session_free_slot flushes the final screen
             * to scrollback first, so `history` still recovers it. */
            session_free_slot(s);
        }
    }
}

static void session_pty_readable(int fd, short revents, void *ud) {
    session *s = ud;
    (void)revents;
    uint8_t buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            /* VT engine tracks screen state (snapshot source on reattach);
             * attached clients get the same bytes raw (perfect fidelity). */
            if (s->vt) vt_feed(s->vt, buf, (size_t)n);
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

void session_attach(session *s, struct client *c, uint16_t cols, uint16_t rows) {
    for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++) {
        if (s->clients[i] == c) return;
        if (!s->clients[i]) {
            s->clients[i] = c;
            /* Force geometry to the newest client (last-resize-wins). */
            session_resize(s, cols, rows);
            /* Real grid snapshot: repaint blob restores screen content,
             * cursor, and tracked modes exactly as the app left them. */
            char *blob = NULL;
            size_t blob_len = s->vt ? vt_snapshot(s->vt, &blob) : 0;
            static const char fallback[] = "\x1b[0m\x1b[2J\x1b[H";
            const char *body = blob_len ? blob : fallback;
            size_t body_len = blob_len ? blob_len : sizeof fallback - 1;

            size_t payload_len = 12 + body_len;
            uint8_t *payload = xmalloc(payload_len);
            put_u16(payload, s->cols);
            put_u16(payload + 2, s->rows);
            put_u64(payload + 4, sb_total_lines(s->sb));
            memcpy(payload + 12, body, body_len);
            client_send(c, MSG_SNAPSHOT, payload, (uint32_t)payload_len);
            free(payload);
            free(blob);
            return;
        }
    }
    client_disconnect(c); /* session full */
}

void session_detach(session *s, struct client *c) {
    for (int i = 0; i < MAX_CLIENTS_PER_SESSION; i++)
        if (s->clients[i] == c) s->clients[i] = NULL;
    sb_flush(s->sb); /* detach is a durability point */
}

void session_stdin(session *s, const uint8_t *data, uint32_t len) {
    if (s->child.master_fd < 0) return;
    /* PTY master write; kernel buffers. Short writes under flood are
     * tolerable for keyboard input in M1; M3 adds a staging ring. */
    ssize_t off = 0;
    while (off < (ssize_t)len) {
        ssize_t n = write(s->child.master_fd, data + off, len - (size_t)off);
        if (n > 0) { off += n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

void session_resize(session *s, uint16_t cols, uint16_t rows) {
    if (!cols || !rows || (cols == s->cols && rows == s->rows)) return;
    s->cols = cols;
    s->rows = rows;
    /* Grid reflow first, then TIOCSWINSZ (SIGWINCH makes the app repaint
     * into the new geometry). */
    if (s->vt) vt_resize(s->vt, rows, cols);
    if (s->child.master_fd >= 0) pty_resize(s->child.master_fd, cols, rows);
}
