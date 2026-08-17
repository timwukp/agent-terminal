# Deferred daemon work (C PRs the GUI will want)

Each item is a standalone, additive C PR (proto.h + proto.c + daemon +
unit/integration tests + docs), following the protocol's evolution rules:
tail-optional appends, per-entry length prefixes, capability bits in
HELLO/HELLO_OK flags. GUI features gate on `server_flags` bits so older
daemons keep working with the feature grayed out.

## 1. Pane resize — `MSG_RESIZE_PANE`

Today `src/daemon/layout.c` halves the parent rect on every split; there
is no ratio state and no resize message, so drag-to-resize cannot exist
client-side.

- New C→D message: `u8 pane_id, u16 ratio_permille` (position of the
  split between this pane and its tree sibling, 100‰–900‰ clamped, panes
  never below PANE_MIN_COLS/ROWS).
- `layout.c` stores a ratio per internal node (default 500‰) and the
  reflow path uses it; handoff v2 serialization grows the ratio per node
  (additive — old records imply 500‰).
- Advertise `SERVER_FLAG_PANE_RESIZE` in HELLO_OK server_flags.
- Tests: layout unit tests for ratio math + min-size clamping;
  integration script driving resize over the wire; golden-byte compat
  test unchanged (single-pane path untouched).

This is the largest item: layout, reflow, handoff, and tests all move.

## 2. Bell signal — `MSG_PANE_BELL` — **DONE**

Landed as proto.h 0x38, exactly the shape sketched here: `u8 pane_id`,
emitted from the daemon's bell callback when the session is composited
(single-pane keeps raw passthrough — no double signal), sent only to
CLIENT_CAP_PANES clients. Wire behaviour is pinned by
`tests/integration/test_pane_bell.sh` (attribution, capability gate,
single-pane silence, and the OSC-terminator non-ring); the GUI treats it
exactly like a local bell (Terminal.tsx `pane_bell` ctrl event).

## 3. Session event push — `MSG_SESSIONS_CHANGED` — **DONE**

Landed as proto.h 0x39, but deliberately *not* in the shape sketched here.
The sketch was `u8 kind` + the changed session's LIST2 entry; what shipped
is an **empty** message meaning "the table changed, ask again", because
carrying the entry would have made 0x39 a second encoding of
`MSG_SESSION_LIST2` that has to be versioned in lockstep with it, and a
`kind` byte an enumeration of causes no client has a use for. Empty is
also idempotent, which is what lets the daemon coalesce a burst of changes
onto one 20 ms tick.

Detection is derived, not hooked: the daemon re-encodes the LIST2 payload
each tick and compares **bytes** against the last one it sent, so all nine
producers that mutate the table are covered by one gate — including fields
appended to LIST2 later. Baseline seeded at listen time, not on the first
tick, so a client that connects and creates inside the first 20 ms (the
autospawn path) is still notified.

Delivery is global: unlike `MSG_PANE_BELL`, which fans out over one
session's clients, this goes to every `CLIENT_CAP_SESSION_EVENTS` client
whether attached or not, because a sidebar lists sessions it is not
attached to. Wire behaviour is pinned by
`tests/integration/test_session_events.sh` (fan-out, capability gate,
coalescing, no-change silence); the GUI sidebar stops its 2 s poll while
push is live and resumes on any daemon that does not advertise
`SERVER_CAP_SESSION_EVENTS`.
