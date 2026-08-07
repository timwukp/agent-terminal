#include "loop.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "common/xutil.h"

/* Sized for panes: 64 sessions × 6 panes + 32 clients + listener + signal
 * pipe = 418 fds, where the old 256 was silently too small — loop_add_fd
 * returns -1 past the cap and three call sites used to ignore it, leaving a
 * child whose output nobody would ever read. 1024 static pollfd+slot pairs
 * cost 20 KB, so round up rather than size exactly. */
#define LOOP_MAX_FDS 1024

typedef struct {
    loop_cb cb;
    void *ud;
} loop_slot;

static struct pollfd g_pfds[LOOP_MAX_FDS];
static loop_slot g_slots[LOOP_MAX_FDS];
static int g_nfds = 0;
static bool g_running = false;

static int find_idx(int fd) {
    for (int i = 0; i < g_nfds; i++)
        if (g_pfds[i].fd == fd) return i;
    return -1;
}

int loop_add_fd(int fd, short events, loop_cb cb, void *ud) {
    if (g_nfds >= LOOP_MAX_FDS || find_idx(fd) >= 0) return -1;
    g_pfds[g_nfds] = (struct pollfd){.fd = fd, .events = events};
    g_slots[g_nfds] = (loop_slot){.cb = cb, .ud = ud};
    g_nfds++;
    return 0;
}

int loop_mod_fd(int fd, short events) {
    int i = find_idx(fd);
    if (i < 0) return -1;
    g_pfds[i].events = events;
    return 0;
}

void loop_del_fd(int fd) {
    int i = find_idx(fd);
    if (i < 0) return;
    /* Swap-remove; loop_run() restarts dispatch after any removal so a
     * moved entry is never skipped or double-fired within one poll round. */
    g_nfds--;
    g_pfds[i] = g_pfds[g_nfds];
    g_slots[i] = g_slots[g_nfds];
}

void loop_quit(void) { g_running = false; }

static unsigned g_tick_ms = 0;
static void (*g_tick_cb)(void) = 0;

void loop_set_tick(unsigned interval_ms, void (*cb)(void)) {
    g_tick_ms = interval_ms;
    g_tick_cb = cb;
}

int loop_run(void) {
    g_running = true;
    uint64_t last_tick = now_ms();
    while (g_running) {
        int timeout = g_tick_cb ? (int)g_tick_ms : -1;
        int n = poll(g_pfds, (nfds_t)g_nfds, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("poll: %s", strerror(errno));
        }
        if (g_tick_cb && now_ms() - last_tick >= g_tick_ms) {
            g_tick_cb();
            last_tick = now_ms();
        }
        if (n == 0) continue;
        int before = g_nfds;
        for (int i = 0; i < g_nfds && n > 0; i++) {
            if (!g_pfds[i].revents) continue;
            short re = g_pfds[i].revents;
            g_pfds[i].revents = 0;
            n--;
            g_slots[i].cb(g_pfds[i].fd, re, g_slots[i].ud);
            if (g_nfds != before) break; /* set mutated; re-poll */
        }
    }
    return 0;
}
