#!/usr/bin/env bash
# MSG_PANE_BELL (0x38): a bell inside a SPLIT session must reach clients as an
# attributed event, because composite frames are rebuilt from grid state and
# the raw \a never survives them (protocol-notes.md trap #1 — until now the
# bell just vanished). Four properties, each aimed at a specific wrong
# implementation:
#
#   1. ≥2 panes + BEL in a NON-ZERO pane → 0x38 with THAT pane's id, to a
#      CLIENT_CAP_PANES client. (A hardcoded id 0 fails here.)
#   2. A client withOUT the capability never receives 0x38. (A missing cap
#      gate fails here.)
#   3. ONE pane → no 0x38 at all; the raw \a arrives inside MSG_OUTPUT
#      instead. (An implementation that always sends would ring twice —
#      fails here.)
#   4. The BEL that terminates an OSC title write must NOT ring — in a split
#      session, where the only bell path is the callback. (An implementation
#      scanning raw output bytes for 0x07 fails here.)
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    # Kill by tracked pid, never by name — a broad pkill hits the production
    # daemon on this machine (it has).
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

SHELL=/bin/sh "$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"

python3 - "$TMP/.agent-terminal/run/default.sock" <<'PY' || fail "wire probe failed"
import socket, struct, sys, time

sock_path = sys.argv[1]

def frame(t, payload=b''):
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
    return typ, (read_exactly(s, plen) if plen else b'')

def connect(flags):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.sendall(frame(0x01, struct.pack('<HH', 1, flags)))
    typ, payload = read_frame(s)
    assert typ == 0x02, f"expected HELLO_OK, got {typ:#x}"
    return s

def new_session(s, name, cols=80, rows=24):
    nb = name.encode()
    argv = b''  # default command ($SHELL = /bin/sh)
    s.sendall(frame(0x12, struct.pack('<HHB', cols, rows, len(nb)) + nb
                          + struct.pack('<H', len(argv)) + argv))

def attach(s, name, pane_id=0, cols=80, rows=24):
    nb = name.encode()
    s.sendall(frame(0x14, struct.pack('<HHBB', cols, rows, pane_id, len(nb)) + nb))

def collect(s, seconds):
    """Drain frames for a fixed window; returns list of (type, payload)."""
    out = []
    end = time.time() + seconds
    s.settimeout(0.2)
    while time.time() < end:
        try:
            typ, payload = read_frame(s)
        except socket.timeout:
            continue
        if typ is None:
            break
        out.append((typ, payload))
    s.settimeout(10)
    return out

def wait_for_type(s, want, seconds, forbid=None):
    """Read until a frame of type `want` arrives; assert `forbid` never does."""
    end = time.time() + seconds
    s.settimeout(0.2)
    got = None
    while time.time() < end:
        try:
            typ, payload = read_frame(s)
        except socket.timeout:
            continue
        if typ is None:
            break
        assert forbid is None or typ != forbid, f"forbidden frame {typ:#x} arrived"
        if typ == want:
            got = (typ, payload)
            break
    s.settimeout(10)
    return got

BELL = 0x38

# --- setup: one capable driver, one capable listener, one incapable listener
# (MSG_LAYOUT is sent on layout CHANGES, so a fresh one-pane session yields a
# snapshot, not a layout — the first layout arrives with the split below.)
drv = connect(flags=1)             # CLIENT_CAP_PANES
new_session(drv, 'bells')
assert wait_for_type(drv, 0x31, 5), "no snapshot after create"

cap = connect(flags=1)
attach(cap, 'bells')
assert wait_for_type(cap, 0x31, 5), "capable listener never got a snapshot"

nocap = connect(flags=0)           # pre-pane client
attach(nocap, 'bells')
assert wait_for_type(nocap, 0x31, 5), "incapable listener never got a snapshot"

# --- split, then find the NEW pane's id (non-zero by construction: pane 0 is
#     permanently the first pane, splits draw 1..254)
drv.sendall(frame(0x16, bytes([0, 255])))   # stacked=0, target=active
time.sleep(0.5)
lay = wait_for_type(drv, 0x35, 5)
assert lay, "no layout after split"
payload = lay[1]
npanes = payload[5]
assert npanes == 2, f"expected 2 panes, layout says {npanes}"
ids = [payload[6 + i * 9] for i in range(npanes)]
new_id = [i for i in ids if i != 0][0]
active = payload[4]
assert active == new_id, f"split should focus the new pane (active={active}, new={new_id})"

# drain the split-triggered composite noise before asserting on bells
collect(drv, 0.8); collect(cap, 0.8); collect(nocap, 0.8)

# --- 4: OSC-terminating BEL must not ring (checked FIRST, so a raw-0x07
#     scanner has its chance to misfire before the real bell arrives)
drv.sendall(frame(0x20, b"printf '\\033]0;quiet\\a'\n"))
frames = collect(cap, 1.5)
bells = [f for f in frames if f[0] == BELL]
assert not bells, f"OSC terminator rang the bell: {bells}"
print("ok: the BEL that terminates an OSC title write does not ring")

# --- 1: a real BEL in the split (active = new, non-zero pane) reaches the
#     capable listener with the right pane id, exactly one byte of payload
drv.sendall(frame(0x20, b"printf '\\a'\n"))
got = wait_for_type(cap, BELL, 5)
assert got, "no MSG_PANE_BELL for a real BEL in a split session"
assert len(got[1]) == 1, f"payload must be exactly u8 pane_id, got {len(got[1])} bytes"
assert got[1][0] == new_id, f"bell attributed to pane {got[1][0]}, rang in {new_id}"
print(f"ok: split-session bell arrives as 0x38 with pane_id={new_id}")

# the driver (also capable) got it too — broadcast, not reply
assert wait_for_type(drv, BELL, 3), "driver client missed the broadcast bell"
print("ok: every capable client gets the broadcast")

# --- 2: the incapable client saw composite output but never 0x38
frames = collect(nocap, 1.0)
assert frames, "incapable client went silent (test would be vacuous)"
assert not [f for f in frames if f[0] == BELL], "0x38 leaked to a client without CLIENT_CAP_PANES"
print("ok: no bell frame for a client that never claimed the capability")

# --- 3: back to ONE pane -> no 0x38; the raw \a rides MSG_OUTPUT
drv.sendall(frame(0x17, bytes([new_id])))   # close the new pane
time.sleep(0.5)
collect(drv, 0.8); collect(cap, 0.8)
drv.sendall(frame(0x20, b"printf 'X\\aY'\n"))
end = time.time() + 5
saw_raw = False
cap.settimeout(0.2)
while time.time() < end and not saw_raw:
    try:
        typ, payload = read_frame(cap)
    except socket.timeout:
        continue
    if typ is None:
        break
    assert typ != BELL, "single-pane session sent 0x38 — the terminal would ring twice"
    if typ == 0x30 and b'\x07' in payload:
        saw_raw = True
assert saw_raw, "single-pane raw \\a never arrived in MSG_OUTPUT (control failed)"
print("ok: one pane means raw \\a passthrough and no 0x38")
PY

echo "PASS: pane bells are attributed, capability-gated, and never double-ring"
