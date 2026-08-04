#ifndef AT_TTY_H
#define AT_TTY_H

#include <stdint.h>

/* Enter raw mode on the controlling tty. Registers atexit + signal cleanup
 * that restores termios AND emits mode-reset sequences (leave alt screen,
 * disable mouse/bracketed-paste, SGR reset) so a crash never wedges the
 * hosting terminal. */
int tty_raw_enter(void);
void tty_raw_leave(void);

int tty_get_size(uint16_t *cols, uint16_t *rows);

#endif
