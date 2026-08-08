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

/* Bind and listen. If inherited_fd >= 0 it is an already-bound listener passed
 * across the daemon's own re-exec: adopt it instead of rebinding, so the socket
 * name is never unlinked and a client connecting during the handoff sees a
 * refused connection at worst, never a socket owned by nobody. */
int server_init(const char *socket_path, int inherited_fd);
void server_shutdown(void);

/* Disconnect every client and hand back the listen fd for the next image to
 * inherit. The socket path is deliberately NOT unlinked. */
int server_prepare_handoff(void);

#endif
