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

### The UID is the whole boundary: session A can take over session B

State this plainly rather than leaving it to be inferred from "single-user".
The daemon authenticates *who* connects — the peer's UID — and then authorizes
*everything*. After `MSG_HELLO` any connected client may list every session,
attach to every session, inject keystrokes into every session, kill any of
them, and reload the daemon. There is no per-session token, no capability
scoping, no distinction between the client you launched and the next one.

So: **a command running inside session A can connect to the socket and take
full control of session B.** Not by exploiting a bug — by using the protocol
as designed. It can read what session B is displaying, type into it, and kill
it. If you run an AI agent in one session and something sensitive in another,
the first is not fenced off from the second.

This is the same model as tmux and screen, and it is defended by the two layers
that actually apply to it: the socket is 0600 inside a 0700 directory, and the
daemon verifies the peer UID (`SO_PEERCRED` / `getpeereid`) before speaking the
protocol, fail-closed. What those layers buy is that *other* users cannot reach
your sessions. Within your own UID they buy nothing, because there is nothing
left to check — a process running as you is indistinguishable from you.

It is called out here because agent-terminal is built for running agents in
sessions, which is precisely the case where "everything running as you is
equally trusted" stops matching what people assume. If two workloads must not
reach each other, separate them by UID (or container), not by session.

### What that boundary does *not* let through

Two properties limit how much an intra-UID takeover can escalate:

- **Session argv reaches `execvp` and never a shell.** There is no `system()`
  or `popen()` anywhere in the tree, so an argv that contains `;`, backticks or
  `$(…)` is passed to the new process as literal argument bytes. An
  argv-injection bug elsewhere could therefore start a *wrong program*; it could
  not become shell-metacharacter injection.
- **The daemon never runs with elevated privileges.** It is never setuid, and
  `sudo make install` installs a binary that still runs as you. Taking over the
  daemon gains an attacker exactly your own privileges, which is what they
  already had.

### The GUI webview is inside the trust boundary, not a sandbox

The Tauri client in `app/` is a client like any other: whatever runs in its
webview can reach the same commands, including `stdin_data` into an attached
session. It is not a privilege boundary and is not treated as one — gating the
front end's argv would be theatre while `stdin_data` exists, and the product's
whole purpose is spawning shells you chose.

What is defended is keeping *foreign* code out of that webview: the CSP has no
`script-src` relaxation (`'unsafe-inline'` is `style-src` only, which xterm.js
requires), no remote origin is reachable, devtools are off in release builds,
and the front end carries five production dependencies. The read-only Claude
panel is separately gated — it opens only the path a configured hook names, only
while that path is still a regular file, capped at 1 MiB.

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

   The security-relevant corollary is that **autospawn only fires when nothing
   is answering.** A daemon already on the socket wins, whatever its version, so
   an old binary left at another install prefix — started by a service unit
   pointing there — keeps serving after you patch. Since the protocol skips
   frames it does not recognize, it fails silently rather than reporting a
   mismatch: a stale daemon is an unpatched daemon that looks like a feature
   doing nothing. Two mitigations ship: `make install` warns when a *different*
   `agent-terminald` exists at another common prefix, and the launchd/systemd
   units are templates rendered per-`PREFIX` at install time instead of files
   with a path baked in. `agent-terminal version` reports both builds.

5. **Session names → filesystem paths.** A name becomes one path component
   under `~/.agent-terminal/sessions/`, so it is validated by
   `at_valid_session_name()` (no `/`, no leading `.`, no control bytes, not
   empty) at three independent layers: the client CLI, the daemon's
   `MSG_NEW_SESSION` handler, and — fail-closed — the `session_dir()` choke
   point that every name-to-path conversion passes through. Names are rejected
   rather than rewritten.

**Explicit non-goals:**

- Isolating one session from another session of the **same UID** — see the
  section above. Any client that passes the UID check is fully authorized on
  every session, so this is a stated limit of the design rather than a bug to
  report.
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
