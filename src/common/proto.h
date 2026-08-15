/* proto.h — wire protocol between agent-terminal (client) and agent-terminald.
 *
 * Frame layout, all integers little-endian:
 *
 *   offset  size  field
 *   0       4     payload_len  (u32, excludes this 5-byte header)
 *   4       1     type
 *   5       N     payload      (N == payload_len, max PROTO_MAX_PAYLOAD)
 *
 * Rules:
 *  - HELLO must be the first frame on a connection; version is negotiated
 *    there and applies to the whole connection.
 *  - Unknown frame types are skipped by the receiver (framing makes that
 *    safe); unknown trailing payload bytes are ignored. Payload evolution
 *    within a version is additive-only.
 */
#ifndef AT_PROTO_H
#define AT_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ring.h"

#define PROTO_VERSION      1
#define PROTO_HDR_SIZE     5
#define PROTO_MAX_PAYLOAD  (1u << 20) /* 1 MiB; peer violating this is disconnected */

enum proto_type {
    MSG_HELLO          = 0x01, /* C→D: u16 ver, u16 flags */
    /* MSG_HELLO_OK's u32 generation is an additive append (a v1 client reads
     * only the type byte and ignores the tail). It counts in-place restarts,
     * and it exists because the pid deliberately does NOT change across one:
     * with a re-exec handoff, the pid is the wrong thing to compare and the
     * generation is the only observable that moves. */
    MSG_HELLO_OK       = 0x02, /* D→C: u16 ver, u32 daemon_pid, u32 generation */
    MSG_ERR            = 0x03, /* D→C: u16 code, u16 msg_len, utf8 msg */
    MSG_LIST_SESSIONS  = 0x10, /* C→D: empty */
    MSG_SESSION_LIST   = 0x11, /* D→C: u16 count, entries */
    MSG_NEW_SESSION    = 0x12, /* C→D: u16 cols, u16 rows, u8 nlen, name, u16 argv_bytes, argv */
    MSG_KILL_SESSION   = 0x13, /* C→D: u8 nlen, name */
    MSG_RELOAD         = 0x19, /* C→D: empty — re-exec the daemon in place */
    /* pane_id: 0 = "don't change" — the pre-pane client has always written 0
     * here (attach.c), so 0 cannot mean "select pane 0". 255 = active. */
    MSG_ATTACH         = 0x14, /* C→D: u16 cols, u16 rows, u8 pane_id, u8 nlen, name */
    MSG_DETACH         = 0x15, /* C→D: empty */
    MSG_SPLIT_PANE     = 0x16, /* C→D: u8 stacked, u8 target_pane_id (255 = active) */
    MSG_CLOSE_PANE     = 0x17, /* C→D: u8 pane_id (255 = active) */
    MSG_SELECT_PANE    = 0x18, /* C→D: u8 mode (0=by id, 1=next, 2=prev, 3=last,
                                * 4=up, 5=down, 6=right, 7=left — geometrically
                                * nearest pane in that direction — 8=zoom toggle),
                                * u8 pane_id (modes 1-8 ignore it) */
    MSG_STDIN_DATA     = 0x20, /* C→D: raw bytes for the PTY */
    MSG_RESIZE         = 0x21, /* C→D: u16 cols, u16 rows */
    MSG_OUTPUT         = 0x30, /* D→C: raw child output (live tee) / composite frames */
    /* MSG_SNAPSHOT ends in a LENGTH-IMPLICIT blob (the client writes payload
     * bytes 12..len to the terminal), so despite the additive-only rule above
     * it can never grow a field: appended bytes land inside the blob and get
     * written to the user's screen. Pane metadata rides MSG_LAYOUT instead. */
    MSG_SNAPSHOT       = 0x31, /* D→C: u16 cols, u16 rows, u64 sb_lines, ANSI repaint */
    /* u8 pane_id is a true append (the pre-pane payload is a fixed 12 bytes);
     * the daemon reads it only when len >= 13 and treats 255/absent as the
     * active pane. */
    MSG_SCROLLBACK_REQ = 0x32, /* C→D: u64 start_seq, u32 max_lines[, u8 pane_id] */
    MSG_SCROLLBACK_DATA= 0x33, /* D→C: u64 first_seq, u32 nlines, {u32 len, bytes}... */
    MSG_SESSION_EXITED = 0x34, /* D→C: i32 exit_status */
    /* MSG_SESSION_LIST (0x11) is positional with no per-entry length
     * (hardcoded per-entry parse on both sides), so per-pane data cannot be
     * appended to it either — the NEXT entry would mis-parse. Showing panes
     * in `ls` needs a new message type. */
    /* SESSION_LIST could not grow (positional, no per-entry length), so panes
     * get a v2 with a u16 LENGTH PREFIX per entry: unknown tail fields skip
     * cleanly and future appends stay additive. Entry payload:
     *   u8 nlen, name, u16 view_cols, u16 view_rows, u8 alive, u8 nclients,
     *   u32 pid, u32 exit_status, u8 npanes, u8 zoomed(0/1)
     * Requested only by capable clients; old daemons skip the request
     * (unknown type) and the client falls back to MSG_LIST_SESSIONS. */
    MSG_LIST_SESSIONS2 = 0x1a, /* C→D: empty */
    MSG_SESSION_LIST2  = 0x37, /* D→C: u16 count, then {u16 entry_len, entry}... */
    MSG_LAYOUT         = 0x35, /* D→C: u16 view_cols, u16 view_rows, u8 active_id,
                                * u8 npanes, then per pane:
                                * u8 id, u16 x, u16 y, u16 cols, u16 rows.
                                * Sent only to clients that set CLIENT_CAP_PANES. */
    MSG_PANE_EXITED    = 0x36, /* D→C: u8 pane_id, i32 exit_status (≥2 panes only) */
    /* Composite frames are rebuilt from grid state, so a BEL inside a split
     * session never survives to MSG_OUTPUT — with one pane the raw \a rides
     * the live tee and the hosting terminal rings natively, which is also why
     * this is NOT sent then (it would ring twice). ≥2 panes only, and only to
     * clients that set CLIENT_CAP_PANES. */
    MSG_PANE_BELL      = 0x38, /* D→C: u8 pane_id */
    MSG_PING           = 0x40, /* both: u64 nonce */
    MSG_PONG           = 0x41, /* both: u64 nonce */
};

/* MSG_HELLO u16 flags (always sent, 0 before panes existed). */
#define CLIENT_CAP_PANES 0x0001
/* MSG_HELLO_OK grows u16 server_flags (additive append at offset 10). */
#define SERVER_CAP_PANES 0x0001

enum proto_err {
    ERR_VERSION      = 1,
    ERR_NO_SESSION   = 2,
    ERR_NAME_TAKEN   = 3,
    ERR_BAD_REQUEST  = 4,
    ERR_INTERNAL     = 5,
};

/* --- little-endian scalar helpers (safe on any alignment) --- */
void     put_u16(uint8_t *p, uint16_t v);
void     put_u32(uint8_t *p, uint32_t v);
void     put_u64(uint8_t *p, uint64_t v);
uint16_t get_u16(const uint8_t *p);
uint32_t get_u32(const uint8_t *p);
uint64_t get_u64(const uint8_t *p);

/* Append one complete frame to out. Returns false only if the ring's
 * capacity ceiling would be exceeded (slow-consumer signal). */
bool proto_write_frame(ring *out, uint8_t type, const void *payload, size_t len);

/* Try to decode one frame from the front of in.
 * Returns:  1 frame decoded (type and len set; payload lands in the
 *             caller-supplied scratch buffer, valid until next call),
 *           0 need more bytes,
 *          -1 protocol violation (oversized frame) — caller must disconnect.
 * scratch must be at least PROTO_MAX_PAYLOAD bytes. */
int proto_read_frame(ring *in, uint8_t *type, uint8_t *scratch, size_t *len);

/* Decode a MSG_ERR payload (u16 code, u16 msg_len, utf8 msg) into a slice that
 * is safe to hand to "%.*s".
 *
 * msg_len is a PEER-SUPPLIED number and the frame it arrives in is not obliged
 * to be big enough to hold it: a hostile or simply broken daemon can declare
 * 65535 inside a 4096-byte frame. Passed to %.*s unbounded, the format scan
 * runs past the end of the buffer holding the payload — a stack over-read in
 * the client's HELLO path, where that buffer is 4096 bytes of stack. The check
 * belongs here rather than at each call site because it already existed at one
 * of four and was missing at the other three; one copy cannot drift.
 *
 * Returns true with *msg / *msg_len set to a complete message the frame really
 * carries. Returns false when there is nothing trustworthy to print — *code is
 * still set whenever the frame was long enough to carry one, so the caller can
 * report the number instead of staying silent. Any out param may be NULL. */
bool proto_err_text(const uint8_t *payload, size_t len, uint16_t *code,
                    const char **msg, uint16_t *msg_len);

#endif
