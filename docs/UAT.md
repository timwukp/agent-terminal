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
| TC-16 | **session A cannot forge session B's history** | with session `victim` live, start a second session and write a marker through every fd 3–40 it might have inherited; then read the fd table of a third session's child | no fd of any child names a `.log`; `victim`'s history contains no marker | PASS³ |
| TC-17 | hostile peer on the socket | a fake daemon binds `default.sock` and answers `attach` with a MSG_ERR whose `msg_len` (65535) exceeds its own frame; then with a well-formed error | first: rc≠0, nothing long printed, no sanitizer report; second: the message still reaches the user | PASS³ |
| TC-18 | **one client cannot exhaust daemon memory** | over the wire: `NEW_SESSION` at 65535×65535 + `SPLIT_PANE` + a child that keeps printing, then **disconnect**; a 200×50 session alongside it; sample daemon RSS for 4 s; `ls` | `ls` reports `1000x1000` and no `65535` anywhere; the 200×50 session is untouched; peak RSS ≤ 128 MiB | PASS⁴ |
| TC-19 | **silent connections cannot lock the user out** | 40 `connect()`s that send zero bytes (`MAX_CLIENTS` is 32), held open; probe a real HELLO during the flood, then again after the deadline; separately, a client that HELLOs and then idles 7 s | during: the probe is refused (the denial is real); within ~6 s: a probe completes HELLO and the daemon logs the deadline drop; the idle HELLO'd client still gets its PONG | PASS⁴ |

¹ TC-07 **failed on the build under test** and exposed BUG-1 (below). It passes since v22.
² History was empty for this session — analyzed and confirmed **by design**, two documented
rules compounding: a TUI on the alternate screen never scrolls content into scrollback, and
the final-screen flush runs on session end, which a SIGKILLed daemon never executes. A
`reload` is the safe upgrade path; SIGKILL loses alt-screen content (primary-screen history
that already scrolled off survives on disk regardless).

³ TC-16/TC-17 **failed on v22** and are the security round below. Both pass since the
`O_CLOEXEC` + `msg_len` fixes, and reverting either fix makes them fail again.

⁴ TC-18/TC-19 likewise failed before the geometry clamp and the HELLO deadline (security
round 2 below). Measured on the unfixed daemon: `ls` reported `65535x65535` and peak RSS was
163 MiB; after the clamp, `1000x1000` and 61 MiB. The geometry assertion is the primary one
precisely because it differs by 65× while RSS differs by only 1.3× against a portable ceiling.

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
| GUI-18 | wheel scrolls into history | in a CLI attach, generate ≥3 screens of output (`seq 1 200`), detach, attach in the GUI, scroll the wheel up | output from **before** the GUI attached is there, continuous across the attach point (no gap, no duplicated screen); scrolling to the bottom resumes live following, and typing snaps back to the prompt |
| GUI-19 | font zoom | with the terminal focused, press ⌘+ a few times, ⌘−, then ⌘0; type `=` and `0` bare afterwards | glyphs grow/shrink between 9 and 24 pt and ⌘0 restores the default; the **grid does not reflow** (CLI `ls` geometry unchanged); bare `=`/`0` still reach the shell |
| GUI-20 | jump-to-bottom pill | scroll up in a busy session (`while true; do date; sleep 1; done`), wait, then click the "↓ bottom" pill | the pill appears only while scrolled up, output keeps flowing beneath, and the click lands the view back on the live prompt and focuses it; the pill is gone at the bottom |
| GUI-21 | window title + Claude panel | switch between two sessions; click the `«` strip on the right edge, then `»` | the window title follows the active session (visible in ⌘-Tab); the Claude panel expands to its placeholder and collapses back to a thin strip |
| GUI-22 | fit session to window | attach to a small session (GUI-created ones start 80×24) in a large window, note the dark letterbox, press ⤢; check a CLI attach afterwards | the session reflows to fill the window (letterbox gone); the CLI viewer shows the SAME new geometry — the resize belongs to the session, and only this button ever sends it |
| GUI-23 | token panel + dark chrome | with a claude conversation running anywhere on the machine, expand the right panel (»/«); give claude a prompt and watch | the whole shell is one dark surface (no light sidebars); the panel lists transcripts newest-first with in/out/cache totals that GROW as claude answers; the newest row shows a per-minute sparkline; with no activity in 48 h it says so instead of erroring |
| GUI-24 | terminal wears the theme | attach to a session smaller than the window (letterboxed), look at the boundary between cells and letterbox; select some text with the mouse | no color seam — the cell background IS the letterbox color (before: default-black cells inside a #1e2228 frame); selection shows as a translucent accent tint with glyphs still readable |
| GUI-25 | hooks tab | open the right panel, switch to **Hooks**; click a rule row; temporarily rename ~/.claude/settings.json and switch tabs back and forth (restore after) | rules from the real settings.json appear grouped by event (this machine: two PreToolUse/Bash rows); clicking shows the script source read-only; with the file missing the tab says so honestly instead of erroring |
| GUI-26 | hook-log chain badge | with no ~/.claude/hooks/hooks.log: read the security card; then hand-build a 2-line valid chain per app/design/hook-log.md, watch; edit line 1's reason in an editor; delete the file | absent → "no hook log" pointing at the doc; valid chain → green "chain verified · 2 events" with the events listed newest-first; after the edit → red "chain broken at line 2" with history still shown; deleted → back to the absent state |
| GUI-27 | letterbox labels itself + newborns fit | attach a small CLI-created session in a big window; then create a new session from a template | small session: a dashed hint sits in the dead space naming the grid ("session 80×24 · ⤢ fit to window"); clicking it fills the window and the hint disappears; the GUI-created session arrives already window-sized, no hint, no button press |

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

## Security round (2026-08-12, pre-release audit of `main` @ `9a97b36`)

Read-only audits of the daemon, the GUI/IPC surface and repo hygiene, run before the first
public release on the principle that a fix shipped after a release is an advisory while the
same fix shipped before it is just release content. The architecture held up — the peer-uid
check is fail-closed, 0700 directories are enforced by `lstat` and never chmod-repaired, the
live daemon has **zero** network sockets, argv reaches `execvp` and never a shell, and
scrollback stores re-serialized cells so `history` cannot replay an escape-injection
payload. Three implementation gaps inside that model were found and fixed:

1. **Scrollback log fds lacked `O_CLOEXEC`** (`scrollback.c`, 4 call sites) — the one break
   in an otherwise complete discipline (`lockfile.c`, `handoff.c`, the listener, client
   sockets, PTY masters and the signal pipe all had it). Because every session's child is
   `execvp`'d from the daemon that holds those fds, a program running in one pane inherited
   **append-write** descriptors to every other session's history file, and both `history` and
   copy-mode present those bytes as authoritative. Measured before the fix: a child's fd
   table held `8 → victim/scrollback.log` and `10 → bystander/scrollback.log`, and a marker
   written through fd 8 appeared in the victim's history. After: children hold exactly fds
   0/1/2, all three on their own PTY slave.
2. **The client trusted a MSG_ERR `msg_len` it never bounded** (`attach.c`, 3 sites) — the
   check that already existed and was already correct at `main.c:174` was simply absent here.
   `payload` is a 4096-byte **stack** buffer, so a peer answering the socket could make
   `%.*s` scan to the end of it and past it.
3. **`handle_list` wrote its payload unbounded** (`server.c`) — safe today only by
   `MAX_SESSIONS 64`, a cap declared in a different header, which made raising that cap a
   buffer overflow rather than a truncated list.

Two **negative results** from building the tests, kept here because each one is a way this
round could have reported success while proving nothing:

- The obvious hostile frame for finding 2 — `payload_len=4, msg_len=65535` — **passes against
  the unbroken code**. `%.*s` stops at the first NUL, and a client that read only 4 bytes
  leaves the rest of `payload` on a freshly-mapped, zero-filled stack page, so the scan halts
  immediately and no sanitizer fires. The frame has to *fill* the buffer with NUL-free bytes;
  then the unbounded code prints 4108 bytes (16-byte prefix + 4092 payload) and the length
  assertion discriminates. ASan does not report the tail read, so the sanitizer check in that
  test is a second, weaker net rather than the primary one.
- A runtime bound for finding 3 can never fire at the real cap (64 × 82 = 5,250 bytes against
  a 1 MiB buffer), so any test at that cap is vacuous by construction. The load-bearing guard
  is therefore a `_Static_assert` relating `MAX_SESSIONS`, `SESSION_NAME_MAX` and
  `PROTO_MAX_PAYLOAD`: raising the cap to 20,000 fails the **build** with a message naming
  the fix, which is strictly stronger than truncating a user's session list at runtime. The
  runtime `break` stays as the second layer.

A fourth finding was in the build system rather than the product, and it invalidated an
earlier run of this round: `make` with no target regenerated one version header and built
**nothing**, because the first rule in the Makefile is the version stamp and not `all`. The
unit suites relink their own objects and passed; the integration tests ran a **three-day-old
daemon**, so the first `O_CLOEXEC` fix appeared not to work. Fixed with an explicit
`.DEFAULT_GOAL := all`, which also makes the Makefile's own usage comment true.

Regression coverage: `tests/integration/test_fd_isolation.sh` (TC-16 + TC-17). Each guard was
observed failing against the pre-fix source restored by `cp` — the fd-table assertion, the
write probe and the `msg_len` length assertion independently — and the `_Static_assert` was
observed failing the build. Full gate after the fixes: 8 unit suites under ASan (0 failures),
24/24 integration scripts on release, release build with 0 warnings.

## Security round 2 (2026-08-12): denial of service by a same-uid client

The uid is this daemon's entire trust boundary, so "authenticate harder" is not an available
defense — any process running as the user may connect and is fully authorized. What *is*
available is refusing to let one client consume an unbounded share of memory or of the client
slot table, which matters more here than in a plain multiplexer: the product exists so that a
long-running agent survives, and a daemon killed by the OOM killer takes every session's PTY
with it. Three fixes, each with the guard observed failing:

1. **Geometry was clamped by the VT engine but not by the model** (`session.c`, 3 entry
   points; `server.c`, 3 wire sites). libvt clamps its own grid to 1000×1000 *before*
   `calloc`, so the engine allocation was never the problem — but `session`/`pane` kept the
   raw `u16` the client sent, and the compositor pads every row out to `pane.cols` while
   drawing only the cells the engine holds. At `cols=65535` that is ~64 KiB of spaces per row
   and ~64 MB per frame, rebuilt on the 20 ms tick while any pane is dirty. Two properties
   make it worse than a large allocation: it costs the attacker nothing but a `connect()`, and
   it **survives the attacking client disconnecting**, because `session_composite_all` does
   not check for an attached client (`ls` shows `0 clients, 2 panes` and the cost continues).
   The clamp went in where geometry *enters* the model, so every rectangle derived from it
   later — layout reflow, zoom, split — is bounded by construction; a clamp at those derived
   sites would mask a layout bug instead of bounding an input. `VT_ROWS_MAX`/`VT_COLS_MAX`
   moved into the public `vt.h` for the same reason: a limit a caller cannot see is a limit
   the caller cannot honor, and it is the caller's unclamped copy that does the damage.
   Beyond the audited list, a **third** entry point turned up while fixing it —
   `session_import_pane` reads per-pane geometry straight out of the handoff state file
   rather than deriving it from the (clamped) view, so a torn file reaches the compositor
   without passing the wire clamp at all.
2. **`PRE_HELLO_BUDGET` bounds bytes, not time** (`server.c`, `main.c`). A peer that connects
   and sends *nothing* spends none of that budget and holds its slot forever; at
   `MAX_CLIENTS 32`, `server_accept` then has no slot and closes every real client
   immediately, so one process with 32 idle sockets locks the user out of their own sessions.
   The fix is a per-client `connected_at` and a 5 s deadline, necessarily driven by the daemon
   tick rather than the read path: a silent peer generates no readable event, so no amount of
   care where bytes arrive can ever notice it. 5 s is ~3 orders of magnitude above a local
   HELLO round trip, which is the margin that keeps it from firing on a real client.
3. **`reflow_node` had no cycle guard** (`layout.c`). Node indices are raw `int8_t` that can
   arrive from a handoff state file; `handoff.c` range-clamps them, which buys safe *indexing*
   but says nothing about termination, so `child[0] == self` — or any edge back to an ancestor
   — recursed until the stack died. A depth cap of `LAYOUT_NODES` cannot reject a legitimate
   layout, since `LAYOUT_MAX_LEAVES` is 6 and the deepest real tree is 5. The misleading
   comment at the `handoff.c` clamp was corrected to say which property it actually buys.

Two **methodological results** worth more than the fixes:

- **The intuitive attack frame was silently defeated by an unrelated defense.** The first
  probe pipelined HELLO + NEW_SESSION + SPLIT_PANE, which coalesce into one read; that read
  exceeds `PRE_HELLO_BUDGET` while `hello_done` is false, so the daemon disconnected the
  probe as garbage and reported a *clean* 2 MiB daemon with no session at all. Same shape as
  round 1's NUL-terminated `%.*s` frame: a probe that never reaches the vulnerable code looks
  exactly like a daemon that is not vulnerable. The shipped test reads HELLO_OK before sending
  anything else, and says why in a comment.
- **RSS is the wrong primary assertion.** 163 MiB exceeds the portable 128 MiB ceiling by only
  1.3×, so the check is one machine away from being a coin flip; the daemon's own `ls` output
  differs by 65× (`65535x65535` vs `1000x1000`) and is exact. RSS is kept as an explicitly
  weaker second net at the ceiling `test_soak.sh` already proves portable.

Regression coverage: `tests/integration/test_dos_limits.sh` (TC-18 + TC-19) and
`reflow_survives_cyclic_tree` in `tests/unit/test_layout.c` — four hostile graphs the public
API cannot construct, where the pass condition is that the test *terminates at all*, plus a
maximum-depth legal tree as the negative control. Every guard was mutation-checked against
pre-fix source restored by `cp`: the layout cycle guard 4/4 (each case observed alone, two as
an ASan stack-overflow and two as a UBSan out-of-range index), and **all nine** assertions of
the integration test observed failing independently, including the ones a short-circuit would
otherwise hide — the identity-clamp mutant, an over-eager clamp that squashes 200×50, a probe
that skips the split, `MAX_CLIENTS 128` (so the flood no longer denies anything, which must
fail the *positive control* rather than pass the test), and a reap whose log line changed but
whose behavior did not. Full gate: 9 unit suites under ASan/UBSan (6,352 checks, 0 failures),
25/25 integration scripts on release, release build with 0 warnings.

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
