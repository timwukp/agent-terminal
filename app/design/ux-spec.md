# GUI UX spec — Route A (Tauri + xterm.js)

One window, three regions:

```
┌────────────┬──────────────────────────────────┬───────────────┐
│  SIDEBAR   │           TERMINAL               │  CLAUDE PANEL │
│            │                                  │  (collapsible)│
│ ▸ work  2⧉ │   xterm.js viewport rendering    │ tokens ▁▃▅▇   │
│ ▸ build    │   the attached session exactly   │ in/out/cache  │
│ ▸ logs     │   as the CLI client would        │───────────────│
│            │   (composited frames verbatim)   │ hooks rules   │
│ [+ Claude] │                                  │ security log  │
│ [+ Shell]  │                                  │               │
└────────────┴──────────────────────────────────┴───────────────┘
```

## Sidebar (session dashboard)

- Backed by `MSG_LIST_SESSIONS2` polled every 2 s on the control connection.
- Per session: name, `npanes` badge (`2⧉`), zoom marker, attached-client
  count, child pid. Dead entries never appear (daemon contract: finished
  sessions vanish from the list; `history` recovers their output).
- Click a session → switch the attach connection (drop, reconnect, ATTACH).
- Template buttons at the bottom: one click = `MSG_NEW_SESSION` with the
  template's argv. Defaults ship in `app/tauri/templates.json`
  (`New Claude session` → `["claude"]`, `New shell` → `[$SHELL]`);
  user overrides in `~/.agent-terminal/gui-templates.json`.
- Right-click → kill session (confirm dialog; `MSG_KILL_SESSION`).

## Terminal region

- xterm.js fed verbatim from SNAPSHOT/OUTPUT. No client-side pane
  rendering — dividers arrive pre-composited from the daemon.
- Pane interaction overlays, derived from `MSG_LAYOUT` cell rects:
  - Click inside a pane rect → `MSG_SELECT_PANE` mode 0 (by id).
  - Active pane gets a subtle border highlight (overlay div, not cells).
  - Toolbar: split ─ / split │ / close / zoom (modes map 1:1 to protocol).
  - Drag-on-divider: **cursor affordance only until the daemon supports
    resize** (deferred-daemon-work.md #1); show a tooltip "resize needs
    daemon ≥ vNN" when server_flags lacks the bit.
- Keyboard passes through untouched — chords like `Ctrl-\ z` keep working
  because the GUI is just another client; the GUI also offers the same
  operations as buttons.

## Claude panel (right, collapsible)

Three stacked cards; each degrades to an explicit empty state, never a
blank region:

1. **Token usage** — live in/out/cache-read/cache-creation counters +
   sparkline for the bound transcript; session cumulative totals.
   States: `live`, `aggregate (ambiguous bind)`, `no transcript`.
   See claude-panel.md.
2. **Hooks** — read-only rules table from `~/.claude/settings.json`
   (event, matcher, command, source file). See claude-panel.md.
3. **Security** — the block rules the user's PreToolUse scripts enforce,
   plus a recent-events tail when a hook log exists. See claude-panel.md.

## Notifications

macOS notification when a session finishes a long task and the window is
not focused. Triggers and thresholds in notifications.md.

## Non-goals (v1)

- No tabs/windows beyond one attached session per window (matches the
  daemon's one-session-one-surface model; multiple windows = multiple
  attach connections, which the protocol already supports).
- No hook editing (read-only until the viewer proves stable).
- No theming beyond light/dark following the OS.
