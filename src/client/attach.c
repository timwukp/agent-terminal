#define _DARWIN_C_SOURCE
#include "attach.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME_MAX_WIRE 255

#include "common/path.h"
#include "common/proto.h"
#include "common/ring.h"
#include "common/xutil.h"
#include "tty.h"

static volatile sig_atomic_t g_winch = 0;
static int g_winch_pipe[2] = {-1, -1};

static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
    ssize_t r = write(g_winch_pipe[1], "w", 1);
    (void)r;
}

/* ---- connection ---- */

static int try_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

static int hello(int fd) {
    ring out;
    ring_init(&out, 64, 0);
    uint8_t p[4];
    put_u16(p, PROTO_VERSION);
    put_u16(p + 2, 0);
    proto_write_frame(&out, MSG_HELLO, p, 4);
    uint8_t buf[64];
    size_t n = ring_read(&out, buf, sizeof buf);
    ring_free(&out);
    if (write(fd, buf, n) != (ssize_t)n) return -1;

    /* Blocking read for HELLO_OK / ERR. */
    uint8_t hdr[PROTO_HDR_SIZE];
    size_t got = 0;
    while (got < sizeof hdr) {
        ssize_t r = read(fd, hdr + got, sizeof hdr - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    uint32_t plen = get_u32(hdr);
    if (plen > 4096) return -1;
    uint8_t payload[4096];
    got = 0;
    while (got < plen) {
        ssize_t r = read(fd, payload + got, plen - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    if (hdr[4] == MSG_ERR) {
        if (plen >= 4) {
            uint16_t mlen = get_u16(payload + 2);
            fprintf(stderr, "agent-terminal: %.*s\n", (int)mlen, payload + 4);
        }
        return -1;
    }
    return hdr[4] == MSG_HELLO_OK ? 0 : -1;
}

int daemon_connect(int auto_start) {
    char path[512];
    if (at_socket_path(path, sizeof path) != 0) {
        fprintf(stderr, "agent-terminal: runtime dir: %s\n", strerror(errno));
        return -1;
    }
    int fd = try_connect(path);
    if (fd < 0 && auto_start) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("agent-terminald", "agent-terminald", (char *)NULL);
            /* also try alongside our own binary via PATH failure fallthrough */
            _exit(127);
        }
        if (pid > 0) {
            int st;
            waitpid(pid, &st, 0); /* daemonizes and exits quickly */
            for (int i = 0; i < 20 && fd < 0; i++) {
                usleep(100 * 1000);
                fd = try_connect(path);
            }
        }
    }
    if (fd < 0) {
        fprintf(stderr, "agent-terminal: cannot reach daemon at %s\n", path);
        return -1;
    }
    if (hello(fd) != 0) { close(fd); return -1; }
    return fd;
}

/* ---- input scanning: detach chord Ctrl-\ then Ctrl-d ---- */

#define KEY_CTRL_BACKSLASH 0x1c
#define KEY_CTRL_D 0x04
#define CHORD_TIMEOUT_MS 500

typedef struct {
    int armed;          /* saw Ctrl-\, waiting for the second key */
    uint64_t armed_at;
} chord;

/* Returns 1 if a detach was requested. Forwardable bytes land in fwd
 * (caller-sized >= 2*len). */
static int scan_input(chord *ch, const uint8_t *in, size_t len, uint8_t *fwd, size_t *fwdlen) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = in[i];
        if (ch->armed) {
            ch->armed = 0;
            if (b == KEY_CTRL_D) return 1;
            fwd[o++] = KEY_CTRL_BACKSLASH; /* forward the swallowed byte */
            fwd[o++] = b;
            continue;
        }
        if (b == KEY_CTRL_BACKSLASH) {
            ch->armed = 1;
            ch->armed_at = now_ms();
            continue;
        }
        fwd[o++] = b;
    }
    *fwdlen = o;
    return 0;
}

/* ---- attach pump ---- */

static int send_frame(int fd, uint8_t type, const void *payload, size_t len) {
    uint8_t hdr[PROTO_HDR_SIZE];
    put_u32(hdr, (uint32_t)len);
    hdr[4] = type;
    struct iovec iov[2] = {{.iov_base = hdr, .iov_len = sizeof hdr},
                           {.iov_base = (void *)payload, .iov_len = len}};
    size_t total = sizeof hdr + len;
    size_t sent = 0;
    while (sent < total) {
        ssize_t w = writev(fd, iov, 2);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
        /* adjust iov for partial writes */
        size_t skip = (size_t)w;
        for (int i = 0; i < 2; i++) {
            size_t take = skip < iov[i].iov_len ? skip : iov[i].iov_len;
            iov[i].iov_base = (uint8_t *)iov[i].iov_base + take;
            iov[i].iov_len -= take;
            skip -= take;
        }
    }
    return 0;
}

static int send_attach(int fd, const char *name, uint16_t cols, uint16_t rows) {
    uint8_t p[6 + NAME_MAX_WIRE];
    size_t nlen = strlen(name);
    put_u16(p, cols);
    put_u16(p + 2, rows);
    p[4] = 0; /* pane_id reserved */
    p[5] = (uint8_t)nlen;
    memcpy(p + 6, name, nlen);
    return send_frame(fd, MSG_ATTACH, p, 6 + nlen);
}

static int send_new(int fd, const char *name, char *const argv[], int argc,
                    uint16_t cols, uint16_t rows) {
    uint8_t p[4096];
    size_t nlen = strlen(name);
    put_u16(p, cols);
    put_u16(p + 2, rows);
    p[4] = (uint8_t)nlen;
    memcpy(p + 5, name, nlen);
    size_t off = 5 + nlen + 2;
    size_t abytes = 0;
    for (int i = 0; i < argc; i++) {
        size_t al = strlen(argv[i]) + 1;
        if (off + abytes + al > sizeof p) return -1;
        memcpy(p + off + abytes, argv[i], al);
        abytes += al;
    }
    put_u16(p + 5 + nlen, (uint16_t)abytes);
    return send_frame(fd, MSG_NEW_SESSION, p, off + abytes);
}

static void pump_resize(int fd) {
    uint16_t cols, rows;
    if (tty_get_size(&cols, &rows) != 0) return;
    uint8_t p[4];
    put_u16(p, cols);
    put_u16(p + 2, rows);
    send_frame(fd, MSG_RESIZE, p, 4);
}

int attach_run(const char *name, char *const argv[], int argc) {
    if (pipe(g_winch_pipe) != 0) return 1;
    for (int i = 0; i < 2; i++) fcntl(g_winch_pipe[i], F_SETFL, O_NONBLOCK);
    struct sigaction sa = {.sa_handler = on_winch, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);

    uint16_t cols = 80, rows = 24;
    tty_get_size(&cols, &rows);

    int first = 1;
    int backoff_ms = 250;
    for (;;) { /* reconnect loop */
        int fd = daemon_connect(first);
        if (fd < 0) {
            if (first) return 1;
            if (backoff_ms > 4000) {
                fprintf(stderr, "agent-terminal: giving up.\n");
                return 1;
            }
            usleep((useconds_t)backoff_ms * 1000);
            backoff_ms *= 2;
            continue;
        }
        backoff_ms = 250;

        int rc;
        if (first && argv) rc = send_new(fd, name, argv, argc, cols, rows);
        else rc = send_attach(fd, name, cols, rows);
        if (rc != 0) { close(fd); return 1; }
        first = 0;

        if (tty_raw_enter() != 0) { close(fd); return 1; }

        ring in;
        ring_init(&in, 16384, 0);
        uint8_t *scratch = xmalloc(PROTO_MAX_PAYLOAD);
        chord ch = {0};
        int detached = 0, exited = 0, exit_code = 0, conn_lost = 0;

        while (!detached && !exited && !conn_lost) {
            struct pollfd pfds[3] = {
                {.fd = 0, .events = POLLIN},
                {.fd = fd, .events = POLLIN},
                {.fd = g_winch_pipe[0], .events = POLLIN},
            };
            int timeout = ch.armed ? CHORD_TIMEOUT_MS : -1;
            int n = poll(pfds, 3, timeout);
            if (n < 0 && errno != EINTR) { conn_lost = 1; break; }

            if (ch.armed && now_ms() - ch.armed_at >= CHORD_TIMEOUT_MS) {
                /* timeout: forward the held Ctrl-\ */
                uint8_t b = KEY_CTRL_BACKSLASH;
                send_frame(fd, MSG_STDIN_DATA, &b, 1);
                ch.armed = 0;
            }
            if (n <= 0) continue;

            if (pfds[2].revents & POLLIN) {
                uint8_t drain[32];
                while (read(g_winch_pipe[0], drain, sizeof drain) > 0) {}
                pump_resize(fd);
            }

            if (pfds[0].revents & POLLIN) {
                uint8_t buf[4096], fwd[8192];
                ssize_t r = read(0, buf, sizeof buf);
                if (r > 0) {
                    size_t fwdlen = 0;
                    if (scan_input(&ch, buf, (size_t)r, fwd, &fwdlen)) {
                        detached = 1;
                    }
                    if (fwdlen && send_frame(fd, MSG_STDIN_DATA, fwd, fwdlen) != 0)
                        conn_lost = 1;
                } else if (r == 0) {
                    detached = 1; /* stdin gone: treat as detach */
                }
            }

            if (pfds[1].revents & (POLLIN | POLLHUP)) {
                uint8_t buf[16384];
                ssize_t r = read(fd, buf, sizeof buf);
                if (r <= 0) {
                    if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                    conn_lost = 1;
                } else {
                    ring_write(&in, buf, (size_t)r);
                    for (;;) {
                        uint8_t type;
                        size_t len;
                        int frc = proto_read_frame(&in, &type, scratch, &len);
                        if (frc == 0) break;
                        if (frc < 0) { conn_lost = 1; break; }
                        switch (type) {
                        case MSG_OUTPUT: {
                            size_t off = 0;
                            while (off < len) {
                                ssize_t w = write(1, scratch + off, len - off);
                                if (w < 0) { if (errno == EINTR) continue; break; }
                                off += (size_t)w;
                            }
                            break;
                        }
                        case MSG_SNAPSHOT: {
                            if (len < 12) break;
                            size_t off = 12;
                            while (off < len) {
                                ssize_t w = write(1, scratch + off, len - off);
                                if (w < 0) { if (errno == EINTR) continue; break; }
                                off += (size_t)w;
                            }
                            break;
                        }
                        case MSG_SESSION_EXITED:
                            exited = 1;
                            exit_code = len >= 4 ? (int)get_u32(scratch) : 0;
                            break;
                        case MSG_ERR: {
                            tty_raw_leave();
                            if (len >= 4) {
                                uint16_t mlen = get_u16(scratch + 2);
                                fprintf(stderr, "agent-terminal: %.*s\n", (int)mlen, scratch + 4);
                            }
                            ring_free(&in);
                            free(scratch);
                            close(fd);
                            return 1;
                        }
                        default: break;
                        }
                    }
                }
            }
        }

        ring_free(&in);
        free(scratch);
        close(fd);
        tty_raw_leave();

        if (detached) {
            printf("[detached — session '%s' keeps running]\n", name);
            return 0;
        }
        if (exited) {
            printf("[session '%s' exited: %d]\n", name, exit_code);
            return exit_code;
        }
        /* conn_lost: daemon went away — retry loop */
        fprintf(stderr, "[agent-terminal: connection lost, reconnecting…]\n");
    }
}
