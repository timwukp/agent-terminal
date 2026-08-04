#ifndef AT_SERVER_H
#define AT_SERVER_H

#include <stdint.h>

struct session;

/* Called by session.c to push frames to an attached client. */
struct client;
void client_send(struct client *c, uint8_t type, const void *payload, uint32_t len);
void client_disconnect(struct client *c); /* slow-consumer / protocol errors */

int server_init(const char *socket_path);
void server_shutdown(void);

#endif
