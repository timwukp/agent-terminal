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

## Build

```sh
make                  # release build (macOS / Linux, no deps beyond libc)
make test BUILD=asan  # unit tests under ASan+UBSan
```

## Security posture

- Unix socket in a 0700 dir, 0600 socket, peer-UID verified
  (`SO_PEERCRED` / `getpeereid`).
- The VT parser — the untrusted-input surface — is an isolated, syscall-free
  library (`src/vt/`), fuzzed (libFuzzer/AFL++) and run under ASan+UBSan in CI.
- No crypto code in this repo: SSH is delegated to the system OpenSSH binary.

## Status

Early development. Milestones: M0 skeleton ✅ → M1 daemon+attach → M2 VT
engine → M3 scrollback persistence → M4 hardening → M5 polish.
