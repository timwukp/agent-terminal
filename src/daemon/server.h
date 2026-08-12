#ifndef AT_SERVER_H
#define AT_SERVER_H

#include <stdbool.h>
#include <stdint.h>

struct session;

/* Called by session.c to push frames to an attached client. */
struct client;
void client_send(struct client *c, uint8_t type, const void *payload, uint32_t len);
void client_disconnect(struct client *c); /* slow-consumer / protocol errors */
/* False once the client is disconnected (client_send does that itself on
 * backlog). Lets a caller streaming multiple frames stop early instead of
 * serializing into a slot that was already torn down. */
bool client_alive(const struct client *c);
/* True if the client's HELLO set CLIENT_CAP_PANES: gates MSG_LAYOUT /
 * MSG_PANE_EXITED delivery. Composited output goes to everyone regardless. */
bool client_wants_panes(const struct client *c);
/* Terminal geometry the client last reported (HELLO's ATTACH/RESIZE). */
void client_geometry(const struct client *c, uint16_t *cols, uint16_t *rows);

/* Bind and listen. If inherited_fd >= 0 it is an already-bound listener passed
 * across the daemon's own re-exec: adopt it instead of rebinding, so the socket
 * name is never unlinked and a client connecting during the handoff sees a
 * refused connection at worst, never a socket owned by nobody. */
int server_init(const char *socket_path, int inherited_fd);
void server_shutdown(void);

/* Drop clients that connected but never sent MSG_HELLO. Must be driven by the
 * daemon tick: a silent peer produces no readable event, so no amount of care
 * in the read path can reclaim its slot. Without this, MAX_CLIENTS silent
 * connections deny service to every real client. */
void server_reap_idle(void);

/* Disconnect every client and hand back the listen fd for the next image to
 * inherit. The socket path is deliberately NOT unlinked. */
int server_prepare_handoff(void);

#endif
