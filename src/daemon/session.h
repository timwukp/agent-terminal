#ifndef AT_SESSION_H
#define AT_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "layout.h"
#include "pty.h"
#include "common/scrollback.h"
#include "vt/vt.h"

#define SESSION_NAME_MAX 63
#define MAX_SESSIONS 64
#define MAX_CLIENTS_PER_SESSION 8
#define MAX_PANES_PER_SESSION LAYOUT_MAX_LEAVES

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
    layout lt;
    uint8_t active_id;   /* wire id of the pane holding the keyboard */
    uint8_t last_id;     /* previously active, for select "last" */
    uint8_t next_id;     /* round-robin cursor for ids 1..254 */
    /* Set when any pane's rectangle changed (split/close/resize): the next
     * composite must be a full repaint plus a MSG_LAYOUT broadcast. */
    bool layout_dirty;
    uint64_t last_frame_ms; /* leaky bucket for read-path compositing */
    bool in_use;
} session;

session *session_new(const char *name, char *const argv[], uint16_t cols, uint16_t rows);

/* Rebuild a session around a PTY master and child inherited across the daemon's
 * own re-exec, replaying `blob` (a vt_snapshot) to restore the screen. Does not
 * spawn anything. Returns NULL with errno set. */
session *session_import(const char *name, int master_fd, pid_t pid, uint16_t cols,
                        uint16_t rows, const uint8_t *blob, size_t blob_len);

/* Multi-pane import (handoff v2). _begin claims the slot and restores view
 * geometry + pane bookkeeping; _pane rebuilds one pane around an inherited
 * master into a specific slot (so layout pane_idx references hold);
 * _finish reflows and validates. A session that ends _finish with zero live
 * panes is freed and _finish returns false. */
session *session_import_begin(const char *name, uint16_t view_cols,
                              uint16_t view_rows, uint8_t active_id,
                              uint8_t last_id, uint8_t next_id,
                              const layout *lt);
pane *session_import_pane(session *s, uint8_t slot, uint8_t id, int master_fd,
                          pid_t pid, uint16_t cols, uint16_t rows,
                          const uint8_t *blob, size_t blob_len);
bool session_import_finish(session *s);

session *session_find(const char *name);
void session_kill(session *s);            /* SIGHUP children, free slot */
void session_reap_children(void);         /* SIGCHLD bottom half */
void session_flush_all(void);             /* periodic scrollback flush */
void session_flush_screens_all(void);     /* shutdown: preserve visible screens */
int  session_count(void);
session *session_at(int idx);             /* iterate 0..MAX_SESSIONS-1, may be NULL */

/* The pane keyboard input and snapshots route to (s->active_id). Never NULL
 * for an in-use session. */
pane *session_active_pane(session *s);

/* Pane lookup by wire id; 255 (and, from the ATTACH byte, 0) mean "active".
 * NULL if no such pane. */
pane *session_pane_by_id(session *s, uint8_t id);

/* Split the pane with wire id `target` (255 = active); the new pane runs
 * $SHELL and becomes active. Returns the new pane or NULL with errno:
 * EINVAL (no such pane / minimums violated), ENOSPC (no free slot). */
pane *session_split(session *s, uint8_t target, bool stacked);

/* Close the pane with wire id `id` (255 = active): SIGHUP its child and free
 * it; the sibling absorbs the space. Closing the last pane kills the session.
 * False if no such pane. */
bool session_close_pane(session *s, uint8_t id);

/* mode: 0 = by id, 1 = next, 2 = prev, 3 = last. */
bool session_select_pane(session *s, uint8_t mode, uint8_t id);

/* Composite every session that needs a frame (called from the daemon tick
 * and, rate-limited, from the PTY read path). */
void session_composite_all(void);

void session_attach(session *s, struct client *c, uint16_t cols, uint16_t rows);
void session_detach(session *s, struct client *c);
void session_stdin(session *s, const uint8_t *data, uint32_t len);
void session_resize(session *s, uint16_t cols, uint16_t rows);

#endif
