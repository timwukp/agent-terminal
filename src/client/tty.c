#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "tty.h"

#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;
static bool g_raw = false;

/* Leave alt screen, kill mouse modes + bracketed paste, reset SGR, show
 * cursor. Safe to emit even if none were active. */
static const char RESET_SEQ[] =
    "\x1b[?1049l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1005l\x1b[?1006l"
    "\x1b[?2004l\x1b[0m\x1b[?25h";

void tty_raw_leave(void) {
    if (!g_raw) return;
    g_raw = false;
    ssize_t r = write(1, RESET_SEQ, sizeof RESET_SEQ - 1);
    (void)r;
    tcsetattr(0, TCSANOW, &g_saved);
}

static void on_fatal_signal(int sig) {
    tty_raw_leave();
    signal(sig, SIG_DFL);
    raise(sig);
}

int tty_raw_enter(void) {
    if (g_raw) return 0;
    /* Non-tty stdin (pipes, test harnesses): nothing to configure. */
    if (!isatty(0)) return 0;
    if (tcgetattr(0, &g_saved) != 0) return -1;
    struct termios raw = g_saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) != 0) return -1;
    g_raw = true;

    atexit(tty_raw_leave);
    struct sigaction sa = {.sa_handler = on_fatal_signal};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    return 0;
}

int tty_get_size(uint16_t *cols, uint16_t *rows) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}
