#!/usr/bin/env bash
# A screen whose repaint serializes past PROTO_MAX_PAYLOAD must still attach.
#
# Exists because proto_write_frame refuses a frame over 1 MiB and client_send
# answers any refusal by disconnecting — the slow-consumer path, misfiring on
# a frame that was never going to fit at any consumer speed. With the client's
# reconnect loop, one colorful-enough screen turned attach into a
# connect→snapshot→disconnect loop that never converged. A 1000×1000 grid with
# truecolor changing per cell serializes to ~40 bytes a cell, 40× the cap, and
# panes make big composite frames routine rather than exotic.
#
# The fix caps MSG_SNAPSHOT's blob and streams the remainder as MSG_OUTPUT,
# which the client already concatenates to the same fd. So the observables
# are: the attach survives, the parts sum to one oversized repaint, and the
# split point is invisible in the byte stream.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

"$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"
require_alive "$DPID" "daemon"

SOCK="$TMP/.agent-terminal/run/default.sock"
[ -S "$SOCK" ] || fail "daemon socket $SOCK missing"

python3 - "$SOCK" "$TMP/daemon.log" > "$TMP/probe.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/probe.out")"
import socket, struct, sys, time

sock_path, daemon_log = sys.argv[1], sys.argv[2]

def frame(t, payload):
    return struct.pack('<IB', len(payload), t) + payload

def read_exactly(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf

def read_frame(s):
    hdr = read_exactly(s, 5)
    if hdr is None:
        return None, None
    plen, typ = struct.unpack('<IB', hdr)
    payload = read_exactly(s, plen) if plen else b''
    if plen and payload is None:
        return None, None
    return typ, payload

def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(sock_path)
    s.sendall(frame(0x01, struct.pack('<HH', 1, 0)))
    typ, _ = read_frame(s)
    assert typ == 0x02, 'expected HELLO_OK, got %r' % typ
    return s

def read_frame_or_stall(s, what):
    # Progress-based deadline: the socket timeout (30 s, set in connect)
    # restarts at every recv, so any arriving frame is progress and only
    # a genuine stall fails. A fixed wall-clock cap here lost to machine
    # load once: 6.3 s of paint CPU took 125.6 s of wall clock with
    # frames arriving the whole time (docs/UAT.md, round 9), and the
    # test failed a run in which nothing was wrong.
    try:
        return read_frame(s)
    except socket.timeout:
        assert False, '%s stalled: no frame for %gs' % (what, s.gettimeout())

def wait_log(pattern, timeout=10):
    for _ in range(timeout * 10):
        with open(daemon_log, errors='replace') as f:
            if pattern in f.read():
                return True
        time.sleep(0.1)
    return False

# One shell command that paints a 300x300 grid with truecolor changing every
# cell: 300 rows x 300 cols x ~20 bytes/cell of SGR ≈ 2 MB serialized, past
# the 1 MiB frame cap with margin but cheap enough to feed in a moment. The
# window (u16 cols/rows in NEW_SESSION) is set to the same 300x300 so nothing
# scrolls off and the grid really holds what was printed.
paint = ("i=0; r=0; while [ $r -lt 300 ]; do c=0; row=''; "
         "while [ $c -lt 300 ]; do "
         "row=\"$row\\033[38;2;$((i%256));$(((i/256)%256));$((i%199))mX\"; "
         "i=$((i+1)); c=$((c+1)); done; "
         "printf \"$row\\n\"; r=$((r+1)); done; "
         "printf 'PAINTED\\n'; exec sleep 300")

s1 = connect()
name = b'fat'
argv = b'\x00'.join([b'/bin/sh', b'-c', paint.encode()]) + b'\x00'
s1.sendall(frame(0x12, struct.pack('<HHB', 300, 300, len(name)) + name +
                 struct.pack('<H', len(argv)) + argv))
assert wait_log("session 'fat': pid"), 'session never created'

# Drain the live tee until the sentinel arrives, so the grid is fully painted
# before the second client asks for its snapshot.
seen = b''
while b'PAINTED' not in seen:
    typ, payload = read_frame_or_stall(s1, 'paint')
    assert typ is not None, 'daemon dropped the painting client'
    if typ == 0x30:
        seen += payload[-4096:] if len(payload) > 4096 else payload
        seen = seen[-16384:]
s1.close()

# The attach under test. Pre-fix the daemon computed a ~1.9 MB snapshot frame,
# proto_write_frame said no, and client_send disconnected us right here.
s2 = connect()
s2.sendall(frame(0x14, struct.pack('<HHBB', 300, 300, 0, len(name)) + name))

typ, payload = read_frame(s2)
assert typ is not None, \
    'daemon closed the connection on ATTACH — oversized snapshot still disconnects'
assert typ == 0x31, 'expected MSG_SNAPSHOT first, got 0x%02x' % typ
assert len(payload) >= 12
total = len(payload) - 12

# The remainder rides MSG_OUTPUT. The child is asleep and prints nothing, so
# every OUTPUT byte here is snapshot continuation. PING bounds the read: the
# daemon serialized the whole repaint before seeing the PING, so PONG marks
# the end of the snapshot bytes.
s2.sendall(frame(0x40, struct.pack('<Q', 0x1234)))
while True:
    typ, payload = read_frame_or_stall(s2, 'snapshot')
    assert typ is not None, 'daemon dropped us mid-snapshot'
    if typ == 0x41:
        break
    if typ == 0x30:
        total += len(payload)

assert total > (1 << 20), (
    'repaint was only %d bytes — not past the 1 MiB cap, so this test '
    'no longer exercises the split; enlarge the paint' % total)
print('snapshot delivered in parts: %d bytes total' % total)

# The connection must still work for ordinary traffic afterwards.
s2.sendall(frame(0x40, struct.pack('<Q', 0x5678)))
typ, payload = read_frame(s2)
assert typ == 0x41 and struct.unpack('<Q', payload)[0] == 0x5678
s2.close()
print('probe done')
PY

grep -q "probe done" "$TMP/probe.out" || fail "probe did not complete: $(cat "$TMP/probe.out")"
grep -q "over .* backlog, disconnecting" "$TMP/daemon.log" \
    && fail "daemon hit the slow-consumer path during the snapshot: $(grep backlog "$TMP/daemon.log")"

require_alive "$DPID" "daemon"
echo "PASS: $(grep 'snapshot delivered' "$TMP/probe.out")"
