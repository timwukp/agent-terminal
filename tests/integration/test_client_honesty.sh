#!/usr/bin/env bash
# The client may only report what the daemon confirmed, and may not abandon
# a healthy session over a refused request. Three findings from a real-pty
# QA pass, none of which the wire-level tests could see:
#
#   1. `attach -s <nonexistent> < /dev/null` printed "[detached — session
#      'x' keeps running]" and exited 0 — the stdin-EOF detach path beat the
#      daemon's MSG_ERR, reporting success for a session that never existed.
#      Scripted callers (the AGENTS.md audience) got a false green.
#   2. A refused split (pane below minimums) hit an unconditionally-fatal
#      MSG_ERR handler: the client printed the daemon's message and then
#      disconnected, costing the user their attach to a healthy session.
#   3. Autospawn resolved agent-terminald via PATH only, so a stale build
#      elsewhere on PATH answered — and its skipped-unknown-frames behavior
#      made every pane chord a silent no-op. The client now prefers the
#      daemon binary sitting next to itself and warns when HELLO_OK's
#      server_flags lack pane support.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

cleanup() {
    pkill -f "agent-terminald" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

SHELL=/bin/sh "$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"

# --- 1. attach to a nonexistent session with stdin at EOF -------------------
"$BIN/agent-terminal" attach -s ghost < /dev/null > "$TMP/1.out" 2> "$TMP/1.err"
RC=$?
[ "$RC" -ne 0 ] || fail "attach to nonexistent session exited 0 (stdin at EOF)"
grep -q "keeps running" "$TMP/1.out" \
    && fail "claimed a nonexistent session keeps running: $(cat "$TMP/1.out")"
grep -q "no such session" "$TMP/1.err" \
    || fail "expected 'no such session' on stderr, got: $(cat "$TMP/1.err")"

# The legitimate flows must be unchanged: create-with-EOF-stdin (a scripted
# `new` is exactly how CI starts sessions) and attach-to-existing.
"$BIN/agent-terminal" new -s real -- /bin/sleep 300 < /dev/null > "$TMP/2.out" 2>&1
[ $? -eq 0 ] || fail "scripted new stopped working: $(cat "$TMP/2.out")"
grep -q "keeps running" "$TMP/2.out" || fail "scripted new lost its detach message"
"$BIN/agent-terminal" ls | grep -q "real:" || fail "scripted new created nothing"
"$BIN/agent-terminal" attach -s real < /dev/null > "$TMP/3.out" 2>&1
[ $? -eq 0 ] || fail "scripted attach to existing session broke: $(cat "$TMP/3.out")"
grep -q "keeps running" "$TMP/3.out" || fail "scripted attach lost its detach message"

# --- 2. a refused split must not cost the user their attach -----------------
python3 - "$BIN" > "$TMP/refusal.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/refusal.out")"
import fcntl, os, pty, select, struct, sys, termios, time

BIN = sys.argv[1]

def drain(fd, dur):
    out = b''
    end = time.time() + dur
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                c = os.read(fd, 65536)
            except OSError:
                break
            if not c:
                break
            out += c
    return out

pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm-256color'
    os.environ['SHELL'] = '/bin/sh'
    os.execv(BIN + '/agent-terminal',
             [BIN + '/agent-terminal', 'new', '-s', 'narrow', '--', '/bin/sh'])
# 30 columns: below 2*PANE_MIN_COLS+1, so a side-by-side split is refused.
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 30, 0, 0))
drain(fd, 1.0)
os.write(fd, b'\x1c%')
out = drain(fd, 1.5)
assert b'too small' in out, 'refusal message not shown: %r' % out[-200:]
assert os.waitpid(pid, os.WNOHANG) == (0, 0), \
    'client exited over a refused split'
# The attach must still be fully functional: echo works, a LEGAL split works.
os.write(fd, b'echo STILL-ATTACHED\n')
out = drain(fd, 1.0)
assert b'STILL-ATTACHED' in out, 'session dead after refusal: %r' % out[-200:]
os.write(fd, b'\x1c"')  # stacked: 24 rows is plenty
out = drain(fd, 1.5)
assert b'\xe2\x94\x80' in out, 'legal split failed after a refusal: %r' % out[:150]
os.write(fd, b'\x1cx')
drain(fd, 1.0)
os.write(fd, b'\x1c\x04')
drain(fd, 0.5)
os.waitpid(pid, 0)
print('refusal-nonfatal ok')
PY
grep -q "refusal-nonfatal ok" "$TMP/refusal.out" || fail "refusal probe incomplete"

# Fatal errors must still be fatal: interactive attach to nothing exits 1.
python3 - "$BIN" > "$TMP/fatal.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/fatal.out")"
import os, pty, select, sys, time
BIN = sys.argv[1]
pid, fd = pty.fork()
if pid == 0:
    os.environ['TERM'] = 'xterm-256color'
    os.execv(BIN + '/agent-terminal', [BIN + '/agent-terminal', 'attach', '-s', 'ghost2'])
out = b''
end = time.time() + 5
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try:
            c = os.read(fd, 4096)
        except OSError:
            break
        if not c:
            break
        out += c
_, st = os.waitpid(pid, 0)
assert b'no such session' in out, 'fatal error message missing'
assert os.waitstatus_to_exitcode(st) == 1, 'fatal error lost its exit code'
print('fatal-still-fatal ok')
PY
grep -q "fatal-still-fatal ok" "$TMP/fatal.out" || fail "fatal probe incomplete"

# --- 3. autospawn prefers the sibling daemon over PATH ----------------------
pkill -f "agent-terminald" 2>/dev/null
sleep 0.5
mkdir -p "$TMP/decoy"
cat > "$TMP/decoy/agent-terminald" <<SH
#!/bin/sh
echo DECOY-RAN >> "$TMP/decoy.log"
exit 1
SH
chmod +x "$TMP/decoy/agent-terminald"
PATH="$TMP/decoy:$PATH" "$BIN/agent-terminal" new -s sib -- /bin/sleep 300 \
    < /dev/null > /dev/null 2>&1 \
    || fail "autospawn failed with a decoy on PATH"
[ -f "$TMP/decoy.log" ] && fail "autospawn ran the PATH decoy instead of the sibling"
"$BIN/agent-terminal" ls | grep -q "sib:" || fail "sibling daemon created nothing"

echo "PASS: no false success on missing sessions; refused split keeps the attach; autospawn prefers the sibling daemon"
