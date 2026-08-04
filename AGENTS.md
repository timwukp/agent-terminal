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
  && make fuzz-regress BUILD=asan \
  && make BUILD=release all \
  && for t in tests/integration/test_*.sh; do BUILD=release bash "$t" || exit 1; done \
  && python3 tools/check_svg.py docs/architecture.svg
```

| Target | Purpose |
|---|---|
| `make test BUILD=asan` | unit tests, sanitizers on |
| `make fuzz-regress BUILD=asan` | replay the 206-entry corpus (any compiler) |
| `make fuzz BUILD=fuzz CC=clang` | libFuzzer binaries (needs the fuzzer runtime; Apple clang lacks it) |
| `make tidy` | clang-tidy over `src/vt/` + `src/common/` |
| `python3 tools/check_svg.py docs/architecture.svg` | diagram geometry |

## 3. Command surface

Verified against `agent-terminal` with no arguments:

```
agent-terminal new    [-s name] [-- cmd args...]   create + attach
agent-terminal attach  -s name                     attach to session
agent-terminal ls                                  list sessions
agent-terminal history -s name                     dump scrollback
agent-terminal kill    -s name                     kill session
```

`a` is an alias for `attach`. Default session name is `main`; default command
is `$SHELL`. The daemon accepts `-f`/`--foreground` and `-v`.

### Exit codes (measured)

| Invocation | Exit | stderr / stdout |
|---|---|---|
| `ls`, no daemon running | **0** | warns on stderr, prints `no sessions (daemon not running)` |
| `ls`, daemon running | 0 | `name: 80x24, pid N, K clients` per line |
| `history -s missing` | 1 | `no scrollback found for 'missing'` |
| `attach -s missing`, no daemon | 1 | `cannot reach daemon at <socket path>` |
| unknown verb / missing `-s` | 2 | usage block |
| `kill -s name` | 0 | `killed 'name'` |

`ls` exiting 0 with no daemon is deliberate — "no sessions" is a valid answer,
not an error. **Do not** use `ls`'s exit code to test whether the daemon is up;
grep the output or check the socket path.

## 4. The constraint that matters most for agents

**`agent-terminal new` with `stdin` closed or `/dev/null` will not leave a
usable session behind.**

Measured: `agent-terminal new -s x -- sleep 30 < /dev/null` exits **0**, and
the child *does* run to completion (verified with a file-writing child — it
still wrote its "survived" marker 8 seconds later). But the client sees
immediate EOF on stdin, treats it as a detach and exits; the child's PTY then
reaches EOF, the daemon reaps it, and the session disappears from `ls` right
away. So `attach` and `history` find nothing.

Consequences for non-interactive callers:

- Do **not** assume `new` + exit 0 means "session created and waiting".
- Do **not** poll `ls` in a loop expecting the session to appear.

Working pattern — hold stdin open with a FIFO, then detach explicitly with the
`Ctrl-\ Ctrl-d` chord (`\x1c\x04`):

```sh
mkfifo /tmp/at.in
agent-terminal new -s work -- claude < /tmp/at.in > /dev/null 2>&1 &
exec 8>/tmp/at.in            # hold the write end open
sleep 1.5                    # let the session register

agent-terminal ls            # => work: 80x24, pid 99218, 1 client

printf '\x1c\x04' >&8        # detach, leaving the session running
exec 8>&-
agent-terminal ls            # => work: 80x24, pid 99218, 0 clients
```

The session now persists with zero clients, exactly as after a client crash.
This is the same mechanism the integration tests use — read
`tests/integration/test_reattach.sh` for a complete worked example, and source
`tests/integration/lib.sh` for `require_bins` / `require_alive` / `wait_for`.

### Scrollback semantics

`history -s name` prints only lines that have **scrolled off** the primary
screen — not the current visible screen. With a 24-row PTY, 200 printed lines
yield 177 recovered lines. It reads the on-disk log directly, so it works with
**no daemon running** and for dead sessions. Lines become durable on the 1 s
flush tick (or on detach), so allow ≥1 s before reading.

## 5. Working on the code

### Layout

| Path | Contents |
|---|---|
| `src/vt/` | VT engine → `libvt.a`. **No I/O, no syscalls**; effects only via `vt_callbacks`. The untrusted-input surface. |
| `src/daemon/` | event loop, unix socket server, sessions, PTY |
| `src/client/` | thin client, termios raw mode, attach loop |
| `src/common/` | wire protocol, ring buffer, scrollback, paths |
| `tests/unit/` | table-driven; `runner.h` is the whole framework |
| `tests/integration/` | end-to-end bash; `lib.sh` holds shared guards |
| `fuzz/` | libFuzzer targets + standalone ASan replay driver |
| `tools/` | `vtdump`, corpus generator, `check_svg.py` |

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

- VT changes → cases in `tests/unit/test_vt.c`. They are automatically
  re-run byte-at-a-time, so chunking bugs surface without extra work.
- Protocol/daemon changes → a script in `tests/integration/`, sourcing
  `lib.sh` and calling `require_bins` first.
- A real-terminal capture is the most valuable bug artifact: record with
  `script -r /tmp/capture`, drop it in `tests/data/recordings/` with a golden
  dump from `build/release/vtdump -r 24 -c 80`.

### Static analysis

`.clang-tidy` disables two checks **with recorded evidence** in the file
itself. Do not re-enable them without reading those notes:

- `insecureAPI.DeprecatedOrUnsafeBufferHandling` (67 hits) demands the C11
  Annex K `_s` functions, which are optional and absent from both glibc and
  macOS libc — there is nothing to migrate to. Bounds correctness is enforced
  by ASan/UBSan and fuzzing instead.
- `valist.Uninitialized` (3 hits, all correctly `va_start`/`va_end`-paired) is
  an x86_64-only false positive; clean under Apple clang and clang-tidy 18 on
  arm64 Linux.

CI runs clang-tidy with `--warnings-as-errors='*'` using the same flags as the
real build. To reproduce it exactly:

```sh
docker run --rm -v "$PWD":/w:ro -w /w ubuntu:24.04 bash -c \
  'apt-get update -q && apt-get install -y -q clang-tidy &&
   clang-tidy --warnings-as-errors="*" src/vt/*.c src/common/*.c -- \
     -std=c17 -Isrc -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE'
```

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
