# End-User Acceptance Test Report — agent-terminal × Claude Code

**Build under test:** `main` @ `1671082` (v21) → re-verified through `07cae92` (v22)
**Date:** 2026-08-09 · **Platform:** macOS arm64 (Linux paths covered by CI + container runs)
**Workload:** a real Claude Code CLI (v2.1.226, Haiku 4.5 via Amazon Bedrock) — the exact
workload this project exists to protect, not a synthetic child process.

## Method

Every interactive case drives the **actual client binary through a real pty**
(`pty.fork` + `TIOCSWINSZ` + real keystroke bytes), because the failure modes that matter
to a user — dropped keys, torn screens, lost focus — are invisible to wire-level tests.
Scripted cases use the documented `< /dev/null` contract. Verdicts assert on **markers**:
each prompt asks Claude to reply with a unique token (e.g. `UAT-CRASH-MARKER-55`), and the
assertion greps for that token after ANSI-stripping (Claude styles output mid-string; a raw
grep can miss a marker split by SGR sequences).

Test sessions are prefixed `uat-`; the tester's real `$HOME` is used so autospawn, socket
permissions, and session directories run exactly as shipped.

## Result matrix

| ID | Feature | Procedure (condensed) | Expected | Verdict |
|----|---------|----------------------|----------|---------|
| TC-01 | `version`, no daemon | `agent-terminal version` on a clean env | client hash + `daemon: not running`, rc 0 | PASS |
| TC-02 | `ls`, no daemon | `ls` before anything runs | rc 0, `no sessions (daemon not running)` | PASS |
| TC-03 | scripted create + autospawn | `new -s uat-claude -- claude --version < /dev/null`; then `history` | rc 0; daemon autospawned; output recoverable | PASS |
| TC-04 | one-shot prompt | `new … -- claude -p 'Reply with exactly UAT-MARKER-7391'` via pty | marker rendered; `[session exited: 0]` | PASS |
| TC-05 | **client crash + reattach** | interactive Claude TUI → prompt marker → `kill -9` the client → `attach` from a new pty → second prompt | conversation on screen after reattach; **same** Claude process answers | PASS |
| TC-06 | panes around live Claude | `Ctrl-\ %` split; type into shell pane (file marker); copy-mode; `Ctrl-\ o`; prompt Claude inside the split; `Ctrl-\ x`; prompt again | divider; input isolation; Claude answers in split and after close | PASS |
| TC-07 | daemon reload under live Claude | `reload` with Claude running; compare child pid + generation | rc 0; pid unchanged; generation +1 | PASS¹ |
| TC-08 | conversation across reload | reattach after reload; check pre-reload marker; new prompt | pre-reload text restored (handoff snapshot); Claude answers | PASS |
| TC-09 | history of a Claude session | `kill` the session, then `history` | markers recoverable (ANSI-stripped) | PASS |
| TC-10 | kill + honest errors | `kill`; `ls`; `attach` the dead name with stdin at EOF | attach fails `no such session`, rc 1 | PASS |
| TC-11 | multi-client mirroring | terminal A runs Claude, terminal B attaches; A prompts | B sees the conversation live | PASS |
| TC-12 | resize propagation | `TIOCSWINSZ` 120×40→100×30 + `SIGWINCH` | daemon geometry updates; Claude repaints | PASS |
| TC-13 | daemon SIGKILL (documented limitation) | SIGKILL the daemon under a live Claude TUI | honest degradation; next `new` works | PASS² |
| TC-14 | batch automation | `new … -- claude -p '…' < /dev/null`; wait for exit; `history` | marker recoverable after clean exit | PASS |
| TC-15 | regression | full unit + integration suites after the TC-07 fix | all green | PASS |

¹ TC-07 **failed on the build under test** and exposed BUG-1 (below). It passes since v22.
² History was empty for this session — analyzed and confirmed **by design**, two documented
rules compounding: a TUI on the alternate screen never scrolls content into scrollback, and
the final-screen flush runs on session end, which a SIGKILLed daemon never executes. A
`reload` is the safe upgrade path; SIGKILL loses alt-screen content (primary-screen history
that already scrolled off survives on disk regardless).

## BUG-1 (found by TC-07, fixed in v22): reload was dead on macOS

- **Impact:** `agent-terminal reload` — the upgrade-without-killing-sessions flow — failed
  with `execv … No such file or directory` on macOS whenever the daemon had been
  **autospawned**, which is how every real daemon starts.
- **Root cause:** `resolve_exe()` tried `/proc/self/exe` (Linux-only), then
  `realpath(argv0)`; autospawn passes the bare name `agent-terminald`, which realpath
  resolves against the daemon's cwd. The handoff uses `execv`, which does no PATH lookup.
- **Why earlier rounds missed it:** the restart test — and every manual check — started the
  daemon by **absolute path**, so `realpath(argv0)` accidentally worked. The harness was
  kinder than reality. `test_restart.sh` now spawns the daemon exactly as autospawn does
  (bare argv[0] via PATH, cwd `/`), and reverting the fix makes it fail.
- **Fix:** `_NSGetExecutablePath` + `realpath` on macOS; `/proc/self/exe` unchanged on Linux.

## Testing notes for TUI workloads (Claude Code, vim, htop, …)

1. **Synchronize before typing.** A TUI drops keystrokes typed before its input widget
   mounts. Wait for a prompt marker (Claude: the `❯` box), then settle briefly. The session
   layer delivers every byte faithfully; readiness is the app's business.
2. **ANSI-strip before matching output.** Styled output splits plain substrings.
3. **Know the alt-screen rule** (footnote 2) before relying on `history` for TUI content.
4. **Assert on the rendered grid, not raw bytes** (round-2 lesson). A TUI child's own
   output contains `│` box borders and `\x1b[2J` repaints, so byte-level checks like
   "divider present in output" are meaningless near it. Render the cumulative capture
   through `vtdump` and assert on the final grid — e.g. the divider *column*:
   `rows where grid[r][80] == '│'` flips 39→0→39 across zoom/unzoom, unambiguously.
5. **Reset view state between cases.** Zoom is daemon-side session state: it survives
   detach and even a client crash (by design), so a case that leaves a session zoomed
   changes what the next case's attach snapshot shows.

A second full UAT round ran against v27 (directional selection, zoom, `ls` pane counts):
12/12 cases pass with the real Claude workload, including zoom-while-crashed, reload
dropping zoom but keeping panes, and the literal-ESC byte-preservation check performed
live against Claude's input box. No product defects found; the two in-run failures were
test-method errors that produced notes 4 and 5 above.

## GUI client (`app/tauri`) — manual checklist

The GUI is not covered by the pty harness above: driving a webview through
`tauri-driver` is flaky on macOS runners, so its acceptance is a **manual** checklist
against a **real daemon**, run before any `app/` PR that changes behaviour. Wire-level
behaviour is covered instead by the `at-client` real-daemon integration tests, which is
where anything expressible as bytes belongs.

Build first — Tauri embeds the frontend at compile time, so a stale `dist/` means the
binary runs old JavaScript and a fix appears not to work:

```sh
cd app/tauri && npm ci && npm run build
cd src-tauri && cargo build          # ./target/debug/agent-terminal-gui
```

| ID | Case | Procedure | Expected |
|----|------|-----------|----------|
| GUI-01 | sidebar reflects reality | run `agent-terminal ls` alongside the GUI | same session names; `⧉` pane count, `🔍` zoom, `N●` client count agree |
| GUI-02 | attach + render | click a session running a TUI | screen repaints from SNAPSHOT: full content, cursor, colours, CJK width |
| GUI-03 | typing | type immediately after the window opens, before clicking anything | every keystroke appears once, in order; no key needs retyping |
| GUI-04 | CLI coexistence | `agent-terminal attach -s <name>` in a terminal while the GUI shows it | both render the same output live; `ls` shows 2 clients |
| GUI-05 | session switch | click between two sessions repeatedly | each switch repaints the correct session; input goes to the newly selected one |
| GUI-06 | templates | click *+ New Claude session* / *+ New shell* | session created with a free name (`claude`, then `claude-2`…), appears in sidebar, renders |
| GUI-07 | kill | right-click a session, confirm | session ends; row disappears; `ls` agrees |
| GUI-08 | click-to-focus | in a split session, click a pane | that pane becomes active (cursor moves); clicking a divider changes nothing |
| GUI-09 | session ends underneath | exit the child in a session the GUI shows | "session ended" state, no hang or spinner |
| GUI-10 | **geometry is not imposed** | note `ls` geometry, launch the GUI, attach, re-check `ls`; then resize the window | cols×rows **unchanged** by either; the view scales and letter-boxes instead |
| GUI-11 | typing after clicking the sidebar | click the session row you are **already** on, then type | keystrokes reach the session; focus is not left on the sidebar button |
| GUI-12 | IPC carries raw bytes | run the GUI from a shell so stderr is visible, type one key | **no** `IPC custom protocol failed` line; if it appears, `connect-src` is missing `ipc:` and every keystroke will be refused |
| GUI-13 | active-pane outline | split a throwaway session via the toolbar, click each pane in turn | the outline sits exactly on the clicked pane's edges (not offset, not scaled wrong); single pane shows no outline |
| GUI-14 | toolbar ops | on a throwaway session: split ▯▯, split ▤, zoom, unzoom, close | each button does what its tooltip says; zoom button shows pressed state while zoomed; close is absent at one pane |
| GUI-15 | overlay tracks the window | with a split session, resize the window smaller | outlines shrink with the letter-boxed view and stay glued to their panes |
| GUI-16 | done notification | attach to a session, run `sleep 12 && echo done` via a loop that prints every second for ≥10 s, focus another app, wait ~5 s after it stops | ✓ appears on the session row; an OS notification appears **only if running from a real `.app` bundle** (`npm run bundle`, then launch the `.app`) — the unbundled debug binary shows the ✓ alone, by design |
| GUI-17 | mute + focus rules | mute the session (🔕), repeat GUI-16; then repeat unmuted with the window focused | muted: ✓ but no pop-up; focused: neither — and selecting the session clears its ✓ |

**Use a throwaway session for GUI-06/GUI-07.** Creating and killing are destructive; never
exercise them against a session doing real work.

### Round 1 (2026-08-10, against the production daemon)

Two defects, both found by using the GUI as a user rather than as a test, and neither
visible to the wire-level suite:

1. **Keystrokes silently dropped** (GUI-03). Reported as "typed and nothing appeared, had to
   type it again". The process sat at **0.0–2.7% CPU** throughout, which ruled out throughput
   and pointed at logic. Three causes, none of them performance: nothing called
   `term.focus()` (xterm.js focuses itself only on a mousedown inside the terminal, so input
   went to the document until the user happened to click); keys pressed before the async
   attach resolved were sent to a not-yet-existing attachment and discarded, error and all;
   and a bounded channel's `try_send` dropped frames under paste or fast typing. Fixed by an
   ordered stdin queue that holds input until attach completes and reports failures instead
   of swallowing them.
2. **The GUI resized a session just by looking at it** (GUI-10) — the more serious find, and
   an accident: `ls` showed the tester's live session had gone from **111x54 to 93x48**, the
   GUI window's cell size, reflowing the Claude TUI inside it. What exposed the cause was a
   second session that changed size while having **zero** clients. Isolated on a throwaway
   daemon: the daemon is correct — geometry is durable *session* state, and resize does not
   leak between sessions — the GUI was simply imposing its window size on ATTACH. Fixed with
   no protocol change: `ATTACH` with `cols=0, rows=0` leaves geometry untouched *and* still
   returns a SNAPSHOT, so the GUI adopts the session's real size and scales its view. Window
   resize no longer sends `MSG_RESIZE` at all.

Both are pinned by mutation-verified real-daemon tests
(`stdin_right_after_attach_is_not_lost`, `attach_with_zero_dims_adopts_size_without_resizing`);
re-imposing the window size makes the latter fail with `left: (93, 48)`, `right: (97, 41)`.

Verified in round 1: GUI-01, GUI-02, GUI-04 (CLI and GUI attached to the same live Claude
session simultaneously, both rendering), GUI-10, and GUI-03 at the protocol level.
**Not yet confirmed by eye:** GUI-05 through GUI-09, and the visual quality of the scaled
letter-boxed terminal. The display slept mid-run, so screenshots came back solid black and
the remaining verification was done programmatically over the socket. These stay open.

### Round 2 (2026-08-10, against the production daemon)

One report — "picked new claude session / new shell, typing does nothing" — and it was
**not** a product defect. The sessions had live children (creation worked), and the fix
from round 1 was present in the binary's Rust. What differed was the other half: the
running binary's mtime was **2.5 hours older** than `dist/`, so it carried the previous
frontend. Old JS sent stdin as a JSON number array; new Rust accepts only a bytes
payload and rejected it — **100% of keystrokes**, with the old JS's fire-and-forget
send swallowing every error. Rebuilding both halves fixed it, confirmed end-to-end over
the production socket (a marker typed into a throwaway session echoed back).

The interesting part is that nothing was wrong with the code, and nothing could have
told the tester that. `tauri::generate_context!` embeds `../dist` at compile time, but
`tauri-build` emits `rerun-if-changed` only for `tauri.conf.json` and `capabilities/`.
Measured: with `dist/` **deleted entirely**, `cargo build` exited **0** and produced a
binary referencing no bundled asset at all. `beforeBuildCommand` does not help — it is a
`tauri build` CLI feature, and both the README and app-ci.yml use bare `cargo build`.

Three guards landed, one per failure surface: `build.rs` declares the frontend as a
build input and fails on a missing or stale bundle (verified in four states — missing
fails, fresh builds, source-newer fails, test-only-change builds); `stdin_data`'s
rejection message now names the stale half instead of describing the payload; and
undelivered input raises a visible banner, because a console error is invisible to the
person typing. app-ci.yml asserts the guard still fires — neutering `check_frontend()`
makes that step fail, so it cannot rot into a no-op.

### Round 3 (2026-08-11, against the production daemon)

Same report as round 2 — "typing does nothing" — and the round 2 fix made it
*harder* to diagnose, because the banner it added confidently named the wrong
cause. The tester rebuilt `dist/` as instructed, which changed nothing: the
bundle was already one minute old.

Two distinct defects were behind it, and the first one hid the second.

1. **The app's CSP blocked Tauri's own IPC.** On macOS the IPC call is a `fetch`
   to `ipc://localhost/<command>` (`tauri-2.11.5/scripts/core.js`,
   `convertFileSrc`) while the page is served from `tauri://localhost`. With
   `default-src 'self'` and no `connect-src`, that cross-scheme request is
   refused, so Tauri logs `IPC custom protocol failed` and falls back to
   `postMessage`. The fallback re-serializes the whole envelope through
   `JSON.stringify`, whose replacer turns a `Uint8Array` into `Array.from(val)`
   (`scripts/process-ipc-message-fn.js:26`) — the postMessage path
   **structurally cannot** carry raw bytes. `stdin_data` is the only command
   that sends raw bytes, which is exactly why rendering, the sidebar and
   session switching were all fine while the keyboard was dead. Fixed by adding
   `connect-src 'self' ipc: http://ipc.localhost`.
2. **Round 2's own guard turned a slow path into a dead keyboard.** That guard
   rejected any JSON body as "stale frontend". But a JSON byte array is a
   *legitimate* encoding produced by Tauri's fallback, and refusing it drops
   100% of input. It now decodes both shapes: `Raw` is the fast path, a byte
   array is accepted, and out-of-range elements are an error rather than a
   silent `as u8` truncation. The word "stale" is gone from that message, and a
   test asserts it stays gone.

Causality was established by A/B rebuild of identical code: with `ipc:` present,
`stdin_data` receives `Raw 3 bytes` and returns `Ok(())`; with `ipc:` deleted,
the fallback warning returns and every keystroke arrives as JSON. Four mutants —
restoring the rejection, deleting `ipc:`, dropping `connect-src` entirely, and
truncating with `as u8` — each fail exactly one of the new tests.

A third bug was fixed on the way in and is what the round 2 banner was masking:
clicking a sidebar row left DOM focus on the button, and xterm.js focuses itself
only on a mousedown inside its own element — so clicking the session you were
already on did not remount the component and could not recover focus. The host
now bumps a nonce on every selection, focus is taken on any click in the
terminal region including the letter-box margin, and it is reclaimed when the
window regains focus. Five jsdom tests cover it; removing the mount-time call
that turned out to be dead code failed nothing, which is what identified it as
duplicated rather than untested.

**Method note.** What ended three rounds of guessing was instrumenting both ends
of one path rather than reasoning about it: a temporary command mirroring the
webview's console into the process's stderr, plus a capture-phase `keydown` log
recording the focused element. The first line of output named the cause. Before
that, attach, SNAPSHOT rendering and the daemon's input path had each been proven
working in isolation, which is what narrowed it to the keystroke path — but not
one of those checks could see a CSP violation, because the failure was in the
browser layer between them.

### GUI testing notes

1. **Sample CPU before believing "slow".** An idle process that feels laggy is dropping
   input somewhere, not failing to keep up.
2. **A screenshot of a sleeping display is solid black, not a rendering bug.** Check for a
   plausible image before concluding anything from one.
3. **Never `pkill agent-terminald`.** A launchd-managed daemon may be carrying the tester's
   real work. Confirm a daemon's socket with `lsof -p <pid>` before killing it, and track
   test daemons by pid.
4. **macOS has no `timeout(1)`.** A wrapper that exits 127 looks exactly like a command
   producing no output, which can be misread as a hang.
5. **Automating clicks via `osascript` triggers an Accessibility prompt** and can return
   `missing value` even when the click landed. It is a test-harness artifact; the GUI itself
   needs no such permission.
6. **Run the GUI from a shell with stderr captured, and instrument the webview
   before theorising.** The window has no console, so a JS error or a CSP
   violation is invisible — three rounds of "typing does nothing" had three
   different causes and the browser layer could not be seen from either side.
   A throwaway `#[tauri::command]` that `eprintln!`s what the frontend's
   `console` receives costs minutes and names the cause in its first line.
7. **A diagnostic that guesses a cause is worse than one that describes the
   symptom.** Round 2's "stale frontend: rebuild dist/" was right about the
   payload shape and wrong about why, so round 3's tester followed it and
   rebuilt a bundle that was already current. Report the shape observed; only
   name a cause the code can actually distinguish.
8. **Do not launch a second GUI window while someone is at the keyboard.** A
   test GUI on an isolated `XDG_RUNTIME_DIR` takes focus when it opens, so
   clicks and keys meant for another window land in it — this created an
   unintended session in the test daemon. Isolation contained it, but the
   input hazard is real; drive test instances headlessly instead.
9. **Check the binary's mtime against `dist/`'s before testing the GUI at all.** The
   frontend is embedded at compile time, so a Rust-only rebuild produces a binary
   pairing new commands with old JavaScript — and that binary's symptom is a *product*
   symptom. Round 2 spent real time on "typing does nothing" that was a binary built
   2.5 hours before the frontend it was tested against. `build.rs` now refuses to build
   a stale or missing bundle, and app-ci.yml asserts that refusal still happens.

## Reproducing this UAT

The interactive cases are pty scripts; the shape for all of them:

```python
import pty, os, fcntl, termios, struct
pid, fd = pty.fork()
if pid == 0:
    os.execv(CLIENT, [CLIENT, 'new', '-s', 'uat-x', '--', 'claude', '--model', 'haiku'])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 0, 0))
# read until the prompt glyph appears; write the prompt; write b'\r';
# read until the marker appears; assert.
```

Crash simulation is `os.kill(client_pid, signal.SIGKILL)` — never a clean exit — followed by
a fresh `attach` from a new pty. Detach is the chord bytes `b'\x1c\x04'`. The scripted cases
are plain shell (`new … < /dev/null`, `ls`, `history`, `kill`, `reload`, `version`) asserting
on rc and output per the exit-code table in [AGENTS.md](../AGENTS.md).

## Verdict

**PASS.** The product does what it promises for its stated purpose: a Claude Code session
survived a `kill -9`'d terminal, a daemon binary upgrade, splits, resizes, and multi-client
viewing — same process, same conversation throughout. One high-severity platform bug was
found by exercising the exact path a real user takes (autospawn → reload) and is fixed and
regression-guarded in v22.
