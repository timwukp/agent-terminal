#!/usr/bin/env bash
# A reload must keep the scrollback a client can READ OVER THE WIRE, not just
# the copy on disk.
#
# Two existing tests look adjacent to this and neither can fail for it:
#   - test_restart.sh asserts history survives a restart with `agent-terminal
#     history`, which reads the on-disk log directly;
#   - test_pager_ring.sh reads copy-mode, and copy-mode ALSO loads the log
#     locally (pager.c:164 sb_read_log) and uses MSG_SCROLLBACK_REQ only for the
#     un-flushed tail.
# So both passed for two releases while the daemon served nothing over the wire.
# A client with no filesystem access — the GUI — got an empty scrollbar: across
# a re-exec the in-memory ring was rebuilt EMPTY while the attach snapshot still
# announced the whole history in sb_lines, and MSG_SCROLLBACK_REQ can only read
# that ring. Reported by a user of v0.1.0 as "I scroll up in the GUI and my
# whole conversation is gone", with 18 MB of it in the log. Measured on an
# isolated daemon before the fix: 2977 lines announced, 0 servable.
#
# This test therefore drives the wire path ONLY, with a client that never opens
# the log — the same shape as the protocol drivers in test_dos_limits.sh.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    exec 3>&- 2>/dev/null
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# The wire-only client: HELLO, ATTACH, one MSG_SCROLLBACK_REQ. It reports what
# the snapshot ANNOUNCED (sb_lines) beside what the daemon actually SERVED,
# because the bug is exactly a disagreement between those two numbers.
cat > "$TMP/sbwire.py" <<'PY'
import socket, struct, sys, time
sock, name, start, maxn = sys.argv[1], sys.argv[2].encode(), int(sys.argv[3]), int(sys.argv[4])
HELLO, HELLO_OK, ATTACH, SNAPSHOT, SB_REQ, SB_DATA = 0x01, 0x02, 0x14, 0x31, 0x32, 0x33
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(10.0); s.connect(sock)
buf = b""
def rd(deadline):
    global buf
    while True:
        if len(buf) >= 5:
            (n,) = struct.unpack("<I", buf[:4]); t = buf[4]
            if len(buf) >= 5 + n:
                p = buf[5:5+n]; buf = buf[5+n:]; return t, p
        left = deadline - time.time()
        if left <= 0: return None, None
        s.settimeout(left)
        try: chunk = s.recv(65536)
        except socket.timeout: return None, None
        if not chunk: return None, None
        buf += chunk
def frame(t, p=b""): return struct.pack("<IB", len(p), t) + p
s.sendall(frame(HELLO, struct.pack("<HH", 1, 0)))
t, p = rd(time.time() + 10)
assert t == HELLO_OK, ("no HELLO_OK", t)
s.sendall(frame(ATTACH, struct.pack("<HHBB", 80, 24, 255, len(name)) + name))
announced = -1
dl = time.time() + 4
while True:
    t, p = rd(dl)
    if t is None: break
    if t == SNAPSHOT: announced = struct.unpack("<Q", p[4:12])[0]
s.sendall(frame(SB_REQ, struct.pack("<QI", start, maxn)))
first = served = -1
lines = []
dl = time.time() + 10
while True:
    t, p = rd(dl)
    if t is None: break
    if t == SB_DATA:
        first, served = struct.unpack("<QI", p[:12])
        off = 12
        for _ in range(served):
            (ln,) = struct.unpack("<I", p[off:off+4]); off += 4
            lines.append(p[off:off+ln].decode("utf-8", "replace")); off += ln
        break
print(f"announced={announced} served={served} first_seq={first}")
for l in lines: print("LINE:" + l)
PY

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 || fail "daemon did not start"
SOCK="$TMP/.agent-terminal/run/default.sock"

mkfifo "$TMP/in1"
# 100 lines through a 24-row screen: 77 scroll off into scrollback, 23 remain.
"$BIN/agent-terminal" new -s sbr -- bash --norc -c '
    for i in $(seq 1 100); do echo "RELOADSB-$i"; done
    exec sleep 300
' < "$TMP/in1" > "$TMP/out1" 2>&1 &
exec 3>"$TMP/in1"
wait_for "RELOADSB-100" "$TMP/out1" 10 || fail "child output never reached the first client"
sleep 1.4   # > the 1 s flush tick, so the reload's own flush is not the variable

# Premise: before the reload the wire really does serve those lines. Without
# this, a post-reload assertion could pass against a daemon that never had them.
python3 "$TMP/sbwire.py" "$SOCK" sbr 0 1000 > "$TMP/before.out" 2>&1 \
    || fail "wire client failed before the reload: $(cat "$TMP/before.out")"
BEFORE_SERVED="$(sed -n 's/.*served=\([0-9-]*\).*/\1/p' "$TMP/before.out")"
[ "${BEFORE_SERVED:-0}" -eq 77 ] \
    || fail "pre-reload wire scrollback served $BEFORE_SERVED lines, expected 77 ($(head -1 "$TMP/before.out"))"

RELOAD_OUT="$("$BIN/agent-terminal" reload 2>&1)" || fail "reload exited nonzero: $RELOAD_OUT"
echo "$RELOAD_OUT" | grep -q "generation 1" \
    || fail "reload did not report generation 1 (got: $RELOAD_OUT)"
grep -q "reloaded on" "$TMP/daemon.log" || fail "reload reported success but the daemon never re-execed"

# The reload disconnects every client (server_prepare_handoff), so this is a
# fresh attach — exactly what the GUI does when it reconnects.
python3 "$TMP/sbwire.py" "$SOCK" sbr 0 1000 > "$TMP/after.out" 2>&1 \
    || fail "wire client failed after the reload: $(cat "$TMP/after.out")"
HEAD="$(head -1 "$TMP/after.out")"
AFTER_SERVED="$(sed -n 's/.*served=\([0-9-]*\).*/\1/p' "$TMP/after.out")"
ANNOUNCED="$(sed -n 's/.*announced=\([0-9-]*\).*/\1/p' "$TMP/after.out")"

# The whole defect in one assertion: the snapshot announced history the daemon
# could not serve. Both numbers, so a fix that silenced the announcement instead
# of restoring the lines would not pass either.
[ "${ANNOUNCED:-0}" -eq 77 ] || fail "post-reload snapshot announced $ANNOUNCED lines, expected 77 ($HEAD)"
[ "${AFTER_SERVED:-0}" -eq 77 ] \
    || fail "after the reload the daemon served $AFTER_SERVED of the $ANNOUNCED lines it announced — the ring was rebuilt empty ($HEAD)"

# Then the bytes, because a count can be right while the lines are not: the
# oldest and the newest of the window must both survive, in order.
grep -q "^LINE:.*RELOADSB-1 *$" "$TMP/after.out" \
    || fail "the oldest scrolled-off line (RELOADSB-1) is missing after the reload"
grep -q "^LINE:.*RELOADSB-77" "$TMP/after.out" \
    || fail "the newest scrolled-off line (RELOADSB-77) is missing after the reload"
FIRST_LINE="$(grep -m1 '^LINE:' "$TMP/after.out")"
case "$FIRST_LINE" in
    *RELOADSB-1*) : ;;
    *) fail "post-reload scrollback is out of order; first line served was: $FIRST_LINE" ;;
esac

printf '\x1c\x04' >&3 2>/dev/null || true
exec 3>&-
"$BIN/agent-terminal" kill -s sbr > /dev/null 2>&1

echo "PASS: reload preserves the scrollback clients can read over the wire ($HEAD, oldest and newest present, in order)"
