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

## 2. Bell signal — `MSG_PANE_BELL`

BEL survives the wire only in single-pane raw-tee mode; composited frames
are rebuilt from grid state and drop it. Notification support in any
client (GUI or future CLI feature) wants a semantic signal:

- New D→C message: `u8 pane_id`, emitted when the VT layer consumes a BEL
  in a session with ≥2 panes (single-pane keeps raw passthrough — no
  double signal).
- Sent only to CLIENT_CAP_PANES clients (others skip unknown types
  anyway, but no point sending).
- Small, high value.

## 3. Session event push (optional, later)

The sidebar polls LIST_SESSIONS2 every 2 s. If polling ever proves
annoying (battery, latency), add `MSG_SESSION_EVENT` (D→C on the control
connection: u8 kind = created/exited/pane-change, then the session's
LIST2 entry). Not worth doing until measured to matter.
