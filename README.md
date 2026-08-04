# agent-terminal

A tiny tmux-like session multiplexer in C, purpose-built so long-running
terminal AI agents (e.g. Claude Code CLI) survive terminal front-end crashes.

## Why

Session state (PTY, screen, scrollback) normally lives in the same process
that renders it. When a terminal emulator dies under a huge dialog, the
session dies with it. agent-terminal splits the two:

- **`agent-terminald`** — a per-user daemon that owns PTYs, the VT screen
  state, and disk-persisted scrollback. It never dies with the front-end.
- **`agent-terminal`** — a thin client that runs inside any terminal
  (Terminal.app, iTerm2, over SSH), attaches via a unix socket, and renders.
  Kill the client — or the whole hosting terminal — then reattach: exact
  screen restored, child process untouched.

SSH support = the daemon spawns your system OpenSSH client inside the PTY
(`agent-terminal new -s prod -- ssh myhost`), inheriting `~/.ssh` config and
your agent. No custom crypto, ever.

## Usage

```sh
agent-terminal new -s work -- claude   # start claude in a managed session
# ... terminal crashes, laptop hiccup, whatever ...
agent-terminal attach -s work          # everything is still there
agent-terminal ls                      # list sessions
agent-terminal history -s work         # dump scrollback (works for dead sessions)
agent-terminal kill -s work
```

Detach without killing: `Ctrl-\` then `Ctrl-d`.

## Build & install

```sh
make                  # release build (macOS / Linux, no deps beyond libc)
make test BUILD=asan  # unit tests under ASan+UBSan
sudo make install     # /usr/local/bin + man page (PREFIX= to override)
```

### Run the daemon as a service (recommended)

The daemon auto-starts on first `agent-terminal new`, but a service manager
restarts it after crashes/reboots so `attach` always works:

```sh
# macOS
cp contrib/launchd/dev.agentterminal.daemon.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/dev.agentterminal.daemon.plist

# Linux (systemd user unit; add linger to survive logout)
mkdir -p ~/.config/systemd/user
cp contrib/systemd/agent-terminald.service ~/.config/systemd/user/
systemctl --user enable --now agent-terminald
loginctl enable-linger $USER
```

## Security posture

- Unix socket in a 0700 dir, 0600 socket, peer-UID verified
  (`SO_PEERCRED` / `getpeereid`).
- The VT parser — the untrusted-input surface — is an isolated, syscall-free
  library (`src/vt/`), fuzzed (libFuzzer/AFL++) and run under ASan+UBSan in CI.
- No crypto code in this repo: SSH is delegated to the system OpenSSH binary.

## Scrollback persistence

Lines scrolling off the primary screen are kept in a 10k-line in-memory
ring and appended to a CRC-framed on-disk log
(`~/.agent-terminal/sessions/<name>/scrollback.log`, 2×32 MiB rotation).
The log survives daemon crashes — recovery truncates at the first torn
record — and `agent-terminal history -s <name>` reads it directly, no
daemon required. Records store rendered ANSI text, so even a raw
`less -R scrollback.log` is legible.

## Limitations (v1, by design)

- One child process per session — no panes/windows/splits (the protocol
  reserves a `pane_id` byte for a future v2).
- A daemon crash kills child processes (scrollback survives on disk and
  `history` recovers it; the service unit restarts the daemon). Keeping
  children alive across daemon restarts would need fd-passing supervision
  — deliberately out of scope.
- Standalone combining marks are dropped (base+combiner grapheme storage
  is a v2 item); CJK wide characters are fully supported.
- No scrollback *paging UI* in the client yet — use `history | less -R`.

## Status

All v1 milestones complete: M0 skeleton ✅ → M1 daemon+attach ✅ →
M2 VT engine ✅ → M3 scrollback persistence ✅ → M4 hardening ✅ →
M5 polish ✅.

Hardening in place: nightly 30-min libFuzzer runs per target with corpus
minimization (`.github/workflows/fuzz.yml`), clang-tidy CI gate on
`src/vt/` + `src/common/`, and golden-replay tests pinning libvt against
real vttest 2.7 captures (`tests/data/recordings/`).
