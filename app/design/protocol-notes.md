# Protocol notes for GUI clients

`src/common/proto.h` is the single source of truth. This file is a *reading*
of it from a GUI client's perspective — what to send, what to expect, and the
traps. Nothing here overrides proto.h; if they disagree, proto.h wins and
this file has a bug.

## Frame layout

```
offset  size  field
0       4     payload_len   u32 LE (excludes this 5-byte header)
4       1     type
5       N     payload       N == payload_len, max 1 MiB (PROTO_MAX_PAYLOAD)
```

A peer sending a frame with `payload_len > 1 MiB` is violating the protocol;
the correct response is to disconnect (that is what the C client does —
`proto_read_frame` returns -1). Unknown frame *types* are skipped; unknown
*trailing payload bytes* are ignored. Payload evolution is additive-only.

## Connection lifecycle

1. Connect to the unix socket:
   `$XDG_RUNTIME_DIR/agent-terminal/default.sock` if `XDG_RUNTIME_DIR` is
   set, else `~/.agent-terminal/run/default.sock`. The parent directory is
   0700 and the daemon enforces peer-UID; a GUI client needs no special
   handling beyond connecting as the same user.
2. Send `MSG_HELLO` (0x01): `u16 ver=1, u16 flags`. Set `CLIENT_CAP_PANES`
   (0x0001) on the attach connection; set `CLIENT_CAP_SESSION_EVENTS`
   (0x0002) on whichever connection watches the session table. A control
   connection that only makes one round trip may send 0.
   Exact bytes for the panes-capable hello: `04 00 00 00 01 01 00 01 00`;
   for both capabilities: `04 00 00 00 01 01 00 03 00`.
   Capabilities are separate bits, not a level: a client written before
   0x39 existed set 0x0001 and must never be sent 0x39 on the strength of
   it.
3. Read `MSG_HELLO_OK` (0x02): `u16 ver` always; then **tail-optional**
   additive appends — `u32 daemon_pid` @2, `u32 generation` @6,
   `u16 server_flags` @10. Parse each field only if the payload is long
   enough; an old daemon sends a short payload and that is not an error.
   `generation` counts in-place reloads (the pid deliberately does not
   change across one) — a GUI that caches per-daemon state should key it
   on (pid, generation), not pid.
4. Then either drive control messages or `MSG_ATTACH`.

## The two-connection model

GUI clients should hold **two** connections:

- **Control**: `MSG_LIST_SESSIONS2` (0x1a, empty) → `MSG_SESSION_LIST2`
  (0x37) for the sidebar — asked for when `MSG_SESSIONS_CHANGED` (0x39)
  says to, or polled when the daemon does not offer that; `MSG_NEW_SESSION` (0x12:
  `u16 cols, u16 rows, u8 nlen, name, u16 argv_bytes, argv` — argv is
  NUL-separated) and `MSG_KILL_SESSION` (0x13) for session management.
- **Attach**: `MSG_ATTACH` (0x14: `u16 cols, u16 rows, u8 pane_id, u8 nlen,
  name`). `pane_id` 0 means "don't change" (a pre-pane client has always
  written 0, so 0 can never mean "select pane 0"); 255 means active.

The protocol is natively multi-client: a CLI client and a GUI client attached
to the same session both receive every `MSG_OUTPUT`. Nothing special to do.

Session switching in the GUI: drop the attach connection, open a fresh one,
HELLO + ATTACH again. The `MSG_SNAPSHOT` repaint makes this cheap and avoids
a DETACH state machine.

## Rendering path

- `MSG_SNAPSHOT` (0x31): `u16 cols, u16 rows, u64 sb_lines`, then a
  **length-implicit ANSI blob** from offset 12 to end of payload. Feed the
  blob to the terminal widget verbatim. Because the blob is
  length-implicit, this payload is frozen — it can never grow a field
  (appended bytes would land on the user's screen). Pane metadata rides
  `MSG_LAYOUT` instead.
- `MSG_OUTPUT` (0x30): raw bytes. Feed verbatim. In a multi-pane session
  these are daemon-composited frames (dividers included), so a client with
  zero pane-rendering code still displays splits correctly.
- `MSG_PANE_BELL` (0x38): `u8 pane_id`. A BEL in a **split** session,
  attributed to the pane that rang. Never sent for a single pane (the raw
  `\x07` rides `MSG_OUTPUT` there) and only to `CLIENT_CAP_PANES` clients.
- `MSG_SESSIONS_CHANGED` (0x39): **empty payload** — "the session table
  changed, ask again". Answer it with `MSG_LIST_SESSIONS2`; do not build a
  parser for it, since there is nothing to parse and a future daemon
  appending bytes must not break a client. Sent only to
  `CLIENT_CAP_SESSION_EVENTS` clients, and unlike every other D→C message
  here it goes to **all** of them, not just those attached to the session
  that changed — a sidebar lists sessions it is not attached to. Changes
  are coalesced onto the daemon's 20 ms tick, so a burst of creates is one
  notification, and it is idempotent: two of them cost one extra list.
  Check `SERVER_CAP_SESSION_EVENTS` (0x0002) in HELLO_OK's `server_flags`
  before relying on it — an older daemon and an idle newer one both look
  like silence, so absence of the bit is the only way to tell them apart.
- `MSG_LAYOUT` (0x35): `u16 view_cols, u16 view_rows, u8 active_id,
  u8 npanes`, then per pane `u8 id, u16 x, u16 y, u16 cols, u16 rows`
  (cell coordinates). Sent only to clients that set `CLIENT_CAP_PANES`.
  This is what mouse hit-testing consumes: pixel → cell → containing pane
  rect → `MSG_SELECT_PANE` mode 0 with that id.
- `MSG_PANE_EXITED` (0x36), `MSG_SESSION_EXITED` (0x34), `MSG_ERR` (0x03:
  `u16 code, u16 msg_len, utf8 msg`) as in the C client: an ERR before
  attach succeeds is fatal, after that it is advisory (show, don't
  disconnect) except `ERR_VERSION`.

## Input path

- `MSG_STDIN_DATA` (0x20): raw bytes for the PTY of the **active** pane.
- `MSG_RESIZE` (0x21): `u16 cols, u16 rows` — the client's whole view.
- `MSG_SPLIT_PANE` (0x16): `u8 stacked, u8 target (255=active)`.
- `MSG_CLOSE_PANE` (0x17): `u8 pane_id (255=active)`.
- `MSG_SELECT_PANE` (0x18): `u8 mode, u8 pane_id`. Modes: 0 by-id, 1 next,
  2 prev, 3 last, 4-7 directional (up/down/right/left), 8 zoom toggle
  (modes 1-8 ignore pane_id). A GUI uses mode 0 for click-to-focus and
  mode 8 for a zoom button.
- `MSG_PING`/`MSG_PONG` (0x40/0x41): `u64 nonce`, either direction.

## SESSION_LIST2 entry format

`u16 count`, then per entry `u16 entry_len` followed by `entry_len` bytes:

```
u8 nlen, name, u16 view_cols, u16 view_rows, u8 alive, u8 nclients,
u32 pid, u32 exit_status, u8 npanes, u8 zoomed(0/1)
```

Always honor `entry_len` — skip to the next entry by length, never by the
sum of parsed fields. That prefix exists precisely so future daemons can
append per-entry fields without breaking older GUIs.

## Traps

1. **A raw BEL byte is only observable in single-pane sessions.** With one
   pane the daemon tees child output raw, so `\x07` reaches the client and
   xterm.js's `onBell` fires. With ≥2 panes, frames are composited from
   grid state and BEL never survives — the daemon sends `MSG_PANE_BELL`
   (0x38, `u8 pane_id`) instead, and only then, so the two paths are
   disjoint and a bell cannot ring twice.
2. **No pane-resize message exists.** Splits are 50/50, fixed
   (`src/daemon/layout.c`). Drag-to-resize is deferred daemon work.
3. **Tail-optional parsing is mandatory**, not defensive: a v1 daemon
   really does send HELLO_OK without pid/generation/server_flags.
4. **Do not parse composited OUTPUT frames.** Their exact byte layout
   (dirty-row repaints, no EL) is a daemon implementation detail pinned by
   a golden-byte test in the C repo — render them, never scrape them.
