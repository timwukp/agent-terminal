#!/usr/bin/env bash
# A pane's child dying must not kill the session: the sibling absorbs the
# space, capable clients hear MSG_PANE_EXITED, and — the detail that leaves
# terminals permanently broken when missed — the transition back to one pane
# re-arms autowrap (\x1b[?7h), which the composite had turned off.
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

SHELL=/bin/sh "$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"
require_alive "$DPID" "daemon"

SOCK="$TMP/.agent-terminal/run/default.sock"
[ -S "$SOCK" ] || fail "daemon socket $SOCK missing"

python3 - "$SOCK" "$TMP" > "$TMP/probe.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/probe.out")"
import os, signal, socket, struct, sys, time

sock_path, tmp = sys.argv[1], sys.argv[2]

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
    return typ, read_exactly(s, plen) if plen else b''

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sock_path)
s.sendall(frame(0x01, struct.pack('<HH', 1, 1)))  # CLIENT_CAP_PANES
typ, _ = read_frame(s)
assert typ == 0x02

name = b'rp'
argv = b'\x00'.join([b'/bin/sleep', b'300']) + b'\x00'
s.sendall(frame(0x12, struct.pack('<HHB', 80, 24, len(name)) + name +
                struct.pack('<H', len(argv)) + argv))

def pump(dur, sink=None):
    """Collect frames for dur seconds; returns {type: [payloads]}."""
    got = {}
    end = time.time() + dur
    s.settimeout(0.2)
    while time.time() < end:
        try:
            typ, payload = read_frame(s)
        except socket.timeout:
            continue
        if typ is None:
            break
        got.setdefault(typ, []).append(payload)
        if sink is not None and typ == 0x30:
            sink.write(payload)
    s.settimeout(10)
    return got

pump(0.6)
s.sendall(frame(0x16, bytes([0, 255])))  # side-by-side split
got = pump(1.0)
assert 0x35 in got, 'no MSG_LAYOUT after split'
lay = got[0x35][-1]
_, _, active, npanes = struct.unpack('<HHBB', lay[:6])
assert npanes == 2
new_id = active

# Kill the NEW pane's child directly (kill -9: not a graceful close).
import re
log = open(tmp + '/daemon.log').read()
m = re.search(r"pane %d pid (\d+)" % new_id, log)
assert m, 'new pane pid not in daemon log'
os.kill(int(m.group(1)), signal.SIGKILL)

sink = open(tmp + '/after_reap.bin', 'wb')
got = pump(2.0, sink)
sink.close()

# MSG_PANE_EXITED for the killed pane, not MSG_SESSION_EXITED.
assert 0x36 in got, 'no MSG_PANE_EXITED (types: %r)' % sorted(got)
pe = got[0x36][-1]
assert pe[0] == new_id, 'PANE_EXITED names pane %d, expected %d' % (pe[0], new_id)
assert struct.unpack('<i', pe[1:5])[0] == 128 + 9, 'exit status not 128+SIGKILL'
assert 0x34 not in got, 'session exited when only a pane should have'

# Survivor layout: one pane, full view, active back to 0.
assert 0x35 in got, 'no MSG_LAYOUT after reap'
lay2 = got[0x35][-1]
vc, vr, active2, npanes2 = struct.unpack('<HHBB', lay2[:6])
assert npanes2 == 1 and active2 == 0, 'reap left %d panes, active %d' % (npanes2, active2)

# Session still alive and interactive: PING answers.
s.sendall(frame(0x40, struct.pack('<Q', 7)))
got = pump(1.0)
assert 0x41 in got, 'daemon stopped answering after pane reap'
s.close()
print('probe done')
PY

grep -q "probe done" "$TMP/probe.out" || fail "probe did not complete: $(cat "$TMP/probe.out")"

# The transition bytes must re-arm autowrap: a terminal left in ?7l wraps
# nothing ever again, which the user cannot diagnose. grep the raw capture.
grep -q $'\x1b\[?7h' "$TMP/after_reap.bin" \
    || fail "no autowrap re-arm (\\x1b[?7h) in the post-reap byte stream"

# The daemon's session table still holds the session with pane 0's child.
"$BIN/agent-terminal" ls | grep -Eq "rp: .*pid [0-9]+" || fail "session rp gone after pane reap"

require_alive "$DPID" "daemon"
echo "PASS: pane child death reaped as MSG_PANE_EXITED; sibling absorbed the view; autowrap re-armed; session survived"
