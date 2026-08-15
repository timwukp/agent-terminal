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
| TC-20 | **the shipped service unit names the prefix you installed to** | `make install PREFIX=/opt/at-install-test DESTDIR=…`, then parse the rendered plist with `plistlib` and grep the systemd `ExecStart`; then install again to a *second* prefix | both units exec `<PREFIX>/bin/agent-terminald`, neither contains a placeholder, the template-only note, or `/usr/local/bin`; `<PREFIX>/bin` is first on the launchd `PATH` with `/usr/bin` and `/bin` still present; the default prefix lists `/usr/local/bin` exactly once; the second install re-renders instead of reusing the first | PASS⁵ |
| TC-21 | **an install that leaves a different daemon elsewhere says so** | drive `tools/check_install_paths.sh` with three synthetic prefixes — a differing binary, a byte-identical one, a symlink to the installed copy — and once with `cmp` hidden by an empty `PATH` | the differing copy warns and the warning names **both** paths; rc is 0 in every case, including the warning one; silence for identical bytes, for the symlink, and for an absent prefix; with `cmp` unavailable it prints `DID NOT RUN` rather than nothing | PASS⁵ |
| TC-22 | **the `PREFIX` your shell hands `make` cannot render an unstartable unit** | `make install DESTDIR=… PREFIX=~/.local` (tilde quoted, so `make` receives it literally) and again with a relative `PREFIX`; read `ProgramArguments[0]` back with `plistlib` and grep `ExecStart`; then `env -u HOME make units PREFIX=~/.local` | both prefixes render an **absolute** path equal to the expanded prefix, and a binary is actually installed at the path the unit execs; no `<destdir>~` sibling directory is created; with `HOME` unset the build stops non-zero naming `HOME` rather than rendering a unit | PASS⁶ |

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

⁵ TC-20/TC-21 are new in security round 4 below and cover the install path rather than the
⁶ New in the install round below. Both prefix forms fail against the previous tree: the tilde one renders `~/.local/bin/agent-terminald`, which is what was measured on the developer machine, and the relative one renders a path that means nothing to a service manager. 4/4 Makefile mutants killed; the fifth assertion in that section is a cross-check on the test's own expectation and is documented in the test as unkillable, with its predicate verified by moving the installed binary aside.
running daemon. Both fail against the previous tree: TC-20 because the units had
`/usr/local/bin` written into them, TC-21 because no check existed. Measured on this machine
before the change: `/usr/local/bin/agent-terminald` and `~/.local/bin/agent-terminald` were
byte-identical (`5a61d0dc7306c310…`) **only because the installed plist had been hand-edited**
to the second path — the shipped copy said the first.

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
| GUI-07 | kill | right-click a throwaway session; read the prompt; press Escape; right-click again and click **Kill**. Do this in a real `.app` bundle, not only the debug binary | the prompt appears **inside the window** in place of the row, naming that session; Escape and Cancel restore the row and kill nothing; Kill ends the session, the row disappears and `ls` agrees. A prompt that never appears means the build regressed to `window.confirm`, which is a no-op on macOS |
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
| GUI-28 | **viewer refuses a symlink** | with the throwaway hook rule below in place, click its row and read the script; then `ln -sf ~/.ssh/id_rsa /tmp/uat-hook.sh` (any file you can name works — a `printf MARKER > /tmp/uat-secret` is the polite version) and click the row again | first click: the script source. After the swap: a refusal naming a non-regular file, and **none of the target's bytes appear** — the check is per click, so no restart is involved; deleting the link and restoring the real file serves it again |
| GUI-29 | **oversize script truncates visibly** | `python3 -c 'open("/tmp/uat-hook.sh","w").write("#x\n"*800000)'` (≈2.3 MiB), click the row, scroll to the end of the viewer | the source renders and ends in a visible notice that the script is larger than 1 MiB and only the first 1 MiB is shown, pointing at the file; the panel stays responsive and the window does not grow by the file's size |
| GUI-30 | hook log tolerates a hostile writer | with a valid 2-line chain (GUI-26) present, append 3 MiB with no newline in it (`python3 -c 'open(P,"a").write("x"*3_000_000)'`), watch the card for ~10 s; then append a single `\n` followed by a fresh valid line | the card keeps updating throughout (no freeze, no ballooning memory): the unterminated line is counted as malformed rather than buffered, the badge reports the chain broken, and the following newline resynchronizes so the new event is listed |
| GUI-31 | a bell in a background SPLIT session notifies | split a session (`Ctrl-\ %` in a CLI attach or the toolbar), focus a DIFFERENT session in the GUI (window still frontmost is fine — use a second session, not an unfocused window, to isolate the trigger), then in one pane of the split run `sleep 1; printf '\a'` | the split session's row gets the ✓ badge (and an OS notification if the window is in the background and the row is not muted) — before MSG_PANE_BELL this was silent, because composite frames strip the raw `\a` and only the idle machine could fire |
| GUI-32 | theme switch, and light is readable where dark was | with a session attached, pick **light** in the sidebar's Theme control: read the chrome, the session output (run `ls --color=always` and `printf '\e[97mbright white\e[0m\n'`), an active-pane border on a split, and the kill confirmation; then pick **system** and flip macOS System Settings ▸ Appearance while the window stays open; quit and relaunch after choosing **dark** | the whole window switches at once — chrome and terminal, no dark island; every glyph stays readable, including bright white, which xterm's own default palette renders at 1.46:1 on white; the pane border and the ✓ badge remain visible; on **system** the window follows the OS immediately without a relaunch; an explicit **dark** survives the relaunch (the preference is stored, and only `system` follows the OS) |

**Use a throwaway session for GUI-06/GUI-07.** Creating and killing are destructive; never
exercise them against a session doing real work.

**The hook rule for GUI-28/GUI-29 must be inert.** The panel lists rules by event and
matcher without regard to whether they can fire, so add the throwaway to
`~/.claude/settings.json` under a matcher that matches no tool (`"matcher":
"UatNeverMatches"`) with `"command": "/tmp/uat-hook.sh"`, and remove it afterwards. A rule
with a real matcher would have Claude Code *execute* the file — including the 2.3 MiB one.

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
- **A third denial surface, found by CI failing the test rather than the product.** The
  40-connection flood aborted on macOS CI with `ECONNREFUSED` at connection 40 while passing
  locally: `server_accept` takes **one** connection per poll cycle against a listen backlog of
  16, so a burst can fill the accept *queue* — and macOS answers `ECONNREFUSED` on an AF_UNIX
  socket that is still listening. CI hit it because the daemon was still compositing part 1's
  sessions and so accepted more slowly than the flood connected. Two changes, because the
  queue and the slot table are different resources: the flood now retries a refusal (verified
  the hard way — `SIGSTOP` the daemon so the queue provably fills, and the pre-patch flood dies
  with `ECONNREFUSED` where the patched one reaches 40 and exits 0 on `SIGCONT`), and part 2
  kills part 1's sessions first so it measures the slot table rather than the compositor's tick
  budget. The backlog itself is left alone: unlike a held slot it is self-healing, since a
  refused client simply retries.

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

## Security round 3 (2026-08-12): the GUI's read gate and its plugin ACL

The webview is inside the trust boundary by design — this is a client for spawning shells, so
gating the argv it sends would be theatre while `stdin_data` exists. What is *not* inside the
boundary is the filesystem the panel reads on the user's behalf, and the hook rules it reads
come from a file two other programs write. Four changes, plus one bug found and deliberately
left for its own PR:

1. **The read gate followed symlinks** (`hooks.rs`). `Path::is_file` goes through
   `fs::metadata`, which answers about the symlink *target*, so the "regular file" half of the
   gate was satisfied by a link. The exact-match-against-the-snapshot half already held — the
   snapshot is only ever written from `~/.claude/settings.json` and there is no fs-write
   command — so this is not arbitrary file read; the reachable case is that a hook command
   normally lives somewhere far more writable than `~/.claude` (`/tmp/guard.sh`, a script
   inside a checked-out repo), and replacing it in place with a link to `~/.ssh/id_rsa` has the
   panel render the key on the next click, without the attacker needing read access to the
   target at all. Fixed with `symlink_metadata` + `file_type().is_file()`, which also rules out
   a FIFO — one click into a read that never returns, while holding the panel's lock. Because
   `File::open` follows links and runs after the `lstat`, the opened fd's `(dev, ino)` is
   compared to the `lstat`'s: the check is on the file that was actually opened, not on what
   the path meant a moment earlier.
2. **Both reads are now bounded** (`hooks.rs`). `read_to_string` on a hook script and the
   log-tail delta were unbounded, in a panel that polls every 2 s. The script cap is 1 MiB with
   the truncation *stated in the rendered text*, because a viewer whose job is auditing a hook
   silently showing 1 MiB of 3 is the worst available outcome. The log gets two separate caps:
   1 MiB per poll (the cursor advances only by what was consumed, so a backlog is verified
   across the next few polls rather than inside one blocking call, and `N events` therefore
   counts what has been *verified*), and 1 MiB for a single unterminated line — the per-poll cap
   bounds work but not memory, since bytes with no newline in them are held for the next poll
   and accumulate for as long as a writer withholds the newline.
3. **The webview's plugin ACL was `notification:default`** (`capabilities/default.json`), all
   16 of the plugin's permissions, against three calls in `notify.ts`. Now those three are
   named one at a time. Stated honestly: **13 of the 16 name commands
   `tauri-plugin-notification` 2.3.3 does not register at all** (`init()` registers exactly
   `is_permission_granted`, `request_permission`, `notify`), so this is least privilege, not a
   closed hole. It is worth keeping anyway because the ACL is what a future plugin version's
   new commands would be granted by. The mapping is not greppable from the npm package —
   `requestPermission()`/`sendNotification()` reach the backend through a `window.Notification`
   replacement the plugin injects, not through `invoke()` — so `src/capabilities.test.ts`
   asserts the granted set equals the called set in **both** directions rather than leaving the
   correspondence to a reader.
4. **A comment recording why OSC 8 hyperlinks are inert** (`Terminal.tsx`). There is no
   `linkHandler` and no web-links addon, which is the decision; what needed writing down is that
   xterm's fallback path is also dead only *by accident*. wry 0.55.1's WKWebView UI delegate
   implements four methods and `runJavaScriptConfirmPanel` is not among them, so `confirm()`
   completes false on macOS. Adding the addon, a `linkHandler`, or a webkit2gtk target turns
   session output — which chooses both the visible text and the destination — into browser
   navigation.

Two results worth more than the fixes:

- **A mutant whose output is byte-identical means the test watches the wrong quantity.**
  Removing the script cap (`take(u64::MAX)`) survived the first harness run: the string still
  got truncated further downstream, so every assertion about the *rendered text* passed — while
  the process had still read and held the whole file, which is the entire cost being refused.
  The fix was to the test, not the mutant: `read_capped` returns the byte count it consumed, and
  the assertion is `read == cap + 1`. Generally, a guard that bounds a *resource* rather than a
  *result* has to expose an observable, or it cannot be tested at all.
- **An unschedulable guard belongs in the harness as a named expected survivor.** The
  `(dev, ino)` comparison only fires if the path is swapped between the `lstat` and the `open`,
  which no test can schedule deterministically; deleting it kills nothing. Rather than omit that
  mutant and quietly report a clean sweep, it is run and listed as expected to survive, with
  `same_file` tested directly on two real files instead.

**One real bug found and not fixed here.** `window.confirm()` completing false on macOS is not
only about hyperlinks: `Sidebar.tsx` gates session kill on exactly that call, so the GUI's kill
button is **silently inert** in this build (fail-safe, and GUI-07 was never eyeballed — the
round-1 note that GUI-05..09 remain unconfirmed is why it went unnoticed). It gets its own
stacked PR with an in-app confirmation and the missing `Sidebar` test, rather than being folded
into a security change.

**Follow-up, in the next PR:** fixed. The prompt is now rendered by the app, in place of the
row it is about, with focus on Cancel and Escape to dismiss — and two guards the platform
dialog never had, because a modal dialog blocks the poll while an in-app one does not: the
prompt is dropped if its session dies underneath it, and dropped if that name reappears on a
different pid (the daemon addresses sessions by name, and `nextSessionName` reuses a freed
one, so a prompt left standing across that gap would kill a session its reader never saw).
Seven tests in `src/sidebar/Sidebar.test.tsx`, which did not exist; 6/6 mutants killed,
including restoring `window.confirm` with a spy that *accepts* — a spy returning false would
be satisfied by a component that does nothing at all, which is precisely the bug.

Regression coverage: 5 new tests in `hooks.rs` (9 total in-crate) and
`src/capabilities.test.ts` (4). Mutation-checked against pre-fix source restored by `cp`: the
hook gate 9 killed + the 1 named expected survivor above, the capability ACL 5/5 — including
the two directions of the drift assertion and a vacuity guard, since a test that reads its
sources through `import.meta.glob` passes trivially if the glob matches nothing. Full gate
after `npm run build`: vitest 132 tests in 21 files, `cargo test --workspace` 119 tests,
clippy `-D warnings` clean, `tsc --noEmit` clean, frontend bundle unchanged at 500.85 kB.

## Security round 4 (2026-08-12): what the uid boundary really means, and the install path

Rounds 1–3 fixed defects *inside* the trust model. This round is about the model itself, and
about the one way the project could hand somebody an unpatched daemon while every test stayed
green. One half changes only words; the other half changes the build.

### The threat model, stated instead of implied

`SECURITY.md` said "single-user, local-only tool" and left the consequence to be inferred. The
consequence is worth a sentence of its own: the daemon authenticates *who* connects — the
peer's uid — and then authorizes *everything*. After `MSG_HELLO` any connected client may list
every session, attach to every session, inject keystrokes into every session, kill any of them,
and reload the daemon. There is no per-session token and no capability scoping, so **a command
running inside session A can connect to the socket and take full control of session B** — not
by exploiting a bug, but by using the protocol as designed.

This is the same model as tmux and screen, and the two layers defending it are the right ones
and verified correct in round 1 (socket 0600 inside a 0700 directory; peer-uid check
fail-closed before the protocol is spoken). What they buy is that *other* users cannot reach
your sessions; within your own uid they buy nothing, because a process running as you is
indistinguishable from you. It is called out because this project's selling point is running
*agents* inside sessions, which is exactly where "everything running as you is equally trusted"
stops matching what people assume.

Two limits on how far such a takeover escalates were re-checked by reading the tree rather than
quoted from the previous round: session argv reaches **one** `execvp` call site
(`src/daemon/pty.c:76`) and never a shell — zero occurrences of `system(` or `popen(` anywhere
under `src/` — so argv content is literal argument bytes, and an argv-injection bug elsewhere
could start a *wrong program* but could not become metacharacter injection. And there are zero
`setuid`/`setgid`/`seteuid` calls: `sudo make install` installs a binary that still runs as
you, so taking over the daemon gains an attacker exactly the privileges they already had. The
GUI webview is likewise inside the boundary rather than a sandbox, which is why the defenses
that matter there are the ones keeping *foreign code out of the webview* (no `script-src`
relaxation, no reachable remote origin, no devtools in release, five production dependencies),
not a gate on the argv it sends. No code changed for any of this; the point is that a reader of
`SECURITY.md` should not have to derive it.

### The install path could hand you a stale daemon, silently

This one is a real defect, and it is not in any C file. `make install` defaults to
`PREFIX=/usr/local`; `AGENTS.md` recommends `PREFIX=$HOME/.local` (sudo blocks on a password
prompt with no tty, so agents in particular are told to use it); and both service units had
`/usr/local/bin/agent-terminald` written into them, with a launchd `PATH` block that omitted
`~/.local/bin`. Follow both documents and launchd or systemd starts whatever sits at
`/usr/local/bin` — after an earlier install, an **older build**.

Nothing reports that. The client autospawns the `agent-terminald` next to its own binary, but
only when nothing is answering the socket, so the old daemon wins by answering first; and the
protocol skips frames it does not recognize, so every message the old build predates becomes a
silent no-op — new key bindings do nothing, new fields read as absent, no error anywhere. A
stale daemon is an unpatched daemon, and it looks like a feature that does nothing.

Measured on this machine before the change: `/usr/local/bin/agent-terminald` and
`~/.local/bin/agent-terminald` were byte-identical (`5a61d0dc7306c310…`) — but only because the
installed `~/Library/LaunchAgents/dev.agentterminal.daemon.plist` had been **hand-edited** to
the `.local` path while the repo's shipped copy still said `/usr/local/bin`. The hazard had
already been worked around by hand, once, by the only person who would have noticed.

Two changes:

1. **The units are templates rendered per-`PREFIX` at install time** (`contrib/*.in` →
   `PREFIX/share/agent-terminal/`), with the install prefix substituted into launchd's
   `ProgramArguments[0]`, systemd's `ExecStart`, and the launchd `PATH`. `contrib/` no longer
   contains a copyable unit at all, and TC-20's first assertion is about the repo rather than
   the output for that reason: a test that only checked the rendered copy would keep passing if
   someone re-added a ready-made `.plist` beside the template, and that file *is* the bug. The
   explanatory paragraph in each template is deleted at render time (`@TEMPLATE_NOTE_BEGIN@` /
   `…END@`, stripped **before** substitution) so the installed file never claims to be
   something you still have to edit — the first draft rendered "THIS IS A TEMPLATE" into the
   usable copy, complete with a substituted path.
2. **`make install` warns when a byte-different `agent-terminald` exists at another common
   prefix** (`tools/check_install_paths.sh`, compared with `cmp`, naming both paths). It always
   exits 0: a leftover binary elsewhere is something to tell the user about, not a reason to
   fail their `sudo make install`. Under `DESTDIR` the check is skipped and says so, since the
   prefixes it inspects belong to the build host and not to the staging root.

An unsubstituted placeholder now fails loudly on both platforms rather than starting the wrong
binary: launchd rejects a `ProgramArguments[0]` that is not an executable path, and systemd
reads a leading `@` in `ExecStart` as its argv[0]-override prefix, leaving a *relative* path,
which `ExecStart` does not accept — the unit is refused.

Regression coverage: `tests/integration/test_install_units.sh` (TC-20 + TC-21), 11 assertions.
13/13 mutants killed, 0 survivors, 0 errors — across the Makefile's render rule and prefix
handling, both templates, and every branch of the check script (drop the warning, drop the
`cmp` guard, drop the "both paths" line, reverse the identical-bytes skip). Two mutants first
came back as harness **ERRORs** rather than survivors, correctly: the mutation regex had drifted
against the `UNIT_PATH :=` line and edited nothing, and an edit that changes no bytes proves
nothing about the guard. Restoring the four mutated files was then verified byte-for-byte in
Python against a deliberate drift control, because the first verification loop was written in
zsh, where an unquoted `set -- $pair` does not word-split — every "ok" it printed had compared
an empty string to an empty string.

One finding from this round is a build-system bug worth recording separately: the rendered unit
initially depended only on its template, not on the *value* of `PREFIX`, so
`make install PREFIX=/a` followed by `make install PREFIX=/b` reinstalled `/a`'s unit — the
stale-path bug arriving through the build system instead of through the docs. The first fix, a
content-compare stamp file, **also failed**, because GNU make 3.81 (what macOS ships) compares
mtimes at 1-second granularity and the whole sequence runs inside one second. The fix that
holds is the idiom already used for `$(VERSION_H)`: a `FORCE` prerequisite, render to `$@.tmp`,
and `cmp -s` then either discard or move — always re-run, but leave the mtime alone when the
bytes are unchanged. TC-20's second-prefix install is the assertion that pins it.

## Install round 5 (2026-08-12): the `PREFIX` your shell hands `make`

Round 4 made the service units templates rendered per-`PREFIX`, and TC-20 asserted that the
rendered unit names the prefix you installed to. It passed, and the units it produced on this
machine were still unstartable.

`make install PREFIX=~/.local` — the form written in this project's own session notes — rendered
`ProgramArguments[0] = ~/.local/bin/agent-terminald`, in a file whose own notes say `$HOME does
not expand in plists`. Two shell rules combine to make this invisible. zsh does not expand a
tilde after `=` in a command argument (`magicequalsubst` is off by default), so `make` receives
the tilde literally; and `/bin/sh`, which runs the recipe, expands one only at the **start** of a
word. `install -d ~/.local/bin` therefore lands in `$HOME` while `sed` copies the tilde into the
unit verbatim. The binaries were correct, the daemon was correct, and the file you are told to
copy into `~/Library/LaunchAgents/` could not start.

The same root cause has a second symptom that shows the tilde is not merely cosmetic:
`make install DESTDIR=/tmp/at-tilde PREFIX=~/.local` created a directory named `at-tilde~`,
because in `$(DESTDIR)$(PREFIX)/bin` the tilde is no longer at the start of the word. And a
relative `PREFIX=out` has the same shape without any tilde at all: the files land where the
caller expects, and the unit names a path a service manager started elsewhere cannot resolve.

The fix normalizes `PREFIX` once, in the Makefile, and routes the binaries, the rendered units,
the stale-daemon check and `uninstall` through the normalized value, so those four cannot disagree
about where an install went. A tilde that cannot be expanded — `HOME` unset — stops the build,
because both alternatives (leave the tilde, or resolve it against the current directory) produce a
unit file that lies about where the daemon is.

Why TC-20 did not catch it: it installs to `/opt/at-install-test`, an absolute path with no tilde
in it. The test was checking that the *substitution* worked, which it did. What was never checked
is the layer below — that the value being substituted is one launchd and systemd can use. TC-22
now drives the two forms a person actually types, and its load-bearing assertion is not that the
string looks absolute but that the path the unit execs is a path a binary was installed to.

One mutation-testing note, because it changes what the numbers mean. Four Makefile mutants (drop
the tilde expansion, drop the relative resolution, drop the `HOME` guard, revert one install line
to the raw `PREFIX`) were all killed. A fifth, which installs the binaries to `sbin/` while the
unit still names `bin/`, was killed too — but by TC-20's own `-x` check in the section above,
which runs first, not by the new agreement assertion. That assertion cannot be killed by any
Makefile mutation for exactly that reason, so it is not a bug-finder; it is a cross-check on the
expectation the new test computes for itself. Its predicate was verified separately by moving the
installed binary aside and watching it flip to failing. Recording that distinction matters more
than the count: an assertion nothing can kill is usually decoration, and this one has a stated
reason not to be.

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
