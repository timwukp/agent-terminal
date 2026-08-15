# Notifications — when "claude finished" fires

Two independent triggers, OR-ed; either one notifies if the window is
unfocused (focused windows never notify).

## 1. Bell

xterm.js `onBell` fires when a BEL byte reaches the widget. Claude Code
rings the terminal bell when it finishes a turn (configurable on its side),
so this is the precise signal — **but only in single-pane sessions**: with
one pane the daemon tees raw child output; with ≥2 panes frames are
composited from grid state and BEL never survives the wire
(protocol-notes.md trap #1). Split sessions arrive as `MSG_PANE_BELL`
(0x38) instead, relayed to the webview as the `pane_bell` ctrl event and
treated exactly like a local bell — so the trigger is now reliable at any
pane count, through two disjoint paths that cannot double-ring.

## 2. Output idle

A per-session state machine over `MSG_OUTPUT` arrival timestamps, running
in the Rust core (it sees every frame regardless of pane count):

```
IDLE     --output-->                    ACTIVE
ACTIVE   --sustained ≥ busy_threshold-->  BUSY
ACTIVE   --idle ≥ settle-->              IDLE      (short burst: no notify)
BUSY     --idle ≥ done_threshold-->      IDLE + NOTIFY
```

Defaults (user-configurable): `busy_threshold` 10 s of recurring output
(gaps < 2 s), `done_threshold` 5 s of silence, `settle` 5 s. The
busy-threshold exists so a one-line `ls` in a shell pane doesn't notify;
only sessions that were *working* announce completion.

Per-session mute toggle in the sidebar context menu.

## Rules

- Implemented as a pure function `step(state, event, now) -> (state,
  Option<Notify>)` — table-driven unit tests, no timers in the logic
  (timers only *deliver* ticks).
- Notification content: session name + last non-empty screen line
  (from the GUI's own xterm buffer, not scraped from the wire).
- macOS permission requested on first notify attempt via the Tauri
  notification plugin; denial degrades silently to a sidebar badge.
- The webview is granted exactly three plugin commands —
  `notification:allow-is-permission-granted`, `allow-request-permission`,
  `allow-notify` — and not `notification:default`, which is all 16 of the
  plugin's permissions. Thirteen of those name commands
  `tauri-plugin-notification` does not even register on this platform, so
  the narrowing is least privilege rather than a closed hole. The mapping
  from JS name to command is not greppable from the npm package:
  `requestPermission()` and `sendNotification()` reach the backend through
  a `window.Notification` replacement the plugin injects, not through
  `invoke()`. `src/capabilities.test.ts` fails if the granted set stops
  matching what the source calls, in either direction.
