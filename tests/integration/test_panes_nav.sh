#!/usr/bin/env bash
# Directional selection, zoom, and the v2 session list — wire-level, so each
# geometry assertion is exact. The chord bytes themselves are covered by
# tests/unit/test_scan.c; this file trusts the daemon side of the contract.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    # Kill by tracked pid, never by name: a broad pkill also hits a
    # PRODUCTION daemon on the same machine (it did — took a live session
    # with it). The pid survives `reload` because the handoff is an in-place
    # execv, so this stays correct across restarts the test performs.
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

SHELL=/bin/sh "$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"

python3 - "$TMP/.agent-terminal/run/default.sock" > "$TMP/probe.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/probe.out")"
import socket, struct, sys, time

sock_path = sys.argv[1]

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
    typ, _ = read_frame(s)
    assert typ == 0x02
    return s

def collect(s, dur):
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
    s.settimeout(10)
    return got

def last_layout(got):
    assert 0x35 in got, 'no MSG_LAYOUT (types: %r)' % sorted(got)
    lay = got[0x35][-1]
    vc, vr, active, npanes = struct.unpack('<HHBB', lay[:6])
    panes = {}
    off = 6
    for _ in range(npanes):
        pid_, x, y, c, r = struct.unpack('<BHHHH', lay[off:off+9])
        panes[pid_] = (x, y, c, r)
        off += 9
    return vc, vr, active, panes

s = connect()
name = b'nav'
argv = b'\x00'.join([b'/bin/sleep', b'300']) + b'\x00'
s.sendall(frame(0x12, struct.pack('<HHB', 160, 48, len(name)) + name +
                struct.pack('<H', len(argv)) + argv))
collect(s, 0.6)

# Build a 2x2-ish grid: split side-by-side, then stack each half.
s.sendall(frame(0x16, bytes([0, 255])))   # % : left/right — active = right
collect(s, 0.8)
s.sendall(frame(0x16, bytes([1, 255])))   # " : right splits top/bottom — active = right-bottom
collect(s, 0.8)
s.sendall(frame(0x18, bytes([4, 0])))     # up -> right-top
got = collect(s, 0.8)
vc, vr, active_rt, panes = last_layout(got)
rt = panes[active_rt]
assert rt[1] == 0, 'up did not land on the top-right pane: %r' % (panes,)
print('OK: directional up landed on the top-right pane (id %d)' % active_rt)

s.sendall(frame(0x18, bytes([7, 0])))     # left -> pane 0 (left column)
got = collect(s, 0.8)
_, _, active_l, panes = last_layout(got)
assert active_l == 0, 'left did not land on pane 0: active=%d %r' % (active_l, panes)
print('OK: directional left landed on pane 0')

s.sendall(frame(0x18, bytes([6, 0])))     # right -> back into the right column
got = collect(s, 0.8)
_, _, active_r, panes = last_layout(got)
assert active_r != 0, 'right stayed on pane 0'
print('OK: directional right left pane 0 (id %d)' % active_r)

# A direction with nothing there is a polite no-op, not an error.
s.sendall(frame(0x18, bytes([7, 0])))     # left -> pane 0
collect(s, 0.6)
s.sendall(frame(0x18, bytes([7, 0])))     # left again: nothing to the left
got = collect(s, 0.6)
assert 0x03 not in got, 'no-op direction produced MSG_ERR'
print('OK: direction with no pane there is a quiet no-op')

# Secondary-key discrimination: split the right-top pane once more (stacked),
# giving three right-column panes with distinct centers. From the full-height
# left pane, "right" must pick the RIGHT pane whose center is vertically
# nearest the left pane's center — not the first, not the farthest.
s.sendall(frame(0x18, bytes([6, 0])))     # into the right column
collect(s, 0.6)
s.sendall(frame(0x18, bytes([4, 0])))     # to right-top
collect(s, 0.6)
s.sendall(frame(0x16, bytes([1, 255])))   # stack right-top again -> 4 panes
collect(s, 0.8)
s.sendall(frame(0x18, bytes([7, 0])))     # back to left
got = collect(s, 0.6)
_, _, active_l2, panes4 = last_layout(got)
assert active_l2 == 0, 'setup: expected to be back on pane 0'
lp = panes4[0]
lcy = lp[1] + lp[3] // 2
s.sendall(frame(0x18, bytes([6, 0])))     # the discriminating "right"
got = collect(s, 0.8)
_, _, active_n, panes4 = last_layout(got)
cands = {pid: (xy[1] + xy[3] // 2) for pid, xy in panes4.items() if pid != 0}
nearest = min(cands, key=lambda k: abs(cands[k] - lcy))
assert active_n == nearest, \
    'right picked pane %d (center %d) instead of nearest %d (center %d) to %d' % (
        active_n, cands.get(active_n, -1), nearest, cands[nearest], lcy)
print('OK: directional right picked the vertically nearest pane (id %d)' % active_n)

# Fold the grid back to 3 panes so the later zoom/split arithmetic is stable.
s.sendall(frame(0x18, bytes([0, active_n])))
collect(s, 0.4)
s.sendall(frame(0x17, bytes([255])))
collect(s, 0.8)
s.sendall(frame(0x18, bytes([7, 0])))     # settle on pane 0
collect(s, 0.6)

# Zoom: the layout broadcast shows the active pane at full view, others hidden
# from render; unzoom restores. Verify via geometry in MSG_LAYOUT after apply.
s.sendall(frame(0x18, bytes([8, 0])))     # zoom toggle on pane 0
got = collect(s, 0.8)
vc, vr, active_z, panes = last_layout(got)
z = panes[active_z]
assert z[2] == vc and z[3] == vr, 'zoomed pane is not full-view: %r of %dx%d' % (z, vc, vr)
print('OK: zoom gave the active pane the full view (%dx%d)' % (vc, vr))

s.sendall(frame(0x18, bytes([8, 0])))     # unzoom
got = collect(s, 0.8)
vc, vr, active_u, panes = last_layout(got)
u = panes[active_u]
assert u[2] < vc, 'unzoom did not restore the tree rectangle: %r' % (u,)
print('OK: unzoom restored tree geometry (%dx%d)' % (u[2], u[3]))

# Auto-unzoom on split: zoom, then split — the new layout must be tree-shaped.
s.sendall(frame(0x18, bytes([8, 0])))     # zoom
collect(s, 0.6)
s.sendall(frame(0x16, bytes([0, 255])))   # split while zoomed
got = collect(s, 0.8)
vc, vr, _, panes = last_layout(got)
assert len(panes) == 4, 'split under zoom lost panes: %r' % (panes,)
assert all(p[2] < vc for p in panes.values()), 'a pane still holds full width after auto-unzoom+split'
print('OK: splitting under zoom auto-unzoomed first (4 tree panes)')

# v2 session list: per-entry length prefix, pane count present.
s.sendall(frame(0x1a, b''))
got = collect(s, 0.8)
assert 0x37 in got, 'no MSG_SESSION_LIST2'
p = got[0x37][-1]
count = struct.unpack('<H', p[:2])[0]
assert count == 1
elen = struct.unpack('<H', p[2:4])[0]
entry = p[4:4+elen]
nlen = entry[0]
assert entry[1:1+nlen] == b'nav'
npanes = entry[1+nlen+14]
zoomed = entry[1+nlen+15]
assert npanes == 4, 'LIST2 pane count wrong: %d' % npanes
assert zoomed == 0
print('OK: SESSION_LIST2 reports 4 panes, not zoomed')

s.close()
print('probe done')
PY

grep -q "probe done" "$TMP/probe.out" || fail "probe did not complete: $(cat "$TMP/probe.out")"

# The human-facing view: ls must show the pane count via LIST2.
"$BIN/agent-terminal" ls | grep -q "nav: .*4 panes" \
    || fail "ls does not show the pane count: $("$BIN/agent-terminal" ls)"

echo "PASS: directional selection (up/left/right + no-op), zoom toggle with auto-unzoom on split, SESSION_LIST2 in ls"
