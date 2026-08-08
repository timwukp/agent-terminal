#ifndef AT_SESSION_H
#define AT_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "pty.h"
#include "common/scrollback.h"
#include "vt/vt.h"

#define SESSION_NAME_MAX 63
#define MAX_SESSIONS 64
#define MAX_CLIENTS_PER_SESSION 8
#define MAX_PANES_PER_SESSION 6

struct client; /* owned by server.c */
struct session;

/* One child process with its screen and scrollback. Everything here used to
 * live directly in `session`; splits make it per-pane. Panes are a by-value
 * fixed array inside the session — client->attached is a raw pointer into
 * static g_sessions[], so nothing on this path may ever be realloc'd. */
typedef struct pane {
    struct session *sess; /* owning session, for VT callbacks */
    /* Wire id, NOT the array index. 0 is permanently the session's first
     * pane; later panes draw from 1..254 round-robin so an id is never
     * reused while a close for it could still be in flight. 255 is reserved
     * as the "active pane" sentinel in the protocol. */
    uint8_t id;
    bool in_use;
    uint16_t x, y;        /* top-left cell in the composite; 0,0 until layout lands */
    uint16_t cols, rows;
    pty_child child;      /* pid == -1 once reaped */
    int exit_status;      /* valid when pid == -1 */
    vt *vt;               /* screen state; snapshot source on reattach */
    scrollback *sb;       /* ring + disk persistence */
} pane;

typedef struct session {
    char name[SESSION_NAME_MAX + 1];
    /* Composite geometry — what an attached client's terminal shows. Renamed
     * from cols/rows when panes were lifted out so that every reader had to
     * decide which geometry it meant; with one pane they coincide with
     * pane 0's, and session_resize keeps them so. */
    uint16_t view_cols, view_rows;
    struct client *clients[MAX_CLIENTS_PER_SESSION];
    pane panes[MAX_PANES_PER_SESSION];
    bool in_use;
} session;

session *session_new(const char *name, char *const argv[], uint16_t cols, uint16_t rows);

/* Rebuild a session around a PTY master and child inherited across the daemon's
 * own re-exec, replaying `blob` (a vt_snapshot) to restore the screen. Does not
 * spawn anything. Returns NULL with errno set. */
session *session_import(const char *name, int master_fd, pid_t pid, uint16_t cols,
                        uint16_t rows, const uint8_t *blob, size_t blob_len);

session *session_find(const char *name);
void session_kill(session *s);            /* SIGHUP children, free slot */
void session_reap_children(void);         /* SIGCHLD bottom half */
void session_flush_all(void);             /* periodic scrollback flush */
void session_flush_screens_all(void);     /* shutdown: preserve visible screens */
int  session_count(void);
session *session_at(int idx);             /* iterate 0..MAX_SESSIONS-1, may be NULL */

/* The pane keyboard input and snapshots route to. With splits not yet in the
 * protocol this is always the first in-use pane (id 0); the active-id state
 * arrives with the split messages. Never NULL for an in-use session. */
pane *session_active_pane(session *s);

void session_attach(session *s, struct client *c, uint16_t cols, uint16_t rows);
void session_detach(session *s, struct client *c);
void session_stdin(session *s, const uint8_t *data, uint32_t len);
void session_resize(session *s, uint16_t cols, uint16_t rows);

#endif
