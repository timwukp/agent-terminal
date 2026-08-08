#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "pager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/scrollback.h"
#include "common/xutil.h"

/* Hard cap on retained lines. The disk log is capped at 2x32 MiB, so a full
 * log is bounded but can still be ~600k short lines; keeping every one costs
 * pointer + length + seq per line plus the text. This bounds copy-mode memory
 * the way the daemon's ring bounds the daemon's. Oldest lines are dropped, and
 * pager_dropped() reports how many so the status line can say so rather than
 * silently presenting a truncated history as complete. */
#define PAGER_MAX_LINES 200000

/* Search pattern cap: the prompt is one row, so anything longer than a narrow
 * terminal's width cannot be displayed anyway. */
#define PAGER_PAT_MAX 128

typedef struct {
    char *text;
    uint32_t len;
    uint64_t seq;
} pline;

struct pager {
    pline *lines;
    uint32_t count, cap;
    uint64_t dropped;      /* lines evicted by PAGER_MAX_LINES */
    uint64_t highest_seq;  /* highest seq accepted; +1 is what we still want */
    bool have_any;

    uint32_t top;          /* index of the line drawn on row 0 */
    /* The line the user is "on", tracked separately from top because top is
     * clamped to the last page: a hit inside that last page cannot become the
     * first drawn row, so deriving the next search start from top made `n`
     * re-find the same line forever. */
    uint32_t cur;
    uint16_t cols, rows;
    uint64_t sb_lines;     /* daemon's total at entry: the tail we may lack */
    uint64_t want_from;    /* next ring seq to request, UINT64_MAX when done */

    bool active;
    int out_fd;

    /* input decoding */
    bool esc_pending;      /* saw ESC, waiting for CSI/SS3 or a timeout */
    bool csi_pending;      /* inside ESC [ ... final */
    char csi[16];
    uint8_t csi_len;

    /* search */
    bool searching;        /* typing a pattern at the prompt */
    char pat[PAGER_PAT_MAX + 1];
    uint8_t pat_len;
    bool have_pat;
    char msg[96];          /* transient status, e.g. "pattern not found" */
};

/* ---- output helpers ---- */

static void wr(pager *pg, const char *s, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(pg->out_fd, s + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return; /* terminal gone; the attach loop will notice */
        }
        off += (size_t)w;
    }
}

static void wrs(pager *pg, const char *s) { wr(pg, s, strlen(s)); }

/* ---- construction ---- */

pager *pager_new(void) {
    pager *pg = xcalloc(1, sizeof *pg);
    pg->out_fd = 1;
    pg->cols = 80;
    pg->rows = 24;
    pg->want_from = UINT64_MAX;
    return pg;
}

void pager_free(pager *pg) {
    if (!pg) return;
    for (uint32_t i = 0; i < pg->count; i++) free(pg->lines[i].text);
    free(pg->lines);
    free(pg);
}

void pager_set_out_fd(pager *pg, int fd) { pg->out_fd = fd; }

uint32_t pager_line_count(const pager *pg) { return pg->count; }
uint32_t pager_top(const pager *pg) { return pg->top; }
uint32_t pager_cur(const pager *pg) { return pg->cur; }
uint64_t pager_dropped(const pager *pg) { return pg->dropped; }
bool pager_active(const pager *pg) { return pg->active; }
bool pager_esc_pending(const pager *pg) { return pg->esc_pending; }

/* ---- line accumulation ---- */

/* Defined with the drawing code below; needed here for tail-following. */
static uint32_t max_top(const pager *pg);

void pager_add_line(pager *pg, uint64_t seq, const char *text, uint32_t len) {
    /* The disk log and the daemon's ring overlap: everything in the ring that
     * has been flushed is also on disk. Dropping non-increasing seqs is what
     * makes reading both sources safe, and it also makes a duplicate-delivering
     * daemon harmless. */
    if (pg->have_any && seq <= pg->highest_seq) return;
    pg->have_any = true;
    pg->highest_seq = seq;

    /* Follow the tail while the view is against the bottom. The ring reply
     * arrives *after* pager_enter has already anchored to the bottom of the disk
     * lines, so without this the newest history — the second the daemon has not
     * flushed yet, and the most likely thing the user is looking for — lands
     * below the view and is counted in the status line but never drawn. A user
     * who has scrolled up keeps their position. */
    bool follow = pg->top == max_top(pg);

    if (pg->count == pg->cap) {
        uint32_t ncap = pg->cap ? pg->cap * 2 : 1024;
        if (ncap > PAGER_MAX_LINES) ncap = PAGER_MAX_LINES;
        if (ncap == pg->cap) {
            /* At the cap: evict the oldest line to make room. */
            free(pg->lines[0].text);
            memmove(pg->lines, pg->lines + 1, (pg->count - 1) * sizeof *pg->lines);
            pg->count--;
            pg->dropped++;
            if (pg->top > 0) pg->top--;
        } else {
            pg->lines = xrealloc(pg->lines, ncap * sizeof *pg->lines);
            pg->cap = ncap;
        }
    }
    pline *l = &pg->lines[pg->count++];
    l->text = xmalloc(len ? len : 1);
    if (len) memcpy(l->text, text, len);
    l->len = len;
    l->seq = seq;

    if (follow) {
        pg->top = max_top(pg);
        pg->cur = pg->count - 1;
    }
}

static void disk_line(void *ud, uint64_t seq, const char *text, uint32_t len) {
    pager_add_line(ud, seq, text, len);
}

int64_t pager_load_disk(pager *pg, const char *session_name) {
    return sb_read_log(session_name, disk_line, pg);
}

int64_t pager_load_disk_pane(pager *pg, const char *session_name, uint8_t pane_id) {
    return sb_read_log_pane(session_name, pane_id, disk_line, pg);
}

uint64_t pager_want_from(const pager *pg) { return pg->want_from; }

void pager_add_batch_done(pager *pg, uint32_t nlines_received) {
    /* sb_fetch clamps to 1000 lines per reply and the daemon truncates on
     * payload overflow, so one request is not guaranteed to be the whole tail.
     * Re-request from just past what we now hold; stop when a reply adds
     * nothing (already at the end, or the lines were evicted from the ring —
     * sb_fetch cannot distinguish those, so treat both as done). */
    if (nlines_received == 0 || !pg->have_any || pg->highest_seq + 1 >= pg->sb_lines)
        pg->want_from = UINT64_MAX;
    else
        pg->want_from = pg->highest_seq + 1;
}

/* ---- ANSI stripping (search + status) ---- */

size_t pager_strip_ansi(const char *in, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)in[i];
        if (b == 0x1b) {
            if (i + 1 >= len) break;
            unsigned char n = (unsigned char)in[i + 1];
            if (n == '[') {          /* CSI: params then a final 0x40-0x7e */
                i += 2;
                while (i < len && ((unsigned char)in[i] < 0x40 || (unsigned char)in[i] > 0x7e)) i++;
                continue;            /* loop's i++ steps past the final byte */
            }
            if (n == ']') {          /* OSC: terminated by BEL or ESC \ */
                i += 2;
                while (i < len && (unsigned char)in[i] != 0x07) {
                    if ((unsigned char)in[i] == 0x1b && i + 1 < len && in[i + 1] == '\\') { i++; break; }
                    i++;
                }
                continue;
            }
            i++;                     /* two-byte escape: drop both */
            continue;
        }
        if (b < 0x20 || b == 0x7f) continue;
        if (o + 1 < cap) out[o++] = (char)b;
    }
    if (cap) out[o < cap ? o : cap - 1] = '\0';
    return o;
}

/* ---- drawing ---- */

static uint32_t view_rows(const pager *pg) {
    return pg->rows > 1 ? (uint32_t)(pg->rows - 1) : 1; /* last row = status */
}

static uint32_t max_top(const pager *pg) {
    uint32_t vr = view_rows(pg);
    return pg->count > vr ? pg->count - vr : 0;
}

static void clamp_top(pager *pg) {
    uint32_t mt = max_top(pg);
    if (pg->top > mt) pg->top = mt;
}

static void goto_top(pager *pg) { pg->top = pg->cur = 0; }

static void goto_bottom(pager *pg) {
    pg->top = max_top(pg);
    /* Search from the very last line, not from the top of the last page, or
     * `G` then `N` would skip the hits visible on screen. */
    pg->cur = pg->count ? pg->count - 1 : 0;
}

void pager_resize(pager *pg, uint16_t cols, uint16_t rows) {
    if (!cols || !rows) return;
    pg->cols = cols;
    pg->rows = rows;
    clamp_top(pg);
    if (pg->active) pager_draw(pg);
}

void pager_draw(pager *pg) {
    if (!pg->active) return;
    /* Home, then draw each row and erase to end of line. Erasing per row
     * rather than clearing the whole screen up front avoids a visible flash on
     * every keypress. */
    wrs(pg, "\x1b[H");
    uint32_t vr = view_rows(pg);
    for (uint32_t r = 0; r < vr; r++) {
        uint32_t idx = pg->top + r;
        wrs(pg, "\x1b[0m");
        if (idx < pg->count) wr(pg, pg->lines[idx].text, pg->lines[idx].len);
        wrs(pg, "\x1b[0m\x1b[K");
        if (r + 1 < vr) wrs(pg, "\r\n");
    }

    /* Status row, reverse video. */
    char st[512];
    int n;
    if (pg->searching) {
        n = snprintf(st, sizeof st, "/%.*s", (int)pg->pat_len, pg->pat);
    } else {
        uint32_t last = pg->count ? pg->top + (vr < pg->count - pg->top ? vr : pg->count - pg->top) : 0;
        int pct = pg->count ? (int)((uint64_t)last * 100 / pg->count) : 100;
        n = snprintf(st, sizeof st,
                     "scrollback %u-%u/%u (%d%%)%s%s%s  [j/k ^f/^b g/G / n q]",
                     pg->count ? pg->top + 1 : 0, last, pg->count, pct,
                     pg->dropped ? "  +" : "", pg->dropped ? "older dropped" : "",
                     pg->msg[0] ? pg->msg : "");
    }
    if (n < 0) n = 0;
    /* Clip to the terminal width: the status row must not wrap, and with
     * autowrap disabled a too-long line would be silently cut mid-escape. */
    if ((uint32_t)n > pg->cols) n = (int)pg->cols;
    char pos[32];
    snprintf(pos, sizeof pos, "\x1b[%u;1H", (unsigned)pg->rows);
    wrs(pg, pos);
    wrs(pg, "\x1b[0m\x1b[7m");
    wr(pg, st, (size_t)n);
    wrs(pg, "\x1b[0m\x1b[K");
    if (pg->searching) wrs(pg, "\x1b[?25h"); /* show the cursor at the prompt */
    else wrs(pg, "\x1b[?25l");
}

void pager_enter(pager *pg, uint16_t cols, uint16_t rows, uint64_t sb_lines) {
    pg->cols = cols ? cols : 80;
    pg->rows = rows ? rows : 24;
    pg->sb_lines = sb_lines;
    pg->active = true;
    pg->esc_pending = pg->csi_pending = pg->searching = false;
    pg->csi_len = 0;
    pg->msg[0] = '\0';

    /* Ask the daemon for anything newer than the disk log gave us. Lines are
     * only durable on the 1 s flush tick, so without this the most recent
     * second of history would be missing from copy-mode. */
    if (!pg->have_any) pg->want_from = 0;
    else if (pg->highest_seq + 1 < sb_lines) pg->want_from = pg->highest_seq + 1;
    else pg->want_from = UINT64_MAX;

    /* Start at the bottom: the newest lines are what a user reaching for
     * scrollback almost always wants first. */
    goto_bottom(pg);

    /* Alt screen + autowrap off. Autowrap is the load-bearing one: with it on,
     * a stored line longer than the terminal is wide would consume two display
     * rows and every row below would be off by one. The client cannot measure
     * display width (it does not link libvt), so it lets the terminal clip. */
    wrs(pg, "\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[0m\x1b[2J\x1b[H");
    pager_draw(pg);
}

void pager_leave(pager *pg) {
    if (!pg->active) return;
    pg->active = false;
    pg->searching = pg->esc_pending = pg->csi_pending = false;
    /* Restore autowrap before leaving the alt screen: the session's own state
     * is re-armed by the snapshot the caller triggers, but a client that dies
     * between here and there must not leave the terminal unwrappable. */
    wrs(pg, "\x1b[?7h\x1b[0m\x1b[?25h\x1b[?1049l");
}

/* ---- movement ---- */

static void move_lines(pager *pg, long delta) {
    long t = (long)pg->top + delta;
    if (t < 0) t = 0;
    uint32_t mt = max_top(pg);
    if (t > (long)mt) t = (long)mt;
    pg->top = (uint32_t)t;
    pg->cur = pg->top;
}

/* ---- search ---- */

static bool line_matches(const pager *pg, uint32_t idx) {
    char plain[SB_LINE_MAX + 1];
    pager_strip_ansi(pg->lines[idx].text, pg->lines[idx].len, plain, sizeof plain);
    return strstr(plain, pg->pat) != NULL;
}

/* Search forward (dir=1) or backward (dir=-1) and scroll the hit into view.
 * Returns false if nothing matched; there is no wraparound, so the position is
 * left alone rather than silently jumping to the other end.
 *
 * from_here includes the current line: a fresh `/pat` must be able to match the
 * line already under the cursor, while `n` must move off it. */
static bool search_step_from(pager *pg, int dir, bool from_here) {
    if (!pg->have_pat || pg->count == 0) return false;
    long i = (long)pg->cur + (from_here ? 0 : dir);
    while (i >= 0 && i < (long)pg->count) {
        if (line_matches(pg, (uint32_t)i)) {
            pg->cur = (uint32_t)i;
            pg->top = pg->cur;
            clamp_top(pg); /* a hit in the last page cannot be the first row */
            return true;
        }
        i += dir;
    }
    return false;
}

static bool search_step(pager *pg, int dir) { return search_step_from(pg, dir, false); }

static void set_msg(pager *pg, const char *m) {
    snprintf(pg->msg, sizeof pg->msg, "  %s", m);
}

/* ---- input ---- */

static pager_action key_normal(pager *pg, uint8_t b, bool *redraw) {
    switch (b) {
    case 'q': return PAGER_EXIT;
    case 'j': move_lines(pg, 1); break;
    case 'k': move_lines(pg, -1); break;
    case ' ':
    case 0x06: move_lines(pg, (long)view_rows(pg)); break;   /* Ctrl-f */
    case 0x02: move_lines(pg, -(long)view_rows(pg)); break;  /* Ctrl-b */
    case 0x04: move_lines(pg, (long)view_rows(pg) / 2); break;  /* Ctrl-d */
    case 0x15: move_lines(pg, -(long)view_rows(pg) / 2); break; /* Ctrl-u */
    case 'g': goto_top(pg); break;
    case 'G': goto_bottom(pg); break;
    case '/':
        pg->searching = true;
        pg->pat_len = 0;
        pg->pat[0] = '\0';
        pg->msg[0] = '\0';
        break;
    case 'n':
        if (!pg->have_pat) set_msg(pg, "no pattern");
        else if (!search_step(pg, 1)) set_msg(pg, "pattern not found");
        else pg->msg[0] = '\0';
        break;
    case 'N':
        if (!pg->have_pat) set_msg(pg, "no pattern");
        else if (!search_step(pg, -1)) set_msg(pg, "pattern not found");
        else pg->msg[0] = '\0';
        break;
    default:
        *redraw = false;
        return PAGER_CONTINUE;
    }
    return PAGER_CONTINUE;
}

/* ESC [ <params> <final>, plus ESC O <final> for SS3 (application-mode
 * arrows, which some terminals send instead of CSI). */
static void key_csi(pager *pg, char final) {
    switch (final) {
    case 'A': move_lines(pg, -1); break;                    /* up */
    case 'B': move_lines(pg, 1); break;                     /* down */
    case 'H': goto_top(pg); break;                          /* home */
    case 'F': goto_bottom(pg); break;                       /* end */
    case '~':
        /* ESC [ 5 ~ = PgUp, 6 ~ = PgDn, 1/7 ~ = Home, 4/8 ~ = End */
        if (pg->csi_len && pg->csi[0] == '5') move_lines(pg, -(long)view_rows(pg));
        else if (pg->csi_len && pg->csi[0] == '6') move_lines(pg, (long)view_rows(pg));
        else if (pg->csi_len && (pg->csi[0] == '1' || pg->csi[0] == '7')) goto_top(pg);
        else if (pg->csi_len && (pg->csi[0] == '4' || pg->csi[0] == '8')) goto_bottom(pg);
        break;
    default: break;
    }
}

pager_action pager_input(pager *pg, const uint8_t *in, size_t len) {
    bool redraw = false;
    pager_action act = PAGER_CONTINUE;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = in[i];

        if (pg->csi_pending) {
            if (b >= 0x40 && b <= 0x7e) {
                pg->csi_pending = false;
                key_csi(pg, (char)b);
                redraw = true;
            } else if (pg->csi_len < sizeof pg->csi) {
                pg->csi[pg->csi_len++] = (char)b;
            }
            continue;
        }
        if (pg->esc_pending) {
            pg->esc_pending = false;
            if (b == '[' || b == 'O') {
                pg->csi_pending = true;
                pg->csi_len = 0;
                continue;
            }
            /* ESC followed by anything else: treat the ESC as "cancel" and let
             * the byte be handled normally below. */
            if (pg->searching) { pg->searching = false; redraw = true; continue; }
            act = PAGER_EXIT;
            break;
        }
        if (b == 0x1b) {
            /* Cannot decide yet: a lone ESC quits, but ESC also introduces
             * arrows. Defer to the next byte or PAGER_ESC_TIMEOUT_MS. */
            pg->esc_pending = true;
            continue;
        }

        if (pg->searching) {
            if (b == '\r' || b == '\n') {
                pg->searching = false;
                pg->have_pat = pg->pat_len > 0;
                if (pg->have_pat && !search_step_from(pg, 1, true))
                    set_msg(pg, "pattern not found");
                else pg->msg[0] = '\0';
                redraw = true;
            } else if (b == 0x7f || b == 0x08) {
                if (pg->pat_len) pg->pat[--pg->pat_len] = '\0';
                else pg->searching = false;
                redraw = true;
            } else if (b == 0x07 || b == 0x03) { /* Ctrl-g / Ctrl-c cancel */
                pg->searching = false;
                redraw = true;
            } else if (b >= 0x20 && b < 0x7f && pg->pat_len < PAGER_PAT_MAX) {
                pg->pat[pg->pat_len++] = (char)b;
                pg->pat[pg->pat_len] = '\0';
                redraw = true;
            }
            continue;
        }

        bool this_redraw = true;
        act = key_normal(pg, b, &this_redraw);
        if (this_redraw) redraw = true;
        if (act == PAGER_EXIT) break;
    }

    if (act == PAGER_EXIT) return act;
    if (redraw) pager_draw(pg);
    return PAGER_CONTINUE;
}

pager_action pager_esc_timeout(pager *pg) {
    if (!pg->esc_pending) return PAGER_CONTINUE;
    pg->esc_pending = false;
    if (pg->searching) { /* ESC at the prompt cancels the search, not the pager */
        pg->searching = false;
        pager_draw(pg);
        return PAGER_CONTINUE;
    }
    return PAGER_EXIT;
}
