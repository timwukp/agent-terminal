# Security Policy

## Reporting a vulnerability

Please report suspected vulnerabilities privately via
[GitHub Security Advisories](https://github.com/timwukp/agent-terminal/security/advisories/new)
("Report a vulnerability"). Do not open public issues for security reports.

You can expect an acknowledgment within 7 days. Please include a minimal
reproduction — for VT-engine issues, the raw byte sequence that triggers the
problem is ideal (it can usually be added directly to the fuzz corpus).

## Supported versions

Only the latest release / `main` branch is supported with security fixes.

## Threat model

agent-terminal is a single-user, local-only tool. The daemon and client run
as the same user and communicate over a unix domain socket. There is no
network listener of any kind.

**Trust boundaries:**

1. **Child process output → VT engine** (the primary untrusted input).
   Programs running inside a session — including remote output relayed by
   `ssh` — can emit arbitrary bytes. The VT parser (`src/vt/`) is therefore:
   - an isolated library with **no syscalls** — a parser bug cannot directly
     become file or network access;
   - total: every byte sequence has defined behavior, all parameters are
     clamped, string buffers are fixed-size;
   - continuously fuzzed (nightly libFuzzer in CI with corpus accumulation,
     ASan/UBSan on every PR).

2. **Unix socket clients → daemon.** The socket lives in a 0700 directory,
   mode 0600, and the daemon verifies the peer's UID (`SO_PEERCRED` on
   Linux, `getpeereid` on macOS) before speaking the protocol. Frames are
   length-delimited with a 1 MiB cap; malformed framing disconnects the
   client. A pre-authentication byte budget limits garbage before HELLO.

3. **Scrollback files.** Written 0600 under `~/.agent-terminal/`. Records
   are CRC-framed; corrupt records terminate reads (no parser re-entry on
   damaged data). Note that scrollback contains whatever your sessions
   displayed — treat the directory as sensitive.

4. **Autospawn → what gets executed.** When no daemon answers the socket,
   `new`/`attach` start one: first the `agent-terminald` in the client
   binary's own directory (same trust domain — anyone who can write there
   can replace the client itself), then a `PATH` lookup as fallback. The
   daemon is never resolved from the current working directory, and the
   client warns when the answering daemon's capabilities say it is an
   older build than the client.

5. **Session names → filesystem paths.** A name becomes one path component
   under `~/.agent-terminal/sessions/`, so it is validated by
   `at_valid_session_name()` (no `/`, no leading `.`, no control bytes, not
   empty) at three independent layers: the client CLI, the daemon's
   `MSG_NEW_SESSION` handler, and — fail-closed — the `session_dir()` choke
   point that every name-to-path conversion passes through. Names are rejected
   rather than rewritten.

**Explicit non-goals:**

- Protecting one user from another user *with the same UID* (same trust
  domain by definition).
- Cryptography: agent-terminal implements none. SSH is delegated entirely
  to the system OpenSSH client, which owns key handling and host
  verification.
- Sandboxing the child process: sessions run whatever command you give
  them, with your privileges — exactly like any shell.

## Known security-relevant limitations

- OSC 52 (clipboard write) sequences are passed through to the hosting
  terminal, capped at 100 KiB. If your hosting terminal honors OSC 52,
  a program in a session can write to your clipboard — the same exposure
  as running that program in the terminal directly.
- The reattach snapshot re-arms terminal modes (mouse reporting, bracketed
  paste) exactly as the application left them.
