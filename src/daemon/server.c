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
#include "loop.h"
#include "session.h"

#define MAX_CLIENTS 32
#define CLIENT_OUT_MAX (4u << 20) /* 4 MiB high-water: hit it → disconnect */
#define PRE_HELLO_BUDGET 64

typedef struct client {
    int fd;
    bool in_use;
    bool hello_done;
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
    memcpy(payload + 4, msg, mlen);
    client_send(c, MSG_ERR, payload, (uint32_t)(4 + mlen));
}

/* ---- request handlers ---- */

static void handle_list(client *c) {
    uint8_t payload[PROTO_MAX_PAYLOAD];
    size_t off = 2;
    uint16_t count = 0;
    for (int i = 0;; i++) {
        session *s = session_at(i);
        if (i >= MAX_SESSIONS) break;
        if (!s) continue;
        size_t nlen = strlen(s->name);
        int ncli = 0;
        for (int j = 0; j < MAX_CLIENTS_PER_SESSION; j++)
            if (s->clients[j]) ncli++;
        payload[off++] = (uint8_t)nlen;
        memcpy(payload + off, s->name, nlen);
        off += nlen;
        put_u16(payload + off, s->cols); off += 2;
        put_u16(payload + off, s->rows); off += 2;
        payload[off++] = s->child.pid > 0 ? 1 : 0;
        payload[off++] = (uint8_t)ncli;
        put_u32(payload + off, (uint32_t)s->child.pid); off += 4;
        put_u32(payload + off, (uint32_t)s->exit_status); off += 4;
        count++;
    }
    put_u16(payload, count);
    client_send(c, MSG_SESSION_LIST, payload, (uint32_t)off);
}

static void handle_new(client *c, const uint8_t *p, size_t len) {
    if (len < 7) { client_err(c, ERR_BAD_REQUEST, "short NEW_SESSION"); return; }
    uint16_t cols = get_u16(p), rows = get_u16(p + 2);
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

    session *s = session_new(name, argv, cols, rows);
    free(argbuf);
    if (!s) {
        client_err(c, errno == EEXIST ? ERR_NAME_TAKEN : ERR_INTERNAL, strerror(errno));
        return;
    }
    c->attached = s;
    session_attach(s, c, cols, rows);
}

static void handle_attach(client *c, const uint8_t *p, size_t len) {
    if (len < 6) { client_err(c, ERR_BAD_REQUEST, "short ATTACH"); return; }
    uint16_t cols = get_u16(p), rows = get_u16(p + 2);
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
    c->attached = s;
    session_attach(s, c, cols, rows);
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
        uint8_t ok[6];
        put_u16(ok, PROTO_VERSION);
        put_u32(ok + 2, (uint32_t)getpid());
        c->hello_done = true;
        client_send(c, MSG_HELLO_OK, ok, sizeof ok);
        return;
    }
    switch (type) {
    case MSG_LIST_SESSIONS: handle_list(c); break;
    case MSG_NEW_SESSION:   handle_new(c, p, len); break;
    case MSG_ATTACH:        handle_attach(c, p, len); break;
    case MSG_KILL_SESSION:  handle_kill(c, p, len); break;
    case MSG_DETACH:
        if (c->attached) { session_detach(c->attached, c); c->attached = NULL; }
        break;
    case MSG_STDIN_DATA:
        if (c->attached) session_stdin(c->attached, p, (uint32_t)len);
        break;
    case MSG_RESIZE:
        if (c->attached && len >= 4) session_resize(c->attached, get_u16(p), get_u16(p + 2));
        break;
    case MSG_SCROLLBACK_REQ: {
        if (!c->attached || len < 12) break;
        uint64_t start = get_u64(p);
        uint32_t maxn = get_u32(p + 8);
        if (maxn > 1000) maxn = 1000;
        static sb_line_ref refs[1000];
        uint32_t got = sb_fetch(c->attached->sb, start, maxn, refs, 1000);
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

static void client_io(int fd, short revents, void *ud) {
    client *c = ud;
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) { client_disconnect(c); return; }
    if (revents & POLLOUT) client_flush(c);
    if (!(revents & POLLIN) || !c->in_use) return;

    uint8_t buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) { client_disconnect(c); return; }
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
    c->attached = NULL;
    ring_init(&c->in, 4096, 2 * PROTO_MAX_PAYLOAD);
    ring_init(&c->out, 4096, CLIENT_OUT_MAX);
    loop_add_fd(cfd, POLLIN, client_io, c);
}

int server_init(const char *socket_path) {
    strncpy(g_socket_path, socket_path, sizeof g_socket_path - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof sa.sun_path) { close(fd); errno = ENAMETOOLONG; return -1; }
    strcpy(sa.sun_path, socket_path);

    /* Stale socket: only unlink if nothing answers. */
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
    g_listen_fd = fd;
    loop_add_fd(fd, POLLIN, server_accept, NULL);
    return 0;
}

void server_shutdown(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].in_use) client_disconnect(&g_clients[i]);
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    if (g_socket_path[0]) unlink(g_socket_path);
}
