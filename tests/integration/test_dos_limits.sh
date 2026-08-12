#!/usr/bin/env bash
# S2: one same-uid client must not be able to deny service to the user's real
# sessions. The uid is this daemon's entire trust boundary — any process running
# as the user may connect and is fully authorized — so "authenticate harder" is
# not available as a defense. What is available is refusing to let a single
# client consume an unbounded share of memory or of the client slot table.
#
# Part 1 — geometry amplification. libvt clamps its own grid to 1000x1000, so the
# engine allocation was never the problem. session/pane kept the raw u16 the
# client sent, and the compositor pads every row out to pane.cols while drawing
# only the cells the engine holds (composite.c emit_pane_row): at cols=65535 that
# is ~64 KiB of spaces per row, ~64 MB per frame, rebuilt on the 20 ms tick for
# as long as any pane is dirty. Two properties make it worse than a big
# allocation: it needs no privilege beyond connecting, and it SURVIVES the
# attacking client disconnecting, because session_composite_all does not require
# an attached client. Measured pre-fix: peak RSS 164 MiB and `ls` reporting
# `65535x65535, 0 clients, 2 panes`; post-fix 62 MiB and `1000x1000`.
#
# Part 2 — client slot exhaustion. PRE_HELLO_BUDGET caps how many BYTES a client
# may send before HELLO, which does nothing about a client that sends none: it
# spends no budget and holds its slot forever. At MAX_CLIENTS=32 such peers
# server_accept has no free slot and closes every real client immediately, so one
# process with 32 idle sockets locks the user out of their own sessions. The fix
# is a wall-clock HELLO deadline, necessarily driven by the daemon tick — a
# silent peer generates no readable event, so nothing in the read path can ever
# notice it.
#
# The primary assertions are the daemon's OWN reported state (`ls` geometry, a
# completed HELLO handshake), not RSS: a resident-set threshold is a proxy that
# has to be tuned per machine, while the reported geometry is exact and fails by
# 65x on the unfixed build. RSS is kept as a weaker second net at the ceiling
# test_soak.sh already proves portable.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
HOLDER=""
cleanup() {
    [ -n "$HOLDER" ] && kill "$HOLDER" 2>/dev/null
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

# ---- part 1: hostile geometry is clamped, legitimate geometry is not ---------
#
# Both sessions are driven over the wire rather than through the client, because
# the client screens its own geometry from the tty and cannot express 65535.
#
# HELLO_OK is read before anything else is sent. Not politeness: PRE_HELLO_BUDGET
# caps pre-HELLO input at 64 bytes, and HELLO + NEW_SESSION together exceed that
# when they arrive in one read, so a pipelined attacker is disconnected before it
# attacks anything. Getting this wrong made the probe report a clean daemon.
cat > "$TMP/geom.py" <<'PY'
import socket, struct, sys, time

sock_path, name, cols, rows = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])

def frame(t, payload):
    return struct.pack('<IB', len(payload), t) + payload

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sock_path)
s.sendall(frame(0x01, struct.pack('<HH', 1, 1)))   # HELLO, CLIENT_CAP_PANES

def read_exactly(n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise SystemExit('daemon closed the connection before HELLO_OK')
        b += c
    return b

plen, typ = struct.unpack('<IB', read_exactly(5))
read_exactly(plen)
if typ != 0x02:
    raise SystemExit('expected HELLO_OK (0x02), got 0x%02x' % typ)

# A child that keeps printing keeps a pane dirty, so the compositor keeps
# rebuilding the frame; a one-shot command would make the cost a single
# transient spike that an RSS sampler could miss between two ticks.
nb = name.encode()
argv = b'/bin/sh\x00-c\x00while :; do echo xxxxxxxxxxxxxxxx; sleep 0.05; done\x00'
s.sendall(frame(0x12, struct.pack('<HHB', cols, rows, len(nb)) + nb +
                struct.pack('<H', len(argv)) + argv))
s.sendall(frame(0x16, struct.pack('<BB', 0, 255)))  # SPLIT_PANE -> 2 panes

# Drain briefly so the daemon is not disconnecting us for backlog while it
# processes the split, then leave: the session must keep compositing with zero
# clients attached, which is what makes this outlive the attacker.
deadline = time.time() + 1.5
try:
    while time.time() < deadline:
        if not s.recv(1 << 20):
            break
except (socket.timeout, ConnectionResetError, BrokenPipeError):
    pass
s.close()
print('PROBE-DONE %s' % name)
PY

python3 "$TMP/geom.py" "$SOCK" huge 65535 65535 > "$TMP/huge.out" 2>&1 \
    || fail "hostile-geometry probe crashed: $(cat "$TMP/huge.out")"
grep -q "PROBE-DONE huge" "$TMP/huge.out" \
    || fail "hostile probe never completed, so its silence proves nothing: $(cat "$TMP/huge.out")"

# Negative control, same code path with a geometry a real terminal could have.
# Without it, a clamp that flattened every session to 1000x1000 — or to 1x1 —
# would pass the assertion below just as well.
python3 "$TMP/geom.py" "$SOCK" normal 200 50 > "$TMP/normal.out" 2>&1 \
    || fail "control-geometry probe crashed: $(cat "$TMP/normal.out")"
grep -q "PROBE-DONE normal" "$TMP/normal.out" \
    || fail "control probe never completed: $(cat "$TMP/normal.out")"

PEAK_RSS=0
for _ in $(seq 1 40); do
    RSS=$(ps -o rss= -p "$DPID" 2>/dev/null | tr -d ' ')
    [ -z "$RSS" ] && fail "daemon died while compositing the oversized session"
    [ "$RSS" -gt "$PEAK_RSS" ] && PEAK_RSS=$RSS
    sleep 0.1
done

"$BIN/agent-terminal" ls > "$TMP/ls.out" 2>&1 || fail "ls failed: $(cat "$TMP/ls.out")"

# The daemon's own report of what it accepted. VT_COLS_MAX/VT_ROWS_MAX are 1000.
grep -q "^huge: 1000x1000," "$TMP/ls.out" \
    || fail "oversized geometry was not clamped to 1000x1000: $(grep '^huge:' "$TMP/ls.out")"
grep -q "65535" "$TMP/ls.out" \
    && fail "a raw 65535 survived into the session list: $(cat "$TMP/ls.out")"
grep -q "^normal: 200x50," "$TMP/ls.out" \
    || fail "a legitimate 200x50 session was altered, so the clamp is not discriminating: $(grep '^normal:' "$TMP/ls.out")"

# Confirms the sessions really are compositing (2 panes) with nobody attached —
# the property that makes the pre-fix cost unstoppable by disconnecting.
grep -q "^huge: 1000x1000, pid [0-9]*, 0 clients, 2 panes" "$TMP/ls.out" \
    || fail "expected the oversized session to be composited with 0 clients: $(grep '^huge:' "$TMP/ls.out")"

# Second net only. 128 MiB is test_soak.sh's already-portable ceiling; the
# unfixed daemon measured 164 MiB here, the fixed one 62 MiB.
PEAK_MB=$((PEAK_RSS / 1024))
[ "$PEAK_MB" -le 128 ] || fail "daemon RSS peaked at ${PEAK_MB} MiB (>128) on two oversized sessions"

# ---- part 2: silent connections cannot hold the client slot table -----------
#
# MAX_CLIENTS is 32 (server.c). 40 connections, so the table is exhausted even
# if a straggler from part 1 has not been reaped yet.
cat > "$TMP/silent.py" <<'PY'
import os, socket, sys, time
sock_path, n, ready, release = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
socks = []
for _ in range(n):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)          # and not one byte is ever sent
    socks.append(s)
with open(ready, 'w') as f:
    f.write('%d\n' % len(socks))
# Exit on the parent's word rather than on a signal, so the shell has no killed
# background job to report and the test output stays readable.
for _ in range(600):
    if os.path.exists(release):
        break
    time.sleep(0.1)
PY

# Does a fresh client get through the HELLO handshake? Returns 0 = yes,
# 1 = the daemon closed on us, 2 = no answer at all.
cat > "$TMP/hello.py" <<'PY'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(float(sys.argv[2]))
try:
    s.connect(sys.argv[1])
    s.sendall(struct.pack('<IB', 4, 0x01) + struct.pack('<HH', 1, 0))
    hdr = b''
    while len(hdr) < 5:
        c = s.recv(5 - len(hdr))
        if not c:
            print('CLOSED')
            raise SystemExit(1)
        hdr += c
    plen, typ = struct.unpack('<IB', hdr)
    print('HELLO_OK' if typ == 0x02 else 'TYPE 0x%02x' % typ)
    raise SystemExit(0 if typ == 0x02 else 1)
except socket.timeout:
    print('TIMEOUT')
    raise SystemExit(2)
except (ConnectionRefusedError, ConnectionResetError, BrokenPipeError) as e:
    print('REFUSED %s' % e)
    raise SystemExit(1)
PY

python3 "$TMP/silent.py" "$SOCK" 40 "$TMP/silent.ready" "$TMP/silent.release" > "$TMP/silent.out" 2>&1 &
HOLDER=$!
# The count, not merely the file: a holder that connected 3 times and then hit
# an error would still have created the file.
wait_for "^40$" "$TMP/silent.ready" 5 \
    || fail "silent-connection holder never reported 40 open connections: $(cat "$TMP/silent.ready" 2>/dev/null) / $(cat "$TMP/silent.out" 2>/dev/null)"
require_alive "$HOLDER" "silent-connection holder"

# Positive control: while the slots are held the denial must be REAL. Without
# this, a HELLO_DEADLINE_MS so short that it fired before we ever measured — or a
# flood that silently failed to connect — would make the recovery check below
# pass against a daemon that was never under attack. The window is ~5 s and this
# probe takes milliseconds, so it is not a tight race.
python3 "$TMP/hello.py" "$SOCK" 3 > "$TMP/denied.out" 2>&1
DENIED_RC=$?
[ "$DENIED_RC" -ne 0 ] \
    || fail "40 silent connections did NOT exhaust the client table (got $(cat "$TMP/denied.out")), so this test cannot show the deadline recovers anything"

# Now past HELLO_DEADLINE_MS (5000) plus tick slack. Polled rather than slept
# once so a slow machine reports the recovery it achieved, not a timeout.
RECOVERED=""
for _ in $(seq 1 40); do
    sleep 0.5
    if python3 "$TMP/hello.py" "$SOCK" 3 > "$TMP/recover.out" 2>&1; then
        RECOVERED=yes
        break
    fi
done
[ -n "$RECOVERED" ] \
    || fail "no client could complete a HELLO within 20 s of 40 silent connections: last=$(cat "$TMP/recover.out")"

grep -q "sent no HELLO in 5000 ms, dropping" "$TMP/daemon.log" \
    || fail "the daemon never logged a HELLO-deadline drop, so recovery came from something other than the reap: $(tail -5 "$TMP/daemon.log")"

touch "$TMP/silent.release"
wait "$HOLDER" || fail "silent-connection holder exited non-zero: $(cat "$TMP/silent.out")"
HOLDER=""

# Negative control for the reap's scope: a client that DID say HELLO and then
# went quiet is a user staring at their terminal, and must never be dropped.
# This is the assertion that separates "reap unauthenticated clients" from
# "reap idle clients", and only a wall-clock wait longer than the deadline can
# make it.
cat > "$TMP/idle.py" <<'PY'
import socket, struct, sys, time

def frame(t, payload):
    return struct.pack('<IB', len(payload), t) + payload

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sys.argv[1])
s.sendall(frame(0x01, struct.pack('<HH', 1, 0)))

def read_frame():
    hdr = b''
    while len(hdr) < 5:
        c = s.recv(5 - len(hdr))
        if not c:
            raise SystemExit('daemon closed the connection')
        hdr += c
    plen, typ = struct.unpack('<IB', hdr)
    body = b''
    while len(body) < plen:
        c = s.recv(plen - len(body))
        if not c:
            raise SystemExit('daemon closed mid-frame')
        body += c
    return typ, body

typ, _ = read_frame()
if typ != 0x02:
    raise SystemExit('expected HELLO_OK, got 0x%02x' % typ)

time.sleep(float(sys.argv[2]))          # well past HELLO_DEADLINE_MS
s.sendall(frame(0x40, struct.pack('<Q', 0xC0FFEE)))   # MSG_PING
typ, body = read_frame()
if typ != 0x41:
    raise SystemExit('expected MSG_PONG (0x41) after idling, got 0x%02x' % typ)
print('IDLE-CLIENT-SURVIVED')
PY

python3 "$TMP/idle.py" "$SOCK" 7 > "$TMP/idle.out" 2>&1 \
    || fail "a client that completed HELLO was dropped while idle: $(cat "$TMP/idle.out")"
grep -q "IDLE-CLIENT-SURVIVED" "$TMP/idle.out" \
    || fail "idle-client control did not run: $(cat "$TMP/idle.out")"

require_alive "$DPID" "daemon"
echo "PASS: dos limits — 65535x65535 clamped to 1000x1000 (200x50 preserved), peak RSS ${PEAK_MB} MiB, 40 silent connections reaped and a real HELLO recovered, HELLO'd idle client kept"
