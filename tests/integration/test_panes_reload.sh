#!/usr/bin/env bash
# Graceful reload of a SPLIT session: both children survive with unchanged
# pids, the layout and both screens come back, and the generation advances by
# exactly one. This is handoff format v2 (per-pane records) under test.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald
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

SOCK="$TMP/.agent-terminal/run/default.sock"
[ -S "$SOCK" ] || fail "daemon socket $SOCK missing"

python3 - "$SOCK" "$TMP" > "$TMP/probe.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/probe.out")"
import re, socket, struct, sys, time

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

def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.sendall(frame(0x01, struct.pack('<HH', 1, 1)))
    typ, payload = read_frame(s)
    assert typ == 0x02
    return s, payload

s, hello = connect()
gen0 = struct.unpack('<I', hello[6:10])[0]

name = b'hv'
argv = b'\x00'.join([b'/bin/sh', b'-c', b'printf PANE-ZERO-SCREEN; exec sleep 300']) + b'\x00'
s.sendall(frame(0x12, struct.pack('<HHB', 120, 30, len(name)) + name +
                struct.pack('<H', len(argv)) + argv))
time.sleep(0.5)
s.sendall(frame(0x16, bytes([0, 255])))  # side-by-side split
time.sleep(0.8)

log = open(tmp + '/daemon.log').read()
pids = set(re.findall(r"session 'hv'.*?pid (\d+)", log))
assert len(pids) == 2, 'expected 2 children before reload, log has %r' % pids

s.sendall(frame(0x19, b''))  # MSG_RELOAD
time.sleep(0.3)
s.close()

# The daemon re-execs in place; give it a moment, then reconnect.
deadline = time.time() + 10
s2 = hello2 = None
while time.time() < deadline:
    try:
        s2, hello2 = connect()
        break
    except (ConnectionRefusedError, FileNotFoundError, socket.timeout):
        time.sleep(0.2)
assert s2, 'daemon never came back after reload'
gen1 = struct.unpack('<I', hello2[6:10])[0]
assert gen1 == gen0 + 1, 'generation %d -> %d (one reload must be one step)' % (gen0, gen1)

s2.sendall(frame(0x14, struct.pack('<HHBB', 120, 30, 0, len(name)) + name))
layouts, snap = [], b''
end = time.time() + 3
s2.settimeout(0.2)
while time.time() < end:
    try:
        typ, payload = read_frame(s2)
    except socket.timeout:
        continue
    if typ is None:
        break
    if typ == 0x35:
        layouts.append(payload)
    if typ == 0x30:
        snap += payload
    if typ == 0x31:
        snap += payload[12:]
assert layouts, 'no MSG_LAYOUT after reload+reattach'
vc, vr, active, npanes = struct.unpack('<HHBB', layouts[-1][:6])
assert npanes == 2, 'reload lost a pane: %d survived' % npanes
assert b'PANE-ZERO-SCREEN' in snap, "pane 0's screen content lost across reload"

log2 = open(tmp + '/daemon.log').read()
pids2 = set(re.findall(r"restored pane \d+ pid (\d+)", log2))
assert pids2 == pids, 'children changed across reload: %r -> %r' % (pids, pids2)
s2.close()
print('probe done')
PY

grep -q "probe done" "$TMP/probe.out" || fail "probe did not complete: $(cat "$TMP/probe.out")"
echo "PASS: split session survived a graceful reload — 2 panes, unchanged pids, screen intact, generation +1"
