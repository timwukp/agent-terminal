# Claude panel — token usage, hooks, security

These three cards visualize **Claude Code's** state, not agent-terminal's.
The daemon carries opaque bytes; everything here comes from Claude Code's
own files under `~/.claude/`. That boundary is deliberate: the terminal
protocol stays workload-agnostic.

## Token usage

### Primary source: transcript JSONL

Claude Code writes a per-session transcript to
`~/.claude/projects/<cwd-slug>/<session-id>.jsonl` (slug = cwd with `/`
replaced by `-`). Assistant messages carry a `usage` object
(`input_tokens`, `output_tokens`, `cache_read_input_tokens`,
`cache_creation_input_tokens`). The file grows as the conversation runs —
watching it gives genuinely real-time, per-message-granularity numbers
with zero configuration.

**Schema caveat:** the transcript format is not a documented public
contract. The parser must be lenient — unknown fields ignored, lines
without `usage` skipped, malformed lines counted and surfaced as a small
"N unparsed" badge rather than an error. Fixtures pin the formats we have
observed; a schema change degrades the panel, never crashes it.

### Session ↔ transcript correlation

1. Sessions launched from a GUI template record `(session_name, cwd,
   start_time)` locally.
2. Compute the cwd slug, watch that project directory (FSEvents via the
   `notify` crate), bind to the `.jsonl` whose mtime first advances after
   `start_time`; confirm by reading the file's own `sessionId`/`cwd`
   fields.
3. Two candidates in the same cwd (two claude instances) → show the
   project **aggregate** with an "ambiguous" badge instead of guessing.
4. Sessions not launched by the GUI → manual "bind transcript" picker
   listing recent JSONLs for the project.
5. No transcript (child sessions, transcript disabled) → explicit
   "no transcript" state.

### Secondary source: OTEL

Claude Code can export telemetry (token counters among them) via OTLP.
When the user has an exporter configured, an OTLP-receiving mode gives
official, stable-schema numbers at the cost of requiring that
configuration and a listening endpoint. Design decision: JSONL is
primary (zero-config), OTEL is an opt-in second source (PR7); when both
are live the panel shows JSONL and footnotes the OTEL cumulative if they
diverge by >5%.

## Hooks card (read-only in v1)

Parse `~/.claude/settings.json` → `hooks` into a table:

| Event | Matcher | Command | Timeout |
|---|---|---|---|
| PreToolUse | Bash | ~/.claude/hooks/block-git-push.sh | … |

- Grouped by event (PreToolUse, PostToolUse, Stop, …), one row per
  matcher/command pair, with the source file shown (settings.json vs
  project `.claude/settings.json` if we later add per-project scan).
- Clicking a command shows the script source (read-only viewer).
- **No editing in v1.** Claude Code itself rewrites settings.json;
  concurrent writes from a GUI are a real corruption hazard. Editing
  lands only after the viewer proves stable, with atomic
  write-temp-then-rename plus an mtime conflict check.

## Security card

- Heuristic summaries of the user's PreToolUse guard scripts (e.g.
  block-git-push.sh → "blocks: git push (all forms)"; the redaction gate
  → the pattern list it scans for). Heuristics are labeled as such; the
  raw script is one click away and is the truth.
- **Execution visibility gap:** hooks write no log today, so "what got
  blocked when" is not observable. The opt-in convention that closes it
  — a hash-chained JSONL at `~/.claude/hooks/hooks.log`, tamper-EVIDENT
  by design and honest about not being tamper-proof — lives in
  [hook-log.md](hook-log.md). The GUI tails and chain-verifies the file
  when present; absent the log, the card states plainly that execution
  history is not recorded.
- **Panel-update transport decision (2026-08-11):** the Usage/Hooks/
  security cards each poll their Tauri command at the sidebar's 2 s
  cadence while visible — a handful of stats per poll, components
  unmount when hidden. A single multiplexed event channel (the KiroCrew
  pattern) is deliberately deferred until a genuinely streaming source
  exists (PR7 OTEL); when it lands, all panel consumers migrate to one
  `claude-events` channel at once.
