#!/usr/bin/env bash
# Panes end-to-end: split, rectangle placement, isolation, close, reattach.
#
# The client's stdout is a terminal byte stream; piping the capture through
# vtdump (exactly as test_golden.sh does) turns it into a grid we can assert
# rectangles on. A wire-level probe drives the daemon so each step is
# deterministic — the real client's chords are covered by their own test.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"
make -C "$ROOT" tools BUILD="$BUILD" > /dev/null 2>&1
require_bins vtdump

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# The split pane runs $SHELL as seen by the DAEMON; pin it so the new pane's
# prompt is /bin/sh's predictable one rather than the developer's zsh setup.
SHELL=/bin/sh "$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"
require_alive "$DPID" "daemon"

SOCK="$TMP/.agent-terminal/run/default.sock"
[ -S "$SOCK" ] || fail "daemon socket $SOCK missing"

python3 - "$SOCK" "$TMP" > "$TMP/probe.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/probe.out")"
import socket, struct, sys, time

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
    payload = read_exactly(s, plen) if plen else b''
    return typ, payload

def connect(caps=1):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.sendall(frame(0x01, struct.pack('<HH', 1, caps)))
    typ, payload = read_frame(s)
    assert typ == 0x02, 'expected HELLO_OK, got %r' % typ
    # server_flags is an additive append at offset 10
    assert len(payload) >= 12, 'HELLO_OK missing server_flags'
    assert struct.unpack('<H', payload[10:12])[0] & 1, 'daemon does not claim panes'
    return s

def new_session(s, name, argv, cols=80, rows=24):
    nb = name.encode()
    ab = b'\x00'.join(a.encode() for a in argv) + b'\x00'
    s.sendall(frame(0x12, struct.pack('<HHB', cols, rows, len(nb)) + nb +
                    struct.pack('<H', len(ab)) + ab))

def pump(s, dur, sink=None, layouts=None):
    """Read frames for `dur` seconds; MSG_OUTPUT/SNAPSHOT bytes -> sink."""
    end = time.time() + dur
    s.settimeout(0.2)
    while time.time() < end:
        try:
            typ, payload = read_frame(s)
        except socket.timeout:
            continue
        if typ is None:
            break
        if sink is not None and typ == 0x30:
            sink.write(payload)
        if sink is not None and typ == 0x31:
            sink.write(payload[12:])
        if layouts is not None and typ == 0x35:
            layouts.append(payload)
    s.settimeout(10)

cap = open(tmp + '/capture.bin', 'wb')
s = connect()

# Two distinct children under one session name: pane 0 prints its marker and
# a shell stays alive; the split pane runs $SHELL (the daemon's choice).
new_session(s, 'pz', ['/bin/sh', '-c', 'printf LEFT-MARKER; exec sleep 300'])
pump(s, 0.6, cap)

# Split side-by-side (stacked=0), target=255 (active).
layouts = []
s.sendall(frame(0x16, bytes([0, 255])))
pump(s, 1.0, cap, layouts)
assert layouts, 'no MSG_LAYOUT after split'
lay = layouts[-1]
vc, vr, active, npanes = struct.unpack('<HHBB', lay[:6])
assert npanes == 2, 'expected 2 panes, got %d' % npanes
panes = {}
off = 6
for _ in range(npanes):
    pid_, x, y, c, r = struct.unpack('<BHHHH', lay[off:off+9])
    panes[pid_] = (x, y, c, r)
    off += 9
assert 0 in panes, 'pane 0 missing from layout'
x0, y0, c0, r0 = panes[0]
new_id = [k for k in panes if k != 0][0]
x1, y1, c1, r1 = panes[new_id]
assert x0 == 0 and x1 == c0 + 1, 'side-by-side geometry wrong: %r' % panes
assert active == new_id, 'new pane should be active'
print('layout: pane0=%r pane%d=%r active=%d' % (panes[0], new_id, panes[new_id], new_id))

# Type into the ACTIVE (new) pane; its shell echoes into its own rectangle.
s.sendall(frame(0x20, b'echo RIGHT-MARKER\n'))
pump(s, 2.5, cap)

# The composite must place LEFT-MARKER at pane 0's origin and RIGHT-MARKER
# inside the new pane's rectangle — asserted on the vtdump grid by the shell
# below, via these coordinates.
open(tmp + '/coords.txt', 'w').write('%d %d %d %d %d %d %d %d\n'
    % (x0, y0, c0, r0, x1, y1, c1, r1))

# Close the active pane; survivor grows back to the full view. Captured to a
# SEPARATE file: the leave-composite transition repaints with \x1b[2J, which
# would wipe RIGHT-MARKER out of the split-phase grid we assert below.
cap.close()
cap2 = open(tmp + '/capture_close.bin', 'wb')
layouts2 = []
s.sendall(frame(0x17, bytes([255])))
pump(s, 1.0, cap2, layouts2)
assert layouts2, 'no MSG_LAYOUT after close'
lay2 = layouts2[-1]
vc2, vr2, active2, npanes2 = struct.unpack('<HHBB', lay2[:6])
assert npanes2 == 1 and active2 == 0, 'close did not return to pane 0: %d panes active %d' % (npanes2, active2)
p0 = struct.unpack('<BHHHH', lay2[6:15])
assert p0[3] == vc2 and p0[4] == vr2, 'survivor did not grow to full view: %r' % (p0,)

# Reattach with a fresh client: the snapshot is a plain single-pane repaint
# again (raw path), and the session still answers.
cap2.close()
s.close()
s2 = connect()
s2.sendall(frame(0x14, struct.pack('<HHBB', 80, 24, 0, 2)) + b'')
# ^ MSG_ATTACH payload: u16 cols, u16 rows, u8 pane_id, u8 nlen, name
s2.close()
s3 = connect()
nb = b'pz'
s3.sendall(frame(0x14, struct.pack('<HHBB', 80, 24, 0, len(nb)) + nb))
typ, payload = read_frame(s3)
assert typ == 0x31, 'reattach: expected MSG_SNAPSHOT, got %r' % typ
snap = open(tmp + '/reattach.bin', 'wb')
snap.write(payload[12:])
pump(s3, 0.5, snap)
snap.close()
s3.close()
print('probe done')
PY

grep -q "probe done" "$TMP/probe.out" || fail "probe did not complete: $(cat "$TMP/probe.out")"

read -r X0 Y0 C0 R0 X1 Y1 C1 R1 < "$TMP/coords.txt"

# Render the captured composite stream and assert rectangle placement.
GRID=$("$BIN/vtdump" -r 24 -c 80 "$TMP/capture.bin")
echo "$GRID" > "$TMP/grid.txt"

# LEFT-MARKER starts at pane 0's origin (row Y0+1 of the dump, column X0).
LEFT_ROW=$(echo "$GRID" | sed -n "$((Y0 + 1))p")
case "$LEFT_ROW" in
    LEFT-MARKER*) ;;
    *) fail "pane 0 content not at its origin: '$LEFT_ROW'" ;;
esac

# RIGHT-MARKER appears somewhere inside the new pane's columns, and nowhere
# in pane 0's columns — writing to pane B must leave pane A's rectangle
# byte-unchanged.
echo "$GRID" | grep -q "RIGHT-MARKER" || fail "new pane's output never rendered"
ROW=$(echo "$GRID" | grep -n "RIGHT-MARKER" | head -1 | cut -d: -f1)
COL=$(echo "$GRID" | sed -n "${ROW}p" | awk -v m="RIGHT-MARKER" '{print index($0, m)}')
[ "$COL" -gt "$((X1))" ] || fail "RIGHT-MARKER at column $COL is left of pane boundary $X1"

# Pane 0's rectangle must contain no RIGHT-MARKER fragment.
if echo "$GRID" | cut -c1-"$C0" | grep -q "RIGHT-MARKER"; then
    fail "active pane's output bled into pane 0's rectangle"
fi

# The divider column renders between the panes on the marker row.
DIV_COL=$((C0 + 1))
DIV_CHAR=$(echo "$GRID" | sed -n "$((Y0 + 1))p" | cut -c"$DIV_COL")
[ "$DIV_CHAR" = "│" ] || fail "no divider at column $DIV_COL on row $((Y0+1)): got '$DIV_CHAR'"

# After the close, a fresh attach renders pane 0's content at full width.
GRID2=$("$BIN/vtdump" -r 24 -c 80 "$TMP/reattach.bin")
echo "$GRID2" | head -1 | grep -q "LEFT-MARKER" || fail "reattach after close lost pane 0's content"
echo "$GRID2" | grep -q "│" && fail "divider survived the close in the reattach snapshot"

# The daemon's own log confirms two distinct pids existed under one name.
PIDS=$(grep -oE "session 'pz'.*pid [0-9]+" "$TMP/daemon.log" | grep -oE "[0-9]+$" | sort -u | wc -l | tr -d ' ')
[ "$PIDS" -ge 2 ] || fail "expected 2 distinct child pids, saw $PIDS"

require_alive "$DPID" "daemon"
echo "PASS: split placed two children in correct rectangles with a divider; input isolated; close restored full view"
