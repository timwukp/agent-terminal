# AGENTS.md

Instructions for coding agents (Claude Code, Cursor, Aider, Copilot agents,
…) operating on or with this repository. Human readers want
[README.md](README.md); this file is the machine-oriented equivalent, with
exact commands, exit codes and verified behaviours.

Everything below was executed against the real binaries. Where a behaviour is
surprising, the surprise is documented rather than smoothed over.

---

## 1. What this project is

A tmux-like session multiplexer in C17 for macOS and Linux, so that a
long-running terminal program (typically an AI coding agent) survives the death
of the terminal that displays it.

- `agent-terminald` — daemon: owns the PTY, the emulated screen, scrollback.
- `agent-terminal` — thin client: attaches over a unix socket, renders.

Zero runtime dependencies beyond libc. No network listener. No crypto code
(SSH is delegated to the system `ssh` binary).

## 2. Build and verify

```sh
make                              # release → build/release/
make test BUILD=asan              # 4701 unit checks under ASan+UBSan
make BUILD=release all            # explicit variant
BUILD=release bash tests/integration/test_reattach.sh   # acceptance test
```

**`BUILD` must match between building and testing.** The integration scripts
resolve `build/$BUILD` and default to `release`; if you build `asan` and run
the scripts without `BUILD=asan`, they abort with
`FAIL: <path> missing — run: make BUILD=asan all`. That message means a
build-variant mismatch, not a test failure.

Full local gate before proposing a change:

```sh
make test BUILD=asan \
  && python3 tools/gen_corpus.py 200 \
  && make fuzz-regress BUILD=asan \
  && make BUILD=release all \
  && for t in tests/integration/test_*.sh; do BUILD=release bash "$t" || exit 1; done \
  && python3 tools/check_svg.py docs/architecture.svg \
  && bash tools/check_links.sh
```

A fresh clone commits only **6 seed corpus entries**; `gen_corpus.py 200` expands
them to 206, which is what CI fuzzes. Skipping it makes `fuzz-regress` pass in
about a second while covering 6 inputs — green, but nearly meaningless.

| Target | Purpose |
|---|---|
| `make test BUILD=asan` | unit tests, sanitizers on |
| `make fuzz-regress BUILD=asan` | replay `fuzz/corpus/vt` (any compiler — no libFuzzer needed) |
| `python3 tools/gen_corpus.py 200` | grow the corpus from 6 committed seeds to 206 |
| `make fuzz BUILD=fuzz CC=clang` | libFuzzer binaries (needs the fuzzer runtime; Apple clang lacks it) |
| `make tidy` | clang-tidy over `src/vt/` + `src/common/` |
| `python3 tools/check_svg.py docs/architecture.svg` | diagram geometry |

### Installing

```sh
make                                  # release build
make install PREFIX="$HOME/.local"    # no sudo; ensure ~/.local/bin is on PATH
sudo make install                     # or system-wide, PREFIX defaults to /usr/local
```

`PREFIX` is normalized to an absolute path before anything uses it, so
`PREFIX=~/.local` and a relative `PREFIX=out` are equivalent to the spelling
above. Do not work around this by pre-expanding it yourself; the reason it
matters is that launchd's `ProgramArguments[0]` and systemd's `ExecStart` expand
nothing, so an unexpanded tilde installs the binaries correctly and renders a
unit that cannot start. `make` fails outright if `PREFIX` starts with `~` and
`HOME` is unset.

Installs `agent-terminald`, `agent-terminal`, `agent-terminal.1`, and the two
service units rendered for this `PREFIX` into `PREFIX/share/agent-terminal/`.
`make install` depends on `all`, so a separate build step is optional.

The client autospawns the daemon that sits **next to its own binary**, with
`PATH` as fallback — see the autospawn note in §3; running from
`build/release/` needs no install at all. An agent installing this unattended
should prefer `PREFIX="$HOME/.local"`: `sudo` will block on a password prompt
with no tty.

**Two prefixes at once is the hazard, and it is silent.** `PREFIX` defaults to
`/usr/local` while the line above recommends `$HOME/.local`, so following this
file after an earlier `sudo make install` leaves two builds installed. They do
not conflict at install time — they conflict at connect time, and whichever
daemon starts first answers the socket, which makes sibling-first autospawn moot
because it only fires when nothing is answering. The protocol skips frames it
does not recognize, so an older daemon turns every newer message into a silent
no-op with no error anywhere: a stale daemon is an unpatched daemon.

Two things follow, both enforced rather than documented:

- `make install` compares the binary it just installed against the other common
  prefixes and warns, naming both paths, when they differ
  (`tools/check_install_paths.sh`; skipped when `DESTDIR` is set, since those
  prefixes then belong to the build host). It warns and exits 0 — a leftover
  binary must not fail somebody's install.
- The launchd/systemd units in `contrib/` are **templates** (`*.in`) with a
  `@BINDIR@` placeholder. `make install` renders them per-`PREFIX`. Copying a
  template unsubstituted fails loudly instead of starting the wrong binary.
  `tests/integration/test_install_units.sh` asserts that `contrib/` contains no
  ready-to-copy unit at all, which is the form that stays true if someone adds
  one back.

To find out which daemon is actually answering: `agent-terminal version` prints
the client build, then the daemon's pid, generation and capabilities.

## 3. Command surface

Verified against `agent-terminal` with no arguments:

```
agent-terminal new    [-s name] [-- cmd args...]   create + attach
agent-terminal attach  -s name                     attach to session
agent-terminal ls                                  list sessions
agent-terminal history -s name                     dump scrollback
agent-terminal kill    -s name                     kill session
agent-terminal reload                              restart the daemon, keep sessions
agent-terminal version                             client + daemon versions
```

`a` is an alias for `attach`. Default session name is `main`; default command
is `$SHELL`. The daemon accepts `-f`/`--foreground` and `-v`.

### Exit codes (measured)

| Invocation | Exit | stderr / stdout |
|---|---|---|
| `ls`, no daemon running | **0** | warns on stderr, prints `no sessions (daemon not running)` |
| `ls`, daemon running | 0 | `name: 80x24, pid N, K clients` per line |
| `history -s missing` | 1 | `no scrollback found for 'missing'` |
| `attach -s missing`, daemon reachable or autospawnable | 1 | `no such session` (autospawn prefers the daemon next to the client binary, then `PATH` — see §3) |
| `attach -s missing`, no daemon binary findable at all | 1 | `cannot reach daemon at <socket path>` |
| `attach -s missing < /dev/null` (scripted) | 1 | `no such session` — the stdin-EOF detach waits for the daemon's verdict instead of racing it |
| any verb, daemon accepts but never confirms | 1 | `no confirmation from daemon for session '<n>'` after a 5 s bound — a wedged daemon cannot hang a script |
| unknown verb / missing `-s` | 2 | usage block |
| `kill -s name`, session exists | 0 | `killed 'name'` |
| `kill -s name`, no such session | 1 | `no such session` (the daemon's own message) |
| any verb with an invalid `-s` name | 1 | `[fatal] invalid session name '<n>': no '/', no leading '.'` |
| `reload`, daemon running | 0 | `daemon reloaded in place (pid N, generation G)` |
| `reload`, no daemon running | 1 | `cannot reach daemon at <socket path>` |
| `version`, daemon running | 0 | client hash line, then `daemon: pid N, generation G, panes yes\|no` |
| `version`, no daemon | 0 | client hash line, then `daemon: not running` (deliberately rc 0: asking is not an error) |

`reload` deliberately does **not** autospawn the daemon, unlike `new`/`attach`:
"restart what is running" has no meaning when nothing is running, and starting a
daemon would be a surprising side effect of asking to reload one.

### Session names are one path component

A name becomes a directory under `~/.agent-terminal/sessions/`, so
`at_valid_session_name()` (`src/common/path.c`) rejects an empty name, any `/`,
any control byte, and a leading `.`. Enforced at three layers, for three
different reasons:

| Layer | Why it is separate |
|---|---|
| `src/client/main.c` (all verbs) | `history` never contacts the daemon — it opens the log file itself, so no daemon-side check can cover it |
| `handle_new` (`src/daemon/server.c`) | a daemon must not trust a client; any process with the socket can send `MSG_NEW_SESSION` |
| `session_dir` (`src/common/scrollback.c`) | fail-closed choke point: reaching it means a caller was added without a gate. Returns `-1`/`EINVAL` rather than `mkdir`ing outside the tree |

A leading `.` is rejected as well as `.`/`..` because `sb_list_logs()` skips
dotted dirents — such a session would exist on disk yet never appear in `ls`.
Names are rejected, never sanitized: rewriting one would make `ls` disagree with
the directory it names. Interior dots are fine (`build_2026.08`, `x..y`), as is
any UTF-8 (`日本語`), since every byte there is ≥ 0x80.

`ls` exiting 0 with no daemon is deliberate — "no sessions" is a valid answer,
not an error. **Do not** use `ls`'s exit code to test whether the daemon is up;
grep the output or check the socket path.

`kill`'s exit code *is* trustworthy: it comes from the daemon's reply, not from
a local `write()`. It returns after the session is gone, so a following `ls`
needs no sleep.

`version` is the skew probe: the client's own build hash comes from the
binary; the daemon line is read from a live `MSG_HELLO_OK` (pid, restart
generation, capability bits), so it needs no new protocol and answers for
any daemon back to v1. `version` never autospawns — observing must not
mutate.

### The client autospawns the daemon

`new` and `attach` start `agent-terminald` themselves if the socket is
unreachable (`src/client/attach.c`, `daemon_connect(auto_start)`): they `fork`,
exec **the `agent-terminald` sitting in the client's own directory first**
(resolved via `_NSGetExecutablePath` / `/proc/self/exe` + `realpath`), falling
back to a `PATH` lookup, then retry the connect for up to 2 s. So after
`make install` there is no separate "start the daemon" step, and running the
client straight out of `build/release/` works too — the sibling daemon is
right there. The `cannot reach daemon` row is only reachable when the client
binary was copied somewhere on its own AND no `agent-terminald` is on `PATH`.

Sibling-first exists because `PATH`-only resolution silently started whatever
build was installed first: a stale daemon answers the socket, and the
protocol's skip-unknown-frames rule turns every message it predates into a
no-op with no error anywhere. The client also checks `MSG_HELLO_OK`'s
`server_flags` and warns once on stderr when the answering daemon lacks pane
support, naming the remedy (`agent-terminal reload`).

## 4. Scripted / non-interactive use

**`agent-terminal new -s x -- cmd < /dev/null` now works**: it creates the
session, waits for the daemon to confirm it, prints the detach message, and
exits 0 with the session persisting under the daemon. Measured:

```sh
agent-terminal new -s job -- some-command < /dev/null   # rc=0
agent-terminal ls                                       # => job: 80x24, pid N, 0 clients
agent-terminal attach -s job < /dev/null                # rc=0 (and detaches again)
agent-terminal history -s job                           # scrolled-off output, after the 1 s flush
```

Two earlier generations of behavior are worth knowing because scripts written
against them are still out there. Originally the request itself could be lost
(the daemon discarded bytes queued on a closing connection — the close-race
class). Then the session survived but the *report* raced: the client printed
"keeps running" before the daemon had confirmed anything, so `attach` to a
nonexistent session also claimed success with rc=0. The current client only
reports what the daemon confirmed:

- Session exists / was created → the snapshot arrives → detach message, rc=0.
- It does not → the daemon's error arrives → `no such session` on stderr, rc=1.
- The daemon confirms nothing within 5 s → `no confirmation from daemon`,
  rc=1. A wedged daemon cannot hang a script.

So exit 0 from a scripted `new`/`attach` **does** now mean the daemon
confirmed the session. What a scripted `new` does not give you is a *client*:
stdin is already at EOF, so nothing stays attached to watch output or type.
To drive a session interactively from a script, hold stdin open with a FIFO
and detach explicitly with the `Ctrl-\ Ctrl-d` chord (`\x1c\x04`):

```sh
mkfifo /tmp/at.in
agent-terminal new -s work -- claude < /tmp/at.in > /dev/null 2>&1 &
exec 8>/tmp/at.in            # hold the write end open
sleep 1.5                    # let the session register

agent-terminal ls            # => work: 80x24, pid 99218, 1 client
printf 'make test\n' >&8     # type into the session

printf '\x1c\x04' >&8        # detach, leaving the session running
exec 8>&-
```

This is the same mechanism the integration tests use — read
`tests/integration/test_reattach.sh` for a complete worked example, and source
`tests/integration/lib.sh` for `require_bins` / `require_alive` / `wait_for`.

### Scrollback semantics

While a session is **running**, `history -s name` prints only lines that have
**scrolled off** the primary screen — not the current visible screen. With a
24-row PTY, 200 printed lines yield 177 recovered lines.

Once a session **ends**, the daemon flushes the still-visible screen to
scrollback, so the same 200 lines all come back — 200 recovered, no
duplicates. This is what makes a short crash message recoverable: it never
scrolled off, so without the flush `history` returned **zero bytes**.

Two exceptions:

- A session ending on the **alternate screen** (inside vim, htop, …) is not
  flushed at all — scrollback holds primary-screen content only. Its
  already-scrolled-off lines are unaffected.
- Trailing blank rows are trimmed, so an idle 24-row screen does not append
  ~22 empty lines.

The client's copy-mode (`Ctrl-\ [`) reads the same scrollback, so it shows the
same 177 lines and not the 23 still on screen. Two properties are easy to get
wrong and both have tests:

- Lines are durable only on the 1 s flush tick, so copy-mode reads the on-disk
  log **and** requests the un-flushed tail over `MSG_SCROLLBACK_REQ`. Entering
  immediately after a burst, the log can hold **0** of the scrolled-off lines —
  measured at 0 of 77 for a 100-line burst, every one arriving over the wire
  (`tests/integration/test_pager_ring.sh`, which asserts the log is short first
  so the test cannot pass by the daemon having flushed early).
- That ring reply lands *after* the first draw, so the view follows the tail
  while it sits at the bottom. Without that, copy-mode opens showing older
  history than it holds, with a status line that says otherwise.

The client does **not** link `libvt`, so it has no `wcwidth` and cannot measure
display width. Copy-mode therefore disables autowrap (`\x1b[?7l`) and emits each
stored line verbatim, letting the terminal clip: one stored line is always
exactly one display row. Leaving copy-mode sends `MSG_DETACH` before
re-`MSG_ATTACH`, because `session_attach` returns early for a client already in
the session's table and would send no snapshot at all.

`history` reads the on-disk log directly, so it works with **no daemon
running** and for dead sessions. Lines become durable on the 1 s flush tick
(or on detach/exit), so allow ≥1 s before reading.

`ls` never reports a session as dead: the daemon frees the slot as soon as the
child is reaped, so a finished session simply disappears. Use `history` — not
`ls` — to recover it.

A repaint larger than the snapshot ceiling (`SNAPSHOT_BODY_MAX`, 768 KB) is
split: `MSG_SNAPSHOT` carries the first chunk and the rest follows as
`MSG_OUTPUT` frames on the same socket. The client concatenates both message
types to the same fd, so the split is invisible; do not add client logic that
assumes one attach produces exactly one frame. Before this, a screen whose
serialization crossed `PROTO_MAX_PAYLOAD` (1 MiB — reachable with truecolor
SGR changing per cell) made `client_send` disconnect the very client it was
repainting, which the reconnect loop turned into an attach loop
(`tests/integration/test_snapshot_large.sh`).

### Panes

Compositing lives in the daemon and a composited frame is an ANSI byte blob —
the same kind of thing as a snapshot — so it rides `MSG_OUTPUT` and needs no
client rendering code. New messages exist only for control (`MSG_SPLIT_PANE`,
`MSG_CLOSE_PANE`, `MSG_SELECT_PANE`) and metadata (`MSG_LAYOUT`,
`MSG_PANE_EXITED`), gated by `CLIENT_CAP_PANES` in `MSG_HELLO`'s flags.

Load-bearing rules, each pinned by a test:

- **At exactly one pane the raw PTY tee runs unchanged.** Its bytes are a
  compatibility surface, pinned byte-for-byte against
  `tests/data/golden/single_pane_stream.bin` by `test_panes_compat.sh`
  (regen deliberately with `REGEN_GOLDEN=1`). Compositing engages at ≥2.
- **The composite runs with autowrap off (`?7l`) and no scroll region
  (`\x1b[r`)**, and every transition back to one pane re-arms `?7h` — a
  terminal left wrap-less is broken in a way users cannot diagnose
  (`test_panes_reap.sh` greps for it).
- **`\x1b[K` is unusable in a pane row** — EL erases to the *terminal's*
  right margin, wiping the pane to the right. Trailing blanks are literal
  spaces carrying the correct bg (mutation-tested via `test_panes.sh`).
- **Damage, not full repaints, drives steady-state frames**: `libvt` keeps a
  row-dirty bitmap (`vt_any_dirty` / `vt_row_dirty` / `vt_damage_clear`).
  Reading damage does not clear it — with several attached clients a
  clear-on-read for one would blank the frame for the rest; the caller
  clears once per composited frame. The equivalence test in `test_vt.c`
  (full render vs dirty-rows-only over every case) is what catches a missed
  `mark_row`, which is otherwise silent visual corruption.
- **Geometry**: last-resize-wins at one pane (unchanged); smallest attached
  client wins at ≥2, or the composite would paint outside a smaller
  client's screen. A shrink the split tree cannot satisfy clamps panes to
  ≥1×1 and keeps rendering — destroying a pane on resize is data loss.
- **Wire ids are not slot indices**: pane 0 is forever the first pane (its
  scrollback file is the pre-pane `scrollback.log`); split panes draw ids
  1..254 round-robin so an in-flight close can never kill a reused slot's
  new tenant; 255 means "active". Per-pane history lives in
  `pane<id>.log` — the id goes in the *filename* because session names are
  validated to one path component.
- **Copy-mode follows the active pane** (`MSG_LAYOUT` carries the id; the
  client passes it back in `MSG_SCROLLBACK_REQ`'s appended byte).
- **Directional selection** (`MSG_SELECT_PANE` modes 4–7) picks the
  geometrically nearest pane past the current edge — edge distance first,
  then perpendicular center offset, so straight across beats diagonal. A
  direction with nothing there is a quiet no-op, not an error.
- **Zoom** (mode 8) is a render/geometry overlay: the layout tree is never
  modified, so unzoom is a pure reflow. Split/close/focus-change auto-unzoom
  first — mutating the tree under a zoom would apply geometry the user
  cannot see. Zoom is deliberately dropped across a reload (ephemeral view
  state, not session state).
- **`MSG_SESSION_LIST2`** exists because v1 is positional and frozen: v2
  entries carry a u16 length prefix, so unknown tail fields skip cleanly
  and future appends stay additive. The client uses it when `server_flags`
  says panes exist, and falls back to v1 otherwise.

## 5. Working on the code

### Layout

| Path | Contents |
|---|---|
| `src/vt/` | VT engine → `libvt.a`. **No I/O, no syscalls**; effects only via `vt_callbacks`. The untrusted-input surface. |
| `src/daemon/` | event loop, unix socket server, sessions/panes, PTY, the split tree (`layout.c`), the compositor (`composite.c`), plus the in-place restart handoff (`handoff.c`) and single-instance lock (`lockfile.c`) |
| `src/client/` | thin client, termios raw mode, attach loop, scrollback copy-mode (`pager.c` — the only client code with a view of its own) |
| `src/common/` | wire protocol, ring buffer, scrollback, paths |
| `tests/unit/` | table-driven; `runner.h` is the whole framework |
| `tests/integration/` | end-to-end bash; `lib.sh` holds shared guards |
| `fuzz/` | libFuzzer targets + standalone ASan replay driver |
| `tools/` | `vtdump`, corpus generator, `check_svg.py` |

### Grapheme storage

A cell holds a base codepoint plus **at most one** combining mark, as a bare
BMP codepoint in `vt_cell.comb`. Three constraints shaped that, and a change
here has to answer all three:

- **It is a value, not an index into engine-side storage.** Cells are relocated
  by six bitwise copies that treat them as POD — both scroll helpers,
  `vt_screen_insert_chars`, `vt_screen_delete_chars`, `vt_resize`, and the
  `memset` in `vt_screen_reset` — plus the shallow `vt tmp = *v;` in
  `vt_snapshot`. A value survives all seven with nothing to own and no
  aliasing; a pointer or arena index would leak or dangle at every one.
- **`libcommon` must not depend on `libvt`,** because the client links
  `libcommon` alone. `serialize_line()` in `scrollback.c` receives cells
  through `on_scrollback_line`, whose signature passes no `vt *`, so it has to
  render a mark without asking the engine to decode anything.
- **`uint16_t` at offset 14 lands in existing tail padding,** keeping
  `sizeof(vt_cell)` at 16 — measured, and asserted at compile time in `vt.h`.
  Grid memory is unchanged: a 1000×1000 grid pair is 30.5 MiB either way.

`width == 0` is **not** the attach test. `vt_wcwidth` also returns 0 for NUL,
for C0/C1 controls, and for ZWJ/ZWNJ, variation selectors, BiDi controls and
U+FEFF — none of which may attach to anything. `vt_width.c` therefore keeps two
disjoint tables: `comb_ranges` (262 ranges, 1378 codepoints, of which the 1005
in the BMP can attach and the other 373 are dropped) and `fmt_ranges` (7 ranges,
275 codepoints, dropped as before). Their union is what `zero_ranges` used to
be — verified identical, sorted and disjoint — so
`vt_wcwidth` returns the same width for every codepoint as it did. There is no
generator to re-run: the comment naming `tools/mkwidth.py` is stale and the
tables are hand-maintained, so **partition, never duplicate**.

Attachment targets the cell `vt_screen_put` wrote last, tracked explicitly in
`struct vt` as `last_row`/`last_col`/`last_valid`. The cursor cannot serve: it
has already advanced past the base (by 2 for a wide char), and at the right
margin it stays put with `pending_wrap` set. That state deliberately lives
outside `vt_cursor` so DECSC/DECRC do not save and restore it, and every
primitive that moves the cursor or relocates cells calls `forget_last()` — 16
sites. Erring toward invalidation only drops a mark; missing one attaches a
mark to an unrelated cell.

### Graceful restart (`reload` / `SIGHUP`)

The daemon re-execs **itself**, in place. There is no fd passing and no
`SCM_RIGHTS` anywhere in the tree, because `execve` already preserves the pid,
the parent-child relationship, the session and controlling terminal, the signal
mask, and every descriptor without `FD_CLOEXEC` (`O_NONBLOCK` lives on the open
file description and survives too). Nothing closes a PTY master, so no child
ever sees carrier loss, and `waitpid` still works afterwards because the daemon
is still the parent.

What the new image must redo, because `execve` does **not** preserve it: every
signal handler is reset to `SIG_DFL`, so `setup_signals()` runs again.

State crosses in a 0600 file under the runtime dir — **not `/tmp`**, which the
systemd unit makes private (`PrivateTmp=true`). It names raw descriptor numbers,
so `handoff_import` proves identity rather than trusting them; fd numbers are
reused across `execv`, and adopting the wrong one means treating stderr as a PTY
master:

| Claimed fd | Proof required |
|---|---|
| lock | `st_dev` + `st_ino` match the lock path |
| PTY master | `TIOCGWINSZ` succeeds |
| listener | `S_ISSOCK`, *in the listening state*, and `getsockname` path matches |

`fd_is_listening()` needs two mechanisms, both measured: `SO_ACCEPTCONN` answers
directly on Linux but returns `ENOPROTOOPT` for `AF_UNIX` on macOS, so the
fallback is `listen()` itself — it succeeds on an already-listening socket and
fails `EINVAL` on a connected one, identically on macOS/arm64 and Linux/x86_64.
The fallback is load-bearing, not belt-and-braces: an *accepted client* fd
reports the **same** bound `sun_path` as the listener on both platforms, so the
path check alone would adopt a client connection as the listener.

Any fd ≤ 2 is refused outright, and a state file that fails any check makes the
daemon start **clean** — never exit. An operator with a daemon and no sessions
is better off than one with neither.

Screens cross as `vt_snapshot()` blobs replayed with `vt_feed()`, the same
round-trip a reattaching client already uses, so there is no second
serialization format to keep correct or fuzz. Replay is inert — it emits no
query responses and pushes nothing to scrollback — which is pinned by
`snapshot_replay_is_inert` in `tests/unit/test_vt.c`, not by the
`handoff_importing()` guards in `session.c`; those guards are defense in depth
against a future `vt_snapshot` that probes or scrolls, and were **measured** to
be unreachable today.

The pid deliberately does not change, so the **generation counter** in
`MSG_HELLO_OK` is the only observable that moves. One reload must advance it by
exactly one: the writer stores `g_generation + 1` and the reader adopts that
value verbatim. (Both incrementing was a real bug; nothing caught it because the
client only ever compared for an increase.)

Attached clients are simply disconnected. `attach.c`'s existing 250 ms→4 s
reconnect loop re-`ATTACH`es and re-snapshots, which is far less state to
serialize than the alternative and exercises a path that was already tested.

A single-instance `flock` on `daemon.lock` guards all of this. It must be
`flock`, not `fcntl(F_SETLK)`: record locks are per-process and dropped when the
process closes *any* descriptor on the file, so the double-fork daemonize would
release it, while `flock` belongs to the open file description and survives both
daemonize and `execve`. `lock_release` unlinks **before** `LOCK_UN`, or a waiter
can lock the inode we then unlink. The pre-existing probe-`connect()` in
`server_init` is not a substitute — it is racy, and it cannot exclude a second
daemon at all once the socket file is removed.

**Service units.** Both shipped units needed fixing, in opposite directions, and
the difference is measured:

- systemd's default `KillMode=control-group` signals every process in the
  cgroup, so `stop`/`restart` would kill every PTY child regardless of daemon
  code. `setsid` does **not** leave a cgroup — a `setsid` grandchild is still
  listed in its parent's `cgroup.procs` and is killed by `cgroup.kill`. Hence
  `KillMode=mixed`. `systemctl --user restart` still does not preserve
  sessions; only `reload` does.
- launchd needs no equivalent: on `bootout` only the main process is signalled
  and a `setsid` grandchild keeps running, reparented to pid 1. Its bug was the
  other way round — a bare `KeepAlive true` respawns even after a clean exit,
  and SIGTERM *is* a clean exit here. `KeepAlive={SuccessfulExit:false}` matches
  `Restart=on-failure`: a job exiting 3 started 3 times in 26 s (launchd
  throttles respawns to ~10 s), a job exiting 0 started once.
- `launchctl kill SIGHUP` keeps the same pid across the in-place `execv` and
  launchd goes on supervising it (`PID` unchanged, `LastExitStatus` 0).
  `launchctl kickstart -k` does not — it `SIGKILL`s the job.

**Crash survival is still not delivered**, and the README says so. On `SIGSEGV`
or `kill -9` no code runs to hold the descriptors, and it is the descriptor
closing, not the exit, that hangs up children. That needs a supervisor that is
both fd holder and parent, i.e. inverting the architecture.

### Invariants — do not break these

1. **`src/vt/` performs no I/O and no syscalls**, and never allocates
   proportionally to untrusted input. `vt_feed()` is total: any byte sequence
   is defined behaviour. This is what makes the parser fuzzable in isolation.
2. **No new runtime dependencies.** libc only, both platforms.
3. **All PTY platform divergence stays in `src/daemon/pty.c`.**
4. **Protocol changes are additive**, negotiated by `HELLO`; unknown frame
   types must stay skippable (length-delimited).
5. **Unknown escape sequences parse to a no-op** — never to undefined
   behaviour.

### Conventions

C17, 4-space indent, `snake_case`, ~88-column lines, braces on the same line.
Comments explain *why*, not *what* — match the surrounding density and do not
add narration. `clang-format` config is in `.clang-format` (`make fmt`).

### Adding tests

An end-user UAT report (real Claude Code workload through a real pty, full
case/procedure/verdict log) lives in `docs/UAT.md`; its "testing notes for
TUI workloads" section is required reading before scripting a TUI child —
synchronize on the app's prompt before typing, ANSI-strip before matching,
and know the alt-screen scrollback rule.

- VT changes → cases in `tests/unit/test_vt.c`. They are automatically
  re-run byte-at-a-time, so chunking bugs surface without extra work.
- Protocol/daemon changes → a script in `tests/integration/`, sourcing
  `lib.sh` and calling `require_bins` first.
- A real-terminal capture is the most valuable bug artifact: record with
  `script -r /tmp/capture`, drop it in `tests/data/recordings/` with a golden
  dump from `build/release/vtdump -r 24 -c 80`.

### Static analysis

`.clang-tidy` disables five checks **with recorded evidence** in the file
itself. Do not re-enable them without reading those notes:

- `insecureAPI.DeprecatedOrUnsafeBufferHandling` (67 hits) demands the C11
  Annex K `_s` functions, which are optional and absent from both glibc and
  macOS libc — there is nothing to migrate to. Bounds correctness is enforced
  by ASan/UBSan and fuzzing instead.
- `valist.Uninitialized` (3 hits, all correctly `va_start`/`va_end`-paired) is
  an x86_64-only false positive; clean under Apple clang and clang-tidy 18 on
  arm64 Linux. `va_list` is a struct on x86_64 and a pointer on arm64, which is
  what changes the analyzer's conclusion.
- `bugprone-reserved-identifier` and its two `cert-dcl37-c` / `cert-dcl51-cpp`
  aliases fire on the feature-test macros (`_POSIX_C_SOURCE`,
  `_DARWIN_C_SOURCE`, …). Those names are *required* to live in the reserved
  namespace, so there is no conforming way to satisfy the check.

CI runs clang-tidy with `--warnings-as-errors='*'` over **all four source
dirs** using the same flags as the real build (the generated `at_version.h`
is stubbed — its content is irrelevant to analysis). To reproduce exactly:

```sh
docker run --rm -v "$PWD":/w:ro -w /w ubuntu:24.04 bash -c \
  'apt-get update -q && apt-get install -y -q clang-tidy &&
   mkdir -p /tmp/inc && printf "#define AT_VERSION \"tidy\"\n" > /tmp/inc/at_version.h &&
   clang-tidy --warnings-as-errors="*" \
     src/vt/*.c src/common/*.c src/client/*.c src/daemon/*.c -- \
     -std=c17 -Isrc -I/tmp/inc \
     -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE'
```

`make tidy` runs the same scope locally. Suppressions in the tree are
NOLINT-with-reason at the site (length-prefixed wire fields tripping
`bugprone-not-null-terminated-result`); do not add a bare NOLINT.

Note for Apple Silicon: that container runs arm64 by default while CI is
x86_64, and some analyzer results genuinely differ between the two. Add
`--platform linux/amd64` when a finding looks architecture-dependent.

## 6. CI

`.github/workflows/ci.yml` — 7 required jobs: `build-test` over
{macos, ubuntu} × {release, asan} (4), plus `fuzz-smoke` (60 s/target),
`clang-tidy`, and `docs` (diagram geometry + doc-link resolution).
`fuzz.yml` runs 30 min/target nightly with corpus minimization.

Integration tests run only in the `release` matrix legs, with
`BUILD=${{ matrix.build }}` exported.

## 7. Repository etiquette

- Never commit generated artifacts: `build/`, generated fuzz corpora beyond
  the committed seeds, or captures containing real filenames/paths. A previous
  commit leaked home-directory names via a fuzz seed — corpus entries must be
  synthetic or sanitized.
- Diagram edits: run `python3 tools/check_svg.py docs/architecture.svg`, and
  also look at a render. The checker is a geometric approximation; it exists
  because two collisions once shipped that only a rendered image exposed.
- Licensed MIT; contributions are accepted under the same terms
  (see [CONTRIBUTING.md](CONTRIBUTING.md)). Security issues go through the
  process in [SECURITY.md](SECURITY.md), never a public issue.
