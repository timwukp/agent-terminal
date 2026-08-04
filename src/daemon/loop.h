/* loop.h — single-threaded poll(2) event loop.
 *
 * poll over kqueue/epoll: the daemon watches < 50 fds, where poll's O(n)
 * scan is unmeasurable, and poll behaves identically on macOS and Linux.
 * Backends can be swapped later without touching call sites. */
#ifndef AT_LOOP_H
#define AT_LOOP_H

#include <poll.h>

typedef void (*loop_cb)(int fd, short revents, void *ud);

int  loop_add_fd(int fd, short events, loop_cb cb, void *ud);
int  loop_mod_fd(int fd, short events); /* change POLLIN/POLLOUT mask */
void loop_del_fd(int fd);
int  loop_run(void); /* blocks until loop_quit(); returns 0 */
void loop_quit(void);

/* Periodic tick: cb fires roughly every interval_ms while the loop runs
 * (piggybacked on the poll timeout; used for scrollback flushing). */
void loop_set_tick(unsigned interval_ms, void (*cb)(void));

#endif
