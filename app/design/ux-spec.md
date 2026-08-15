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
- Right-click → kill session (`MSG_KILL_SESSION`), asked first by an **in-app**
  confirmation that replaces the row it is about — never `window.confirm`, which
  on macOS completes false without showing anything (wry's WKWebView delegate has
  no `runJavaScriptConfirmPanel`), so a platform dialog here is not a weaker guard
  but a dead button. Focus lands on Cancel; Escape dismisses; the prompt is
  dropped if its session dies or its name reappears on a different pid, since the
  daemon addresses sessions by name and a freed name gets reused.

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

## Appearance

- Two themes. Tokens live once, as CSS custom properties keyed on
  `:root[data-theme]` (`app/tauri/src/theme.css`); inline styles read them
  through `theme.*` (which is `var()` references, so a switch repaints them
  for free), and xterm's canvas — which cannot evaluate `var()` — gets
  concrete hex from `resolveTokens()`.
- **Light is designed, not inverted.** Every ink/surface pairing a component
  actually renders is measured against WCAG 2.1 in `theme.test.ts` (4.5:1
  text, 3:1 non-text) instead of being eyeballed; the same measurement moved
  dark's own `danger`, which sat at 4.21:1 on a panel row. `accent` is a fill
  and a series colour, never text — the one policy the numbers depend on.
- Light also ships all 16 ANSI slots, because xterm's defaults are
  dark-surface values (`white` #d3d7cf is 1.46:1 on white, i.e. session output
  the user cannot read). Dark deliberately sets none of them: the session owns
  its colours.
- The preference is `system` (the default), `light`, or `dark`, stored in
  `localStorage` and switchable at the bottom of the sidebar. While it is
  `system` the window follows OS appearance changes live; an explicit choice
  stops the following without unsubscribing, so switching back to `system`
  resumes immediately.
- The first paint precedes any module, so `html` carries a static background
  per `prefers-color-scheme`. Not an inline script: that would need a CSP
  `script-src` relaxation, which this app does not grant for a flash of white.

## Non-goals (v1)

- No tabs/windows beyond one attached session per window (matches the
  daemon's one-session-one-surface model; multiple windows = multiple
  attach connections, which the protocol already supports).
- No hook editing (read-only until the viewer proves stable).
- No theming beyond the two themes above — no custom palettes, no
  per-session colours.
