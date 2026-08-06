/* pager.h — scrollback copy-mode for the client.
 *
 * The client has no screen model (that is the whole architecture: the daemon
 * owns the VT grid), so the pager never tries to interpret the session's
 * screen. It renders *scrollback lines*, which the daemon already stores as
 * self-contained ANSI text, straight to the terminal's alternate screen with
 * autowrap disabled — so one stored line occupies exactly one display row and
 * no character-width logic is needed. The client does not link libvt and has
 * no wcwidth of its own, which is why that matters.
 *
 * Line sources, and why there are two:
 *   - the on-disk log, read directly with sb_read_log() — holds all history
 *     including the rotated-out generation, streamed in seq order;
 *   - the daemon's in-memory ring via MSG_SCROLLBACK_REQ — holds the tail that
 *     has not reached disk yet (writes coalesce in a 64 KiB buffer drained on
 *     the daemon's 1 s tick).
 * They overlap, so pager_add_line() drops any seq it has already seen.
 *
 * The view is frozen: lines the child produces while copy-mode is open are not
 * appended. Exiting repaints from a fresh daemon snapshot.
 */
#ifndef AT_PAGER_H
#define AT_PAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pager pager;

pager *pager_new(void);
void pager_free(pager *pg);

/* Redirect drawing (tests). Default is fd 1. */
void pager_set_out_fd(pager *pg, int fd);

/* Load the whole on-disk log for a session. Returns lines loaded, or -1 if
 * there is no log at all. Safe with no daemon running. */
int64_t pager_load_disk(pager *pg, const char *session_name);

/* Append one line. Ignored if seq is not newer than everything loaded, which
 * is what makes the disk/ring overlap harmless. Text is ANSI, no trailing
 * newline — exactly as stored. */
void pager_add_line(pager *pg, uint64_t seq, const char *text, uint32_t len);

/* Next seq the pager wants from the daemon's ring, or UINT64_MAX when it has
 * everything it asked for. Cleared by pager_add_batch_done(). */
uint64_t pager_want_from(const pager *pg);
void pager_add_batch_done(pager *pg, uint32_t nlines_received);

/* Enter/leave copy-mode. pager_enter draws immediately; pager_leave restores
 * the terminal but does NOT repaint the session — the caller must re-attach
 * for that, because only the daemon can produce a correct snapshot. */
void pager_enter(pager *pg, uint16_t cols, uint16_t rows, uint64_t sb_lines);
void pager_leave(pager *pg);
bool pager_active(const pager *pg);

void pager_resize(pager *pg, uint16_t cols, uint16_t rows);
void pager_draw(pager *pg);

typedef enum { PAGER_CONTINUE, PAGER_EXIT } pager_action;

/* Feed raw tty bytes. Redraws as needed. */
pager_action pager_input(pager *pg, const uint8_t *in, size_t len);

/* A lone ESC quits, but ESC also prefixes arrow/page keys, so the decision
 * waits for either the next byte or this timeout — the same shape as the
 * detach chord in attach.c. */
#define PAGER_ESC_TIMEOUT_MS 100
bool pager_esc_pending(const pager *pg);
pager_action pager_esc_timeout(pager *pg);

/* Strip CSI/OSC/ESC sequences, leaving displayed text. Exposed for tests and
 * used for searching, which must match what the user sees rather than the SGR
 * bytes around it. Returns bytes written; always NUL-terminates when cap > 0. */
size_t pager_strip_ansi(const char *in, size_t len, char *out, size_t cap);

/* Test accessors. pager_cur is the line the user is on, which differs from
 * pager_top once the view is against the bottom: top is clamped to the last
 * page, so a hit there is visible without being the first drawn row. */
uint32_t pager_line_count(const pager *pg);
uint32_t pager_top(const pager *pg);
uint32_t pager_cur(const pager *pg);
uint64_t pager_dropped(const pager *pg);

#endif
