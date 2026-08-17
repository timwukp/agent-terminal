/* server.c — unix socket listener, per-client framing, request dispatch. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/path.h"
#include "common/proto.h"
#include "common/ring.h"
#include "common/xutil.h"
#include "handoff.h"
#include "loop.h"
#include "session.h"

#define MAX_CLIENTS 32
#define CLIENT_OUT_MAX (4u << 20) /* 4 MiB high-water: hit it → disconnect */
#define PRE_HELLO_BUDGET 64
/* PRE_HELLO_BUDGET bounds how many BYTES a client may send before HELLO; it
 * says nothing about TIME, so a peer that connects and sends nothing at all
 * spends none of it and holds its slot forever. At MAX_CLIENTS such peers
 * server_accept has no slot left and closes every real client immediately —
 * one process with 32 open sockets and no traffic locks the user out of their
 * own sessions. So a client that has not identified itself within this window
 * loses its slot. 5 s is ~3 orders of magnitude above a local HELLO round trip
 * (the client sends it immediately after connect, on the same machine), which
 * is the margin that keeps this from ever firing on a real client. */
#define HELLO_DEADLINE_MS 5000

typedef struct client {
    int fd;
    bool in_use;
    bool hello_done;
    uint16_t caps;          /* MSG_HELLO flags (CLIENT_CAP_PANES) */
    uint16_t cols, rows;    /* last geometry from ATTACH/RESIZE */
    uint64_t connected_at;  /* now_ms() at accept; HELLO_DEADLINE_MS baseline */
    ring in, out;
    session *attached; /* NULL until ATTACH */
    uint8_t scratch[PROTO_MAX_PAYLOAD];
} client;

static client g_clients[MAX_CLIENTS];
static int g_listen_fd = -1;
static char g_socket_path[512];

static void client_io(int fd, short revents, void *ud);

/* ---- outbound ---- */

static void client_flush(client *c) {
    while (ring_len(&c->out)) {
        uint8_t buf[16384];
        size_t n = ring_peek(&c->out, buf, sizeof buf);
        ssize_t w = write(c->fd, buf, n);
        if (w > 0) { ring_consume(&c->out, (size_t)w); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        client_disconnect(c);
        return;
    }
    loop_mod_fd(c->fd, ring_len(&c->out) ? (POLLIN | POLLOUT) : POLLIN);
}

void client_send(struct client *c, uint8_t type, const void *payload, uint32_t len) {
    if (!c->in_use) return;
    if (!proto_write_frame(&c->out, type, payload, len)) {
        /* Slow consumer past high-water: drop it; reconnect gets a snapshot. */
        log_msg(LOG_WARN, "client fd %d over %u backlog, disconnecting", c->fd, CLIENT_OUT_MAX);
        client_disconnect(c);
        return;
    }
    client_flush(c);
}

bool client_alive(const struct client *c) { return c->in_use; }

bool client_wants_panes(const struct client *c) {
    return (c->caps & CLIENT_CAP_PANES) != 0;
}

void client_geometry(const struct client *c, uint16_t *cols, uint16_t *rows) {
    if (cols) *cols = c->cols;
    if (rows) *rows = c->rows;
}

void client_disconnect(struct client *c) {
    if (!c->in_use) return;
    if (c->attached) session_detach(c->attached, c);
    loop_del_fd(c->fd);
    close(c->fd);
    ring_free(&c->in);
    ring_free(&c->out);
    c->in_use = false;
}

static void client_err(client *c, uint16_t code, const char *msg) {
    size_t mlen = strlen(msg);
    if (mlen > 512) mlen = 512;
    uint8_t payload[4 + 512];
    put_u16(payload, code);
    put_u16(payload + 2, (uint16_t)mlen);
    /* Length-prefixed wire field, not a C string. */
    memcpy(payload + 4, msg, mlen); /* NOLINT(bugprone-not-null-terminated-result) */
    client_send(c, MSG_ERR, payload, (uint32_t)(4 + mlen));
}

/* ---- request handlers ---- */

/* The exact size a full MSG_SESSION_LIST2 payload can reach, so the buffers
 * that hold one are sized by the layout instead of by PROTO_MAX_PAYLOAD's
 * 1 MiB. 19 is the per-entry overhead of the larger of the two list layouts,
 * handle_list2: 2 entry-length prefix + 1 name length + 2 cols + 2 rows + 1 live
 * + 1 nclients + 4 pid + 4 exit status + 1 npanes + 1 zoomed, with the name
 * itself counted by SESSION_NAME_MAX; the leading 2 is the list's count field.
 *
 * Both list encoders still break out when the next entry would not fit, but a
 * runtime break silently TRUNCATES the user's session list; this assertion is
 * what turns "someone raised MAX_SESSIONS" from a buffer overflow into a build
 * failure. With the encoder now bounded by this constant rather than by
 * PROTO_MAX_PAYLOAD, the assertion is what proves the break is unreachable —
 * so it is load-bearing for the sessions-changed diff too, which must see the
 * whole table or it cannot notice a change in the tail of it. */
#define SESSION_LIST2_MAX (2 + (size_t)MAX_SESSIONS * (SESSION_NAME_MAX + 19))
_Static_assert(SESSION_LIST2_MAX <= PROTO_MAX_PAYLOAD,
               "MAX_SESSIONS x max session entry no longer fits one MSG_SESSION_LIST "
               "payload: raise PROTO_MAX_PAYLOAD or paginate the list message");

static void handle_list(client *c) {
    uint8_t payload[PROTO_MAX_PAYLOAD];
    size_t off = 2;
    uint16_t count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *s = session_at(i);
        if (!s) continue;
        size_t nlen = strlen(s->name);
        int ncli = 0;
        for (int j = 0; j < MAX_CLIENTS_PER_SESSION; j++)
            if (s->clients[j]) ncli++;
        /* MSG_SESSION_LIST is positional with no per-entry length, so it can
         * never grow per-pane fields (a parser mis-reads the *next* entry).
         * The active pane's child stands for the session here, which is exact
         * while sessions have one pane; `ls` showing panes needs a new
         * message, not an extension of this one. */
        pane *ap = session_active_pane(s);
        /* Bound the write like handle_list2 does. Today MAX_SESSIONS(64) x
         * (15 + 63) is far under PROTO_MAX_PAYLOAD, so this never fires — but
         * the safety was resting entirely on a cap declared in another header,
         * which makes raising that cap a buffer overflow rather than a
         * truncated list. */
        size_t entry = 1 + nlen + 2 + 2 + 1 + 1 + 4 + 4;
        if (off + entry > sizeof payload) break;
        payload[off++] = (uint8_t)nlen;
        memcpy(payload + off, s->name, nlen);
        off += nlen;
        put_u16(payload + off, s->view_cols); off += 2;
        put_u16(payload + off, s->view_rows); off += 2;
        payload[off++] = (ap && ap->child.pid > 0) ? 1 : 0;
        payload[off++] = (uint8_t)ncli;
        put_u32(payload + off, (uint32_t)(ap ? ap->child.pid : -1)); off += 4;
        put_u32(payload + off, (uint32_t)(ap ? ap->exit_status : 0)); off += 4;
        count++;
    }
    put_u16(payload, count);
    client_send(c, MSG_SESSION_LIST, payload, (uint32_t)off);
}

/* Serialize the whole session table as a MSG_SESSION_LIST2 payload into `out`
 * (at least SESSION_LIST2_MAX bytes); returns the length written.
 *
 * Lifted out of handle_list2 because it has a SECOND caller now — the
 * sessions-changed gate below diffs these exact bytes to decide whether
 * anything a client can observe has changed. Keeping it one function is the
 * point, not a tidiness preference: the gate must watch precisely the fields
 * this message carries, and a future append to the entry layout has to be
 * covered automatically rather than remembered. Two copies would drift, and
 * the failure would be silent in the worst direction — a field that changes
 * without ever notifying anyone. */
static size_t encode_session_list2(uint8_t *out, size_t cap) {
    size_t off = 2;
    uint16_t count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *s = session_at(i);
        if (!s) continue;
        size_t nlen = strlen(s->name);
        int ncli = 0, npanes = 0;
        for (int j = 0; j < MAX_CLIENTS_PER_SESSION; j++)
            if (s->clients[j]) ncli++;
        for (int j = 0; j < MAX_PANES_PER_SESSION; j++)
            if (s->panes[j].in_use) npanes++;
        pane *ap = session_active_pane(s);
        size_t entry = 1 + nlen + 2 + 2 + 1 + 1 + 4 + 4 + 1 + 1;
        if (off + 2 + entry > cap) break;
        put_u16(out + off, (uint16_t)entry); off += 2;
        out[off++] = (uint8_t)nlen;
        memcpy(out + off, s->name, nlen); /* NOLINT(bugprone-not-null-terminated-result) */
        off += nlen;
        put_u16(out + off, s->view_cols); off += 2;
        put_u16(out + off, s->view_rows); off += 2;
        out[off++] = (ap && ap->child.pid > 0) ? 1 : 0;
        out[off++] = (uint8_t)ncli;
        put_u32(out + off, (uint32_t)(ap ? ap->child.pid : -1)); off += 4;
        put_u32(out + off, (uint32_t)(ap ? ap->exit_status : 0)); off += 4;
        out[off++] = (uint8_t)npanes;
        out[off++] = s->zoomed_id != 255 ? 1 : 0;
        count++;
    }
    put_u16(out, count);
    return off;
}

static void handle_list2(client *c) {
    uint8_t payload[SESSION_LIST2_MAX];
    size_t off = encode_session_list2(payload, sizeof payload);
    client_send(c, MSG_SESSION_LIST2, payload, (uint32_t)off);
}

/* ---- sessions-changed notification (MSG_SESSIONS_CHANGED) ----
 *
 * The last list the daemon believes capable clients have, stored as the BYTES
 * rather than a hash of them. A hash would be smaller and the comparison
 * cheaper, but a collision here does not delay an update — it drops it
 * permanently, and the client would sit showing a stale list with nothing to
 * make it ask again. 5,250 bytes is a cheap price for not having to reason
 * about that. (Same argument, same words, as the GUI's panel ChangeGate.)
 *
 * A poll of the encoder rather than a hook in each mutator, deliberately. The
 * fields MSG_SESSION_LIST2 exposes are written from session_new, session_kill,
 * the SIGCHLD bottom half, attach, detach, split, close_pane, select/zoom and
 * resize — nine producers today, and hand-hooking a set that size means the
 * tenth one silently does not notify. Diffing the encoder's output has exactly
 * one producer, which is the encoder. */
static uint8_t g_last_list[SESSION_LIST2_MAX];
static size_t g_last_list_len;

/* Seed the baseline from the table as it stands at listen time, called from
 * server_init.
 *
 * Needed because "nothing encoded yet" is not a state any client saw: the first
 * tick would otherwise announce the step from an empty buffer to the table as it
 * already was. Seeding rather than suppressing that first notification is the
 * important half. Suppression loses a REAL change — a client that connects and
 * creates a session inside the daemon's first 20 ms would have its own creation
 * folded into the baseline and then never hear about anything, which is not
 * hypothetical: the autospawn path connects immediately after starting the
 * daemon, and at-client's real-daemon test reproduced exactly that and hung.
 *
 * Safe here as an ORDERING guarantee rather than a timing bet: server_init is
 * what creates the listener, so no client can have completed a HELLO yet and the
 * seeded state cannot be one anybody observed. main.c's handoff import runs
 * before this too, so a reload seeds the restored table and announces nothing
 * spurious to the clients that reconnect after it. */
static void session_changes_seed(void) {
    g_last_list_len = encode_session_list2(g_last_list, sizeof g_last_list);
}

static bool client_wants_session_events(const client *c) {
    return c->hello_done && (c->caps & CLIENT_CAP_SESSION_EVENTS) != 0;
}

/* Re-encode the list; if it differs from what was last announced, tell every
 * capable client to ask again. Driven by the daemon tick, which is what
 * coalesces a burst into one notification. */
void server_broadcast_session_changes(void) {
    uint8_t now[SESSION_LIST2_MAX];
    size_t n = encode_session_list2(now, sizeof now);
    bool changed = n != g_last_list_len || memcmp(now, g_last_list, n) != 0;
    if (!changed) return;
    /* Recorded BEFORE the fan-out, not after. client_send disconnects a client
     * that is past its backlog high-water, and a disconnect detaches it, which
     * changes nclients — i.e. the loop below can change the very thing being
     * announced. Storing first means that change is simply a difference the
     * NEXT tick finds and announces; storing afterwards would record the
     * post-disconnect state as already-announced and lose it. */
    memcpy(g_last_list, now, n);
    g_last_list_len = n;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &g_clients[i];
        if (!c->in_use || !client_wants_session_events(c)) continue;
        client_send(c, MSG_SESSIONS_CHANGED, NULL, 0);
    }
}

static void handle_new(client *c, const uint8_t *p, size_t len) {
    if (len < 7) { client_err(c, ERR_BAD_REQUEST, "short NEW_SESSION"); return; }
    /* Clamped here as well as in the model, so no absurd value is ever
     * RECORDED: c->cols feeds the smallest-wins vote in session_attach, and a
     * clamped session paired with a 65535-wide client record is a divergence
     * waiting for a future reader. Clamp, don't reject — a client is allowed
     * to ask for a window bigger than we will paint. */
    uint16_t cols = session_clamp_cols(get_u16(p));
    uint16_t rows = session_clamp_rows(get_u16(p + 2));
    uint8_t nlen = p[4];
    if ((size_t)5 + nlen + 2 > len || nlen == 0 || nlen > SESSION_NAME_MAX) {
        client_err(c, ERR_BAD_REQUEST, "bad name");
        return;
    }
    char name[SESSION_NAME_MAX + 1];
    memcpy(name, p + 5, nlen);
    name[nlen] = '\0';
    /* The name becomes a directory, so it must be one path component. Length
     * alone was checked before, and `../escape` was accepted: the daemon
     * mkdir'd it outside the sessions dir and wrote a scrollback log there.
     * The client screens names too, but a daemon must not trust a client. */
    if (!at_valid_session_name(name)) {
        client_err(c, ERR_BAD_REQUEST, "invalid session name");
        return;
    }

    uint16_t argv_bytes = get_u16(p + 5 + nlen);
    if ((size_t)7 + nlen + argv_bytes > len) { client_err(c, ERR_BAD_REQUEST, "bad argv"); return; }

    /* argv arrives NUL-separated; default to $SHELL when empty. */
    char *argbuf = NULL;
    char *argv[64];
    int argc = 0;
    if (argv_bytes > 0) {
        argbuf = xmalloc((size_t)argv_bytes + 1);
        memcpy(argbuf, p + 7 + nlen, argv_bytes);
        argbuf[argv_bytes] = '\0';
        for (char *q = argbuf; q < argbuf + argv_bytes && argc < 63;) {
            argv[argc++] = q;
            q += strlen(q) + 1;
        }
    }
    if (argc == 0) {
        const char *sh = getenv("SHELL");
        argv[argc++] = (char *)(sh && *sh ? sh : "/bin/sh");
    }
    argv[argc] = NULL;

    c->cols = cols;
    c->rows = rows;
    session *s = session_new(name, argv, cols, rows);
    free(argbuf);
    if (!s) {
        client_err(c, errno == EEXIST ? ERR_NAME_TAKEN : ERR_INTERNAL, strerror(errno));
        return;
    }
    /* Leave the old session the way handle_attach does. Without this the
     * previous session keeps this client in its clients[] forever: g_clients
     * is a static table of reused slots, so once this connection closes the
     * stale entry names whoever lands in that slot next — a client that never
     * asked for the session receives its OUTPUT and votes in its
     * smallest-attached-client geometry, and `ls` counts it. Only reachable
     * from a connection that creates twice (or attaches, then creates); both
     * shipped clients create once per connection, which is why it went
     * unnoticed. */
    if (c->attached && c->attached != s) session_detach(c->attached, c);
    c->attached = s;
    session_attach(s, c, cols, rows);
}

static void handle_attach(client *c, const uint8_t *p, size_t len) {
    if (len < 6) { client_err(c, ERR_BAD_REQUEST, "short ATTACH"); return; }
    uint16_t cols = session_clamp_cols(get_u16(p));      /* see handle_new */
    uint16_t rows = session_clamp_rows(get_u16(p + 2));
    /* p[4] = pane_id, reserved 0 in v1 */
    uint8_t nlen = p[5];
    if ((size_t)6 + nlen > len || nlen == 0 || nlen > SESSION_NAME_MAX) {
        client_err(c, ERR_BAD_REQUEST, "bad name");
        return;
    }
    char name[SESSION_NAME_MAX + 1];
    memcpy(name, p + 6, nlen);
    name[nlen] = '\0';
    session *s = session_find(name);
    if (!s) { client_err(c, ERR_NO_SESSION, "no such session"); return; }
    if (c->attached && c->attached != s) session_detach(c->attached, c);
    c->cols = cols;
    c->rows = rows;
    c->attached = s;
    session_attach(s, c, cols, rows);
    /* p[4]: pane selection on attach. 0 = don't change (the pre-pane client
     * has always sent 0), 255 = active (a no-op), else select by id. */
    if (p[4] != 0 && p[4] != 255 && client_wants_panes(c))
        session_select_pane(s, 0, p[4]);
}

static void handle_kill(client *c, const uint8_t *p, size_t len) {
    if (len < 1 || (size_t)1 + p[0] > len) { client_err(c, ERR_BAD_REQUEST, "bad name"); return; }
    char name[SESSION_NAME_MAX + 1];
    uint8_t nlen = p[0] > SESSION_NAME_MAX ? SESSION_NAME_MAX : p[0];
    memcpy(name, p + 1, nlen);
    name[nlen] = '\0';
    session *s = session_find(name);
    if (!s) { client_err(c, ERR_NO_SESSION, "no such session"); return; }
    session_kill(s);
    handle_list(c); /* confirm with fresh list */
}

static void dispatch(client *c, uint8_t type, const uint8_t *p, size_t len) {
    if (!c->hello_done) {
        if (type != MSG_HELLO || len < 4) { client_disconnect(c); return; }
        uint16_t ver = get_u16(p);
        if (ver != PROTO_VERSION) {
            client_err(c, ERR_VERSION, "daemon speaks protocol v1; upgrade your client");
            client_disconnect(c);
            return;
        }
        c->caps = len >= 4 ? get_u16(p + 2) : 0;
        uint8_t ok[12];
        put_u16(ok, PROTO_VERSION);
        put_u32(ok + 2, (uint32_t)getpid());
        /* The pid is stable across an in-place restart by design, so it cannot
         * tell a client (or a test) that a reload happened. The generation
         * counter can. Appended, so a v1 client that reads 6 bytes is fine. */
        put_u32(ok + 6, handoff_generation());
        /* server_flags: appended after generation, same additive rule. Lets a
         * capable client say "this daemon has no panes" instead of a silent
         * no-op when its split chord goes unanswered. Bits are OR'd in, never
         * assigned one at a time: a client that tests for panes with
         * `flags == SERVER_CAP_PANES` rather than a mask would break the day a
         * second bit appeared, so both bits ship together and the integration
         * test asserts BOTH are present. */
        put_u16(ok + 10, SERVER_CAP_PANES | SERVER_CAP_SESSION_EVENTS);
        c->hello_done = true;
        client_send(c, MSG_HELLO_OK, ok, sizeof ok);
        return;
    }
    switch (type) {
    case MSG_LIST_SESSIONS: handle_list(c); break;
    case MSG_LIST_SESSIONS2: handle_list2(c); break;
    case MSG_NEW_SESSION:   handle_new(c, p, len); break;
    case MSG_ATTACH:        handle_attach(c, p, len); break;
    case MSG_KILL_SESSION:  handle_kill(c, p, len); break;
    case MSG_RELOAD:
        /* Answered before the restart, not after: the reply must reach the
         * client while its socket is still open, and every socket closes as the
         * handoff begins. So this acknowledges "accepted", not "completed" —
         * the client confirms completion by reconnecting and comparing the
         * generation in HELLO_OK. */
        log_msg(LOG_INFO, "reload requested by client fd %d", c->fd);
        client_send(c, MSG_PONG, NULL, 0);
        handoff_request();
        break;
    case MSG_DETACH:
        if (c->attached) { session_detach(c->attached, c); c->attached = NULL; }
        break;
    case MSG_STDIN_DATA:
        if (c->attached) session_stdin(c->attached, p, (uint32_t)len);
        break;
    case MSG_RESIZE:
        if (len >= 4) { /* see handle_new */
            c->cols = session_clamp_cols(get_u16(p));
            c->rows = session_clamp_rows(get_u16(p + 2));
        }
        if (c->attached && len >= 4) session_resize(c->attached, c->cols, c->rows);
        break;
    case MSG_SPLIT_PANE: {
        if (!c->attached || len < 2) { client_err(c, ERR_BAD_REQUEST, "short SPLIT_PANE"); break; }
        errno = 0;
        if (!session_split(c->attached, p[1], p[0] != 0))
            client_err(c, errno == ENOSPC ? ERR_INTERNAL : ERR_BAD_REQUEST,
                       errno == ENOSPC ? "no free pane slot"
                                       : "pane too small to split or no such pane");
        break;
    }
    case MSG_CLOSE_PANE:
        if (!c->attached || len < 1) { client_err(c, ERR_BAD_REQUEST, "short CLOSE_PANE"); break; }
        if (!session_close_pane(c->attached, p[0]))
            client_err(c, ERR_BAD_REQUEST, "no such pane");
        break;
    case MSG_SELECT_PANE:
        if (!c->attached || len < 2) { client_err(c, ERR_BAD_REQUEST, "short SELECT_PANE"); break; }
        if (!session_select_pane(c->attached, p[0], p[1]))
            client_err(c, ERR_BAD_REQUEST, "no such pane");
        break;
    case MSG_SCROLLBACK_REQ: {
        if (!c->attached || len < 12) break;
        uint64_t start = get_u64(p);
        uint32_t maxn = get_u32(p + 8);
        if (maxn > 1000) maxn = 1000;
        static sb_line_ref refs[1000];
        /* The pane_id byte is a true append to the original fixed 12-byte
         * payload: read only when present, 255/absent = active. */
        pane *ap = len >= 13 ? session_pane_by_id(c->attached, p[12])
                             : session_active_pane(c->attached);
        /* Lines the ring still holds come from the ring; older ones are seeked
         * out of the log. Serving the miss matters because the attach snapshot
         * advertises sb_total_lines() — the WHOLE history — so a client that
         * scrolls past the ring used to be told the range exists and then
         * handed nothing. The read is bounded (one page plus at most
         * SB_INDEX_STEP-1 records of sweep, ~285 KB measured) which is what
         * keeps it safe on this single-threaded loop's 20 ms tick. */
        static char sbtext[PROTO_MAX_PAYLOAD];
        uint32_t got = ap ? sb_fetch_deep(ap->sb, start, maxn, refs, 1000,
                                          sbtext, sizeof sbtext)
                          : 0;
        /* payload: u64 first_seq, u32 nlines, then {u32 len, bytes}... */
        static uint8_t payload[PROTO_MAX_PAYLOAD];
        size_t off = 12;
        uint32_t emitted = 0;
        for (uint32_t i = 0; i < got; i++) {
            if (off + 4 + refs[i].len > sizeof payload) break;
            put_u32(payload + off, refs[i].len);
            memcpy(payload + off + 4, refs[i].text, refs[i].len);
            off += 4 + refs[i].len;
            emitted++;
        }
        put_u64(payload, emitted ? refs[0].seq : 0);
        put_u32(payload + 8, emitted);
        client_send(c, MSG_SCROLLBACK_DATA, payload, (uint32_t)off);
        break;
    }
    case MSG_PING:
        client_send(c, MSG_PONG, p, (uint32_t)len);
        break;
    default: break; /* unknown types are skipped — forward compat */
    }
}

/* ---- socket plumbing ---- */

/* POLLHUP must NOT short-circuit the read: a hangup means the peer will send
 * nothing more, not that what it already sent is void. The kernel reports
 * POLLIN|POLLHUP together when bytes are still queued on a closed socket, so
 * returning early here threw away complete frames that had already arrived.
 *
 * That was a real, reproducible data loss, not a theoretical one. A client that
 * writes MSG_NEW_SESSION and closes immediately — which is exactly what
 * `agent-terminal new` does when stdin is already at EOF, because attach.c
 * treats an instant stdin EOF as a detach — had its request dropped. Measured on
 * two runs putting identical bytes on the wire and differing only in a 250 ms
 * sleep before close(): 20 of 20 sessions lost on Linux and 9 of 20 on macOS
 * when closing at once, 0 of 20 on both when lingering. It surfaced as a 3-in-8
 * flake in the integration suite, where a *later* test finds a session missing
 * that `new` had reported as created.
 *
 * So: drain, dispatch, and only then honor the hangup. POLLERR/POLLNVAL still
 * disconnect at once — those say the fd itself is unusable, not just finished.
 *
 * Two details here are deliberately NOT load-bearing for that fix, and were
 * confirmed so by mutation (test_close_race.sh still passes with either one
 * reverted), so do not read them as the mechanism:
 *  - Accepting a POLLHUP-only wakeup. A socket at EOF sets POLLIN as well, so
 *    this branch does not fire in practice; it is there so a platform that
 *    reports the hangup alone cannot spin on an fd nobody reads.
 *  - The exact position of the eof disconnect below. What the measurement
 *    turns on is only that the read and dispatch happen at all; the read loop
 *    breaks on a short read before it ever sees 0, so on the very poll round
 *    that carries the frame, eof is usually still false and the disconnect
 *    lands on a later round. Deferring it cannot leak the fd either way,
 *    because POLLHUP is level-triggered and poll() keeps re-reporting it. */
static void client_io(int fd, short revents, void *ud) {
    client *c = ud;
    if (revents & (POLLERR | POLLNVAL)) { client_disconnect(c); return; }
    if (revents & POLLOUT) client_flush(c);
    if (!c->in_use) return;
    if (!(revents & (POLLIN | POLLHUP))) return;

    bool eof = false;
    uint8_t buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) { eof = true; break; }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            client_disconnect(c);
            return;
        }
        if (!c->hello_done && ring_len(&c->in) + (size_t)n > PRE_HELLO_BUDGET) {
            client_disconnect(c); /* garbage before HELLO */
            return;
        }
        if (!ring_write(&c->in, buf, (size_t)n)) { client_disconnect(c); return; }
        if ((size_t)n < sizeof buf) break;
    }

    for (;;) {
        uint8_t type;
        size_t len;
        int rc = proto_read_frame(&c->in, &type, c->scratch, &len);
        if (rc == 0) break;
        if (rc < 0) { client_disconnect(c); return; }
        dispatch(c, type, c->scratch, len);
        if (!c->in_use) return; /* dispatch may disconnect */
    }

    /* A half-open peer that shut down only its write side gets the same
     * treatment as a full close: this protocol has no request whose reply the
     * daemon may keep streaming after the client stops talking, and a session
     * created here keeps running without any client attached.
     *
     * This IS load-bearing, just for reaping rather than for the fix above:
     * POLLHUP no longer disconnects on its own, so without this a peer that
     * shuts down its write side and lingers holds a client slot forever. At
     * MAX_CLIENTS such peers the daemon stops accepting entirely — which is
     * what part 4 of test_close_race.sh measures. */
    if (eof) client_disconnect(c);
}

static bool peer_uid_ok(int fd) {
#ifdef __linux__
    struct ucred cr;
    socklen_t n = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &n) != 0) return false;
    return cr.uid == getuid();
#else
    uid_t euid;
    gid_t egid;
    if (getpeereid(fd, &euid, &egid) != 0) return false;
    return euid == getuid();
#endif
}

static void server_accept(int fd, short revents, void *ud) {
    (void)revents; (void)ud;
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0) return;
    if (!peer_uid_ok(cfd)) {
        log_msg(LOG_WARN, "rejected connection from foreign uid");
        close(cfd);
        return;
    }
    int fl = fcntl(cfd, F_GETFL);
    if (fl >= 0) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
    fcntl(cfd, F_SETFD, FD_CLOEXEC);

    client *c = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!g_clients[i].in_use) { c = &g_clients[i]; break; }
    if (!c) { close(cfd); return; }

    c->fd = cfd;
    c->in_use = true;
    c->hello_done = false;
    c->connected_at = now_ms();
    c->attached = NULL;
    ring_init(&c->in, 4096, (size_t)2 * PROTO_MAX_PAYLOAD);
    ring_init(&c->out, 4096, CLIENT_OUT_MAX);
    if (loop_add_fd(cfd, POLLIN, client_io, c) != 0) {
        /* Out of poll slots: nothing would ever read this fd, so the client
         * would hang forever on a connection the daemon has silently shelved.
         * Closing makes it retry or fail visibly instead. client_disconnect
         * is wrong here — it calls loop_del_fd on an fd that was never added,
         * and there is no session to detach from yet. */
        log_msg(LOG_ERR, "no event-loop slot for client fd %d, closing", cfd);
        close(cfd);
        ring_free(&c->in);
        ring_free(&c->out);
        c->in_use = false;
    }
}

/* Called from the daemon tick, so the deadline is enforced by wall time rather
 * than by the silent peer's own traffic — which is the whole point: a peer that
 * sends nothing generates no events, so nothing in the read path can ever
 * notice it. Resolution is the 20 ms tick, so the real cutoff is
 * HELLO_DEADLINE_MS + one tick. */
void server_reap_idle(void) {
    uint64_t now = now_ms();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &g_clients[i];
        if (!c->in_use || c->hello_done) continue;
        if (now - c->connected_at < HELLO_DEADLINE_MS) continue;
        log_msg(LOG_WARN, "client fd %d sent no HELLO in %d ms, dropping",
                c->fd, HELLO_DEADLINE_MS);
        client_disconnect(c);
    }
}

/* Is this fd in the listening state?
 *
 * SO_ACCEPTCONN is the direct question and is read-only, but on macOS it returns
 * ENOPROTOOPT for AF_UNIX — measured, not assumed — so it cannot be the only
 * check. The portable fallback is listen() itself: it succeeds on a socket that
 * is already listening and fails EINVAL on a connected one, verified identically
 * on macOS/arm64 and Linux/x86_64. Re-listening is idempotent here because the
 * backlog is the same value server_init passes; it does not drop connections
 * already queued.
 *
 * The fallback is not just belt-and-braces: an *accepted* client fd reports the
 * same bound sun_path as the listener on both platforms, so without a
 * listening-state check the path comparison below would accept a client
 * connection as the listener. */
static bool fd_is_listening(int fd) {
    int acc = 0;
    socklen_t alen = sizeof acc;
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acc, &alen) == 0) return acc != 0;
    if (errno != ENOPROTOOPT) return false;
    return listen(fd, 16) == 0;
}

/* Confirm an fd carried across execv really is our listening socket. The fd
 * number alone proves nothing — numbers are reused, so a stale state file could
 * name the fd that is now our own stderr. Three checks, cheapest first: it is a
 * socket, it is listening, and its bound name is the path we were asked for. */
static bool inherited_listener_ok(int fd, const char *socket_path) {
    if (fd <= 2) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) return false;

    if (!fd_is_listening(fd)) return false;

    struct sockaddr_un sa;
    socklen_t slen = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) != 0) return false;
    if (sa.sun_family != AF_UNIX) return false;
    /* sun_path is not guaranteed NUL-terminated by getsockname; bound it. */
    char bound[sizeof sa.sun_path + 1];
    memcpy(bound, sa.sun_path, sizeof sa.sun_path);
    bound[sizeof sa.sun_path] = '\0';
    return strcmp(bound, socket_path) == 0;
}

int server_init(const char *socket_path, int inherited_fd) {
    strncpy(g_socket_path, socket_path, sizeof g_socket_path - 1);

    if (inherited_fd >= 0) {
        if (!inherited_listener_ok(inherited_fd, socket_path)) {
            /* Fall through to a fresh bind; the fd is closed and no longer
             * consulted (the adopt path below returns before reaching it). */
            log_msg(LOG_WARN, "inherited fd %d is not the listener for %s; rebinding",
                    inherited_fd, socket_path);
            close(inherited_fd);
        } else {
            int ifl = fcntl(inherited_fd, F_GETFL);
            if (ifl >= 0) fcntl(inherited_fd, F_SETFL, ifl | O_NONBLOCK);
            fcntl(inherited_fd, F_SETFD, FD_CLOEXEC);
            if (loop_add_fd(inherited_fd, POLLIN, server_accept, NULL) != 0) {
                close(inherited_fd);
                errno = ENOSPC;
                return -1;
            }
            g_listen_fd = inherited_fd;
            log_msg(LOG_INFO, "adopted inherited listener fd %d", inherited_fd);
            /* Both success paths seed, not just the fresh-bind one: this is the
             * reload path, where the table is at its least empty. */
            session_changes_seed();
            return 0;
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    size_t plen = strlen(socket_path);
    if (plen >= sizeof sa.sun_path) { close(fd); errno = ENAMETOOLONG; return -1; }
    memcpy(sa.sun_path, socket_path, plen + 1); /* bounded by the check above */

    /* Stale socket: only unlink if nothing answers.
     *
     * This is a courtesy, not the mutual exclusion — it is racy (two daemons
     * can both find nothing answering, both unlink, both bind) and the real
     * guarantee is the flock in lockfile.c, which main.c takes before calling
     * here. Keeping the probe means a daemon that was SIGKILLed leaves a socket
     * its successor can clean up without operator action. */
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        close(fd);
        errno = EADDRINUSE;
        return -1;
    }
    unlink(socket_path);

    mode_t old = umask(0177); /* socket lands 0600 */
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    umask(old);
    if (rc != 0 || listen(fd, 16) != 0) { close(fd); return -1; }

    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (loop_add_fd(fd, POLLIN, server_accept, NULL) != 0) {
        close(fd);
        unlink(socket_path);
        errno = ENOSPC;
        return -1;
    }
    g_listen_fd = fd;
    session_changes_seed();
    return 0;
}

int server_prepare_handoff(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].in_use) client_disconnect(&g_clients[i]);
    /* The listener stays bound and stays in the event loop. loop state does not
     * survive execv, but leaving the fd registered here is harmless and means a
     * failed handoff needs no re-registration to keep serving. The socket path
     * is not unlinked: the next image adopts this same fd, so unlinking would
     * remove the name clients reconnect to while the socket kept working only
     * for connections already made. */
    return g_listen_fd;
}

void server_shutdown(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].in_use) client_disconnect(&g_clients[i]);
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    if (g_socket_path[0]) unlink(g_socket_path);
}
