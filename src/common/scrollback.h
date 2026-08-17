/* scrollback.h — per-session scrollback: in-memory ring of recent lines +
 * append-only disk log that survives daemon crashes.
 *
 * Disk record format (little-endian):
 *   [u32 rec_len][u32 crc32][u64 line_seq][rec_len - 12 bytes: line as
 *    UTF-8 text with embedded SGR sequences]
 * rec_len counts crc32+line_seq+payload (not itself). Lines are stored as
 * rendered ANSI text, so the file stays human-salvageable with `less -R`
 * and format-stable across engine changes.
 *
 * Recovery: on open, scan and CRC-validate; truncate at the first bad
 * record (torn final write after a crash).
 *
 * Rotation: two files, scrollback.log + scrollback.log.1, each capped at
 * SB_FILE_MAX; rename-and-reopen keeps rotation O(1). */
#ifndef AT_SCROLLBACK_H
#define AT_SCROLLBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vt/vt.h"

#define SB_MEM_LINES_DEFAULT 10000
#define SB_FILE_MAX_DEFAULT (32u << 20) /* 32 MiB per file */
#define SB_LINE_MAX 8192               /* serialized line cap */

/* Records between sparse-index entries. A straight memory/sweep trade at the
 * 346,100-lines-per-pane worst case (2 generations x SB_FILE_MAX / 189.9 B per
 * record, measured across 28 real logs): 256 -> 10.6 KB per pane and a <=47 KB
 * sweep, 512 -> 5.3 KB and <=95 KB, 1024 -> 2.6 KB and <=190 KB. 512 keeps the
 * whole worst-case fleet (MAX_SESSIONS x LAYOUT_MAX_LEAVES = 384 panes) under
 * 2.1 MB while keeping the sweep smaller than the page it serves. */
#define SB_INDEX_STEP 512u

typedef struct scrollback scrollback;

/* Open (creating dir/file as needed) the scrollback for a session name.
 * Resumes line_seq from what's already on disk. Returns NULL on error. */
scrollback *sb_open(const char *session_name, uint32_t mem_lines, uint32_t file_max);

/* Per-pane variant. Pane 0 is byte-identical to sb_open — same
 * scrollback.log, so every existing log and every tool that reads one keeps
 * working. Other panes get pane<id>.log in the same session directory: the
 * pane lands in the *filename* rather than a path component because session
 * names are validated to be exactly one component, and a uint8_t rendered
 * with %u cannot smuggle a separator. */
scrollback *sb_open_pane(const char *session_name, uint8_t pane_id,
                         uint32_t mem_lines, uint32_t file_max);
void sb_close(scrollback *sb); /* flush + close */

/* Append one line of cells (serialized to ANSI text internally). */
void sb_push_line(scrollback *sb, const vt_cell *cells, uint16_t n);

/* Total lines ever pushed (== next line_seq). */
uint64_t sb_total_lines(const scrollback *sb);

/* Fetch up to max_lines starting at start_seq from the in-memory ring.
 * Returns the number filled into out[] (borrowed pointers, valid until the
 * next sb_push_line). Lines older than the ring cannot be fetched here —
 * use sb_fetch_deep, which falls back to the disk log. */
typedef struct { const char *text; uint32_t len; uint64_t seq; } sb_line_ref;
uint32_t sb_fetch(const scrollback *sb, uint64_t start_seq, uint32_t max_lines,
                  sb_line_ref *out, uint32_t out_cap);

/* Same contract as sb_fetch, but a start_seq below the ring's oldest line is
 * served from the disk log instead of returning nothing. Disk-served text is
 * copied into `buf` and out[].text points into it, so it stays valid until the
 * caller reuses buf (ring-served text is still borrowed from the ring). A
 * short buf serves fewer lines; it is never overrun.
 *
 * The seek is O(log n) + a bounded sweep because each generation carries a
 * sparse index of every SB_INDEX_STEPth record's offset, built during the scan
 * sb_open already performs. Without it, serving one page means reading both
 * generations entire — measured at 181 MB per 10,000-line backfill on a real
 * 18.1 MB log, i.e. 200-450 ms of a SINGLE-threaded event loop per request. */
uint32_t sb_fetch_deep(scrollback *sb, uint64_t start_seq, uint32_t max_lines,
                       sb_line_ref *out, uint32_t out_cap,
                       char *buf, size_t bufsz);

/* Work done by disk-served fetches so far: bytes pread, and records skipped
 * before the first served line.
 *
 * These exist for the tests, and they are not optional decoration. A broken
 * index is INVISIBLE to any assertion about content: a wrong offset lands off a
 * record boundary, rec_len+CRC rejects it, and the fallback sweeps the
 * generation from 0 — which returns exactly the right lines after reading the
 * whole file. Every content check passes while the mechanism above is dead and
 * the daemon is back to blocking its loop for hundreds of milliseconds. So one
 * test asserts these counters as numbers. */
void sb_fetch_stats(const scrollback *sb, uint64_t *bytes_read, uint64_t *recs_swept);

/* Flush buffered writes to disk (called on a timer + on detach). */
void sb_flush(scrollback *sb);

/* Read an entire scrollback log (both rotation generations, in order) for
 * a possibly-dead session, streaming validated payloads to cb. Standalone:
 * does not require an open scrollback. Returns lines read, -1 if no log. */
typedef void (*sb_read_cb)(void *ud, uint64_t seq, const char *text, uint32_t len);
int64_t sb_read_log(const char *session_name, sb_read_cb cb, void *ud);
/* Pane variant; pane 0 reads the same files sb_read_log reads. */
int64_t sb_read_log_pane(const char *session_name, uint8_t pane_id,
                         sb_read_cb cb, void *ud);

/* List session names that have scrollback logs on disk. Fills names as a
 * NUL-separated buffer; returns count. */
int sb_list_logs(char *buf, size_t bufsz);

#endif
