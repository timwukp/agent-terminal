#!/usr/bin/env bash
# Two boundaries that a same-uid attacker sits on, both found in a pre-release
# audit of the current tree:
#
#   1. Scrollback log fds were opened WITHOUT O_CLOEXEC. Every session's child
#      is exec'd from the daemon that holds them, so a program running in one
#      pane inherited append-write descriptors to EVERY other session's and
#      pane's history file — and `history` and copy-mode present those bytes as
#      authoritative. Part 1a reads the child's fd table directly; part 1b
#      writes a marker through every fd and checks no other session's log grew.
#      Asserting on open FLAGS would pass against the bug; only these two forms
#      distinguish a fixed build from a broken one.
#   2. The client read `msg_len` out of a MSG_ERR frame and passed it straight
#      to `%.*s` WITHOUT bounding it by the frame it actually received. The
#      payload buffer in the HELLO path is 4096 bytes of STACK, so a peer
#      answering the socket with a filled payload and msg_len=65535 makes the
#      format scan run to the end of that buffer and past it. Part 2 is a
#      hostile fake daemon; part 2b sends a WELL-FORMED error, so "fix it by
#      never printing" fails the suite too.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
FAKEPID=""
cleanup() {
    # Kill by tracked pid, never by name — a broad pkill also kills the
    # production daemon on a developer's machine.
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    [ -n "$FAKEPID" ] && kill "$FAKEPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

SESSIONS="$TMP/.agent-terminal/sessions"

# Enumerate one process's open descriptors as "fd -> target" lines. /proc where
# it exists (Linux/CI), lsof on macOS. Returns non-zero when NEITHER is
# available, because a check that cannot run must not report clean.
list_fds() { # pid
    if [ -d "/proc/$1/fd" ]; then
        local f
        for f in "/proc/$1/fd"/*; do
            [ -e "$f" ] || continue
            printf '%s -> %s\n' "${f##*/}" "$(readlink "$f" 2>/dev/null)"
        done
        return 0
    fi
    command -v lsof > /dev/null || return 1
    lsof -p "$1" 2>/dev/null | awk '
        NR > 1 { fd = $4; sub(/[rwu]+$/, "", fd)
                 if (fd ~ /^[0-9]+$/) printf "%s -> %s\n", fd, $NF }'
}

# --- 1. no child inherits a scrollback fd -----------------------------------

SHELL=/bin/sh "$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"

# The victim goes first so the daemon already holds its append-write log fd
# when the later sessions are forked — that ordering is the whole bug.
"$BIN/agent-terminal" new -s victim -- /bin/sleep 300 < /dev/null > /dev/null 2>&1 \
    || fail "could not create the victim session"
[ -f "$SESSIONS/victim/scrollback.log" ] || fail "victim session has no log to protect"

# 1a. the fd table of a child spawned AFTER the victim's log was opened.
# Distinct sleep argument so it can be told apart from the victim's child.
"$BIN/agent-terminal" new -s bystander -- /bin/sleep 301 < /dev/null > /dev/null 2>&1 \
    || fail "could not create the bystander session"
sleep 1

CHILD=""
for p in $(pgrep -P "$DPID" 2> /dev/null); do
    case "$(ps -o command= -p "$p" 2> /dev/null)" in
        *"sleep 301"*) CHILD="$p"; break ;;
    esac
done
[ -n "$CHILD" ] || fail "could not find the bystander's PTY child under daemon $DPID"

list_fds "$CHILD" > "$TMP/fds" \
    || fail "no way to enumerate fds on this platform (need /proc or lsof) — the isolation check could not run, which is not a pass"
# Liveness: a child always has stdin/stdout/stderr on its PTY slave. Fewer
# than three lines means the enumeration failed, and an empty fd list would
# otherwise satisfy every assertion below.
[ "$(wc -l < "$TMP/fds")" -ge 3 ] \
    || fail "fd enumeration returned fewer than the 3 std fds, so its silence proves nothing: $(cat "$TMP/fds")"
if grep -q '\.log' "$TMP/fds"; then
    fail "the PTY child inherited a log fd across execvp: $(grep '\.log' "$TMP/fds" | tr '\n' ' ')"
fi

# 1b. the write probe. The marker text never goes to the prober's stdout, so
# reaching ANOTHER session's log requires an inherited descriptor.
#
# Scoped to other sessions deliberately: fds 10 and 11 are writable inside any
# `bash -c` loop that redirects (bash parks the original stderr at fd >= 10
# while `2>/dev/null` is in effect), so a marker in the prober's OWN log means
# the prober wrote to its own terminal — which fds 1 and 2 already allow.
# Verified by running this identical loop with no daemon present at all.
"$BIN/agent-terminal" new -s prober -- bash -c '
    for fd in $(seq 3 40); do
        if eval "echo POISON-VIA-FD-$fd >&$fd" 2>/dev/null; then
            echo "WRITABLE-FD:$fd"
        fi
    done
    echo FD-PROBE-DONE' < /dev/null > /dev/null 2>&1 \
    || fail "could not create the prober session"

# The prober exits on its own; session end flushes the screen to the log.
sleep 2
"$BIN/agent-terminal" history -s prober > "$TMP/probe.out" 2>&1

# THE assertion, checked before liveness: a leak corrupts the prober's own log
# too (raw text fails the record CRC, so `history` stops at it), and that must
# be reported as the leak it is rather than as a probe that never ran.
grep -rl "POISON-VIA-FD" "$SESSIONS" 2> /dev/null \
    | grep -v "^$SESSIONS/prober/" > "$TMP/hits"
if [ -s "$TMP/hits" ]; then
    fail "a child wrote into another session's scrollback through an inherited fd: $(tr '\n' ' ' < "$TMP/hits")"
fi
grep -q "FD-PROBE-DONE" "$TMP/probe.out" \
    || fail "the fd probe never ran, so its silence proves nothing: $(cat "$TMP/probe.out")"

kill "$DPID" 2>/dev/null
DPID=""
sleep 0.5

# --- 2. a lying msg_len must not be trusted --------------------------------
# A fake daemon owns the socket for this part: the real one is stopped, and
# the client prefers an answering socket over autospawning.

cat > "$TMP/fake.py" <<'PY'
import os, socket, struct, sys
home, plen_claim, mlen_claim = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
d = os.path.join(home, '.agent-terminal', 'run')
os.makedirs(d, 0o700, exist_ok=True)
p = os.path.join(d, 'default.sock')
try:
    os.unlink(p)
except FileNotFoundError:
    pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(p)
os.chmod(p, 0o600)
s.listen(4)
print('fake listening', flush=True)
while True:
    conn, _ = s.accept()
    try:
        conn.recv(64)  # the client's MSG_HELLO
        # code=ERR_NO_SESSION(2), then the CLAIMED msg_len, which the caller
        # sets larger than the frame can hold. The filler is NUL-free on
        # purpose: a NUL would stop %.*s before it left the buffer.
        body = struct.pack('<HH', 2, mlen_claim)
        if plen_claim > 4:
            body += b'A' * (plen_claim - 4)
        # MSG_ERR = 0x03; header is u32 payload_len + u8 type, little-endian.
        conn.sendall(struct.pack('<I', plen_claim) + bytes([0x03]) + body)
    except OSError:
        pass
    finally:
        conn.close()
PY

# 2a. the lie: a payload that FILLS the 4096-byte buffer with NUL-free bytes,
# and claims 65535. The fill is what makes this test discriminate. `%.*s` stops
# at the first NUL, and a client that only read 4 bytes leaves the rest of
# `payload` on a freshly-mapped, zero-filled stack page — so the obvious hostile
# frame (plen=4, mlen=65535) prints nothing, trips no sanitizer, and PASSES
# against the unbounded code. Measured: it did. With no NUL to stop at, the
# unbounded code prints 4108 bytes (16-byte prefix + all 4092 payload bytes) and
# stops only on the first NUL past the buffer, so the LENGTH assertion below is
# what discriminates here — ASan does not report the 1-byte tail read, and the
# sanitizer grep is a second, weaker net rather than the primary check.
python3 "$TMP/fake.py" "$TMP" 4096 65535 > "$TMP/fake.log" 2>&1 &
FAKEPID=$!
wait_for "fake listening" "$TMP/fake.log" 5 || fail "fake daemon never bound: $(cat "$TMP/fake.log")"

"$BIN/agent-terminal" attach -s whatever < /dev/null > "$TMP/lie.out" 2>&1
RC=$?
kill "$FAKEPID" 2>/dev/null; wait "$FAKEPID" 2>/dev/null; FAKEPID=""
sleep 0.3

grep -qi "AddressSanitizer\|runtime error\|stack-buffer-overflow" "$TMP/lie.out" \
    && fail "sanitizer tripped on a lying msg_len: $(head -20 "$TMP/lie.out")"
[ "$RC" -ne 0 ] || fail "client reported success against a daemon that only sent an error"
# A message whose claimed length does not fit the frame must not print at all,
# so no line may approach the 4092 bytes the frame actually carried.
LONGEST=$(awk '{ print length }' "$TMP/lie.out" | sort -n | tail -1)
[ "${LONGEST:-0}" -lt 600 ] \
    || fail "client printed $LONGEST bytes from a message whose claimed 65535 does not fit its 4096-byte frame (read to the end of the buffer and past it)"

# 2b. positive control: a well-formed error must STILL be reported, so the fix
# cannot be "stop printing the daemon's messages".
python3 "$TMP/fake.py" "$TMP" 20 16 > "$TMP/fake2.log" 2>&1 &
FAKEPID=$!
wait_for "fake listening" "$TMP/fake2.log" 5 || fail "fake daemon (2b) never bound"

"$BIN/agent-terminal" attach -s whatever < /dev/null > "$TMP/honest.out" 2>&1
RC=$?
kill "$FAKEPID" 2>/dev/null; wait "$FAKEPID" 2>/dev/null; FAKEPID=""

[ "$RC" -ne 0 ] || fail "client reported success on a well-formed error"
grep -q "AAAAAAAAAAAAAAAA" "$TMP/honest.out" \
    || fail "a well-formed daemon error is no longer shown to the user: $(cat "$TMP/honest.out")"

echo "PASS: children inherit no scrollback fds; a lying MSG_ERR msg_len is bounded while an honest one still prints"
