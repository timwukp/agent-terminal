/* agent-terminal — thin client CLI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "attach.h"
#include "common/proto.h"
#include "common/scrollback.h"
#include "common/xutil.h"

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  agent-terminal new    [-s name] [-- cmd args...]   create + attach\n"
            "  agent-terminal attach  -s name                     attach to session\n"
            "  agent-terminal ls                                  list sessions\n"
            "  agent-terminal history -s name                     dump scrollback (works for dead sessions)\n"
            "  agent-terminal kill    -s name                     kill session\n"
            "\ndetach without killing: Ctrl-\\ then Ctrl-d\n");
    exit(2);
}

static int cmd_ls(void) {
    int fd = daemon_connect(0);
    if (fd < 0) { printf("no sessions (daemon not running)\n"); return 0; }

    uint8_t hdr[PROTO_HDR_SIZE] = {0, 0, 0, 0, MSG_LIST_SESSIONS};
    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) { close(fd); return 1; }

    uint8_t rhdr[PROTO_HDR_SIZE];
    size_t got = 0;
    while (got < sizeof rhdr) {
        ssize_t r = read(fd, rhdr + got, sizeof rhdr - got);
        if (r <= 0) { close(fd); return 1; }
        got += (size_t)r;
    }
    uint32_t plen = get_u32(rhdr);
    if (rhdr[4] != MSG_SESSION_LIST || plen > PROTO_MAX_PAYLOAD) { close(fd); return 1; }
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
    for (uint16_t i = 0; i < count && off + 1 <= plen; i++) {
        uint8_t nlen = p[off++];
        if (off + nlen + 14 > plen) break;
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
        if (alive)
            printf("%s: %ux%u, pid %d, %u client%s\n", name, cols, rows, pid, ncli,
                   ncli == 1 ? "" : "s");
        else
            printf("%s: dead (exit %d)\n", name, status);
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

static int cmd_kill(const char *name) {
    int fd = daemon_connect(0);
    if (fd < 0) return 1;
    size_t nlen = strlen(name);
    uint8_t frame[PROTO_HDR_SIZE + 1 + 255];
    put_u32(frame, (uint32_t)(1 + nlen));
    frame[4] = MSG_KILL_SESSION;
    frame[5] = (uint8_t)nlen;
    memcpy(frame + 6, name, nlen);
    ssize_t total = (ssize_t)(PROTO_HDR_SIZE + 1 + nlen);
    int ok = write(fd, frame, (size_t)total) == total;
    close(fd);
    printf(ok ? "killed '%s'\n" : "failed to kill '%s'\n", name);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
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

    if (strcmp(verb, "ls") == 0) return cmd_ls();
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
