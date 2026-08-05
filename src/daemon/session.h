#ifndef AT_SESSION_H
#define AT_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "pty.h"
#include "common/scrollback.h"
#include "vt/vt.h"

#define SESSION_NAME_MAX 63
#define MAX_SESSIONS 64
#define MAX_CLIENTS_PER_SESSION 8

struct client; /* owned by server.c */

typedef struct session {
    char name[SESSION_NAME_MAX + 1];
    pty_child child;      /* pid == -1 once reaped */
    int exit_status;      /* valid when pid == -1 */
    uint16_t cols, rows;
    struct client *clients[MAX_CLIENTS_PER_SESSION];
    vt *vt;               /* screen state; snapshot source on reattach */
    scrollback *sb;       /* ring + disk persistence */
    bool in_use;
} session;

session *session_new(const char *name, char *const argv[], uint16_t cols, uint16_t rows);
session *session_find(const char *name);
void session_kill(session *s);            /* SIGHUP child, free slot */
void session_reap_children(void);         /* SIGCHLD bottom half */
void session_flush_all(void);             /* periodic scrollback flush */
void session_flush_screens_all(void);     /* shutdown: preserve visible screens */
int  session_count(void);
session *session_at(int idx);             /* iterate 0..MAX_SESSIONS-1, may be NULL */

void session_attach(session *s, struct client *c, uint16_t cols, uint16_t rows);
void session_detach(session *s, struct client *c);
void session_stdin(session *s, const uint8_t *data, uint32_t len);
void session_resize(session *s, uint16_t cols, uint16_t rows);

#endif
