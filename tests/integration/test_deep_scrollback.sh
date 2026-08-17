#!/usr/bin/env bash
# Scrollback OLDER than the in-memory ring must be servable over the wire.
#
# test_reload_scrollback.sh covers the ring being rebuilt empty across a reload.
# This covers the other half of the same user report: the ring holds
# SB_MEM_LINES_DEFAULT (10,000) lines, the attach snapshot announces
# sb_total_lines() — the WHOLE log, which is 93,374 lines on this machine's
# `claude` session — and everything between the two was announced and then
# unservable. A GUI that scrolls past 10,000 lines saw its history end there
# while 18 MB of it sat in the log.
#
# So the test has to actually EXCEED the default ring, which means the daemon's
# own default (session.c passes mem_lines = 0) and >10,000 real lines; a smaller
# fixture cannot fail for this. Pre-change, a request from seq 0 answers the
# RING's oldest line (~seq 977 here) with a full, valid-looking page of 1000
# lines — so the assertion that matters is the FIRST SEQ, not the count.
#
# Wire-only for the same reason as test_reload_scrollback.sh: `history` and
# copy-mode both read the log file directly (pager.c:164), so neither can observe
# what MSG_SCROLLBACK_REQ serves.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

LINES=11000          # > SB_MEM_LINES_DEFAULT, with room to spare
ROWS=24              # so LINES - (ROWS - 1) scroll off into the scrollback
EXPECT_TOTAL=$((LINES - ROWS + 1))   # 10,977 announced
PAGE=1000

DPID=""
cleanup() {
    exec 3>&- 2>/dev/null
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# Same wire-only client shape as test_reload_scrollback.sh: HELLO, ATTACH, one
# MSG_SCROLLBACK_REQ. Reports announced beside served and the first seq.
#
# Readiness is deliberately NOT taken by staying attached through the burst.
# Two measured reasons: waiting for 11,000 lines to appear in an attached C
# client's output file makes it repaint ~478 screens into that file while the
# poll greps a growing multi-megabyte file every 100 ms (2m50s for this test);
# and a wire client attached during the burst falls behind the 20 ms composite
# tick, overflows its out-ring and is dropped by the daemon — seen as the
# readiness loop stopping at 9,426 or 10,474 of 10,977 on 2 of 3 runs. So the
# child announces its own completion in a file of its own, and the wire is asked
# only once output has stopped.
cat > "$TMP/sbwire.py" <<'PY'
import socket, struct, sys, time
sock, name, start, maxn = sys.argv[1], sys.argv[2].encode(), int(sys.argv[3]), int(sys.argv[4])
want_total = int(sys.argv[5])
HELLO, HELLO_OK, ATTACH, SNAPSHOT, SB_REQ, SB_DATA = 0x01, 0x02, 0x14, 0x31, 0x32, 0x33
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(20.0); s.connect(sock)
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
        try: chunk = s.recv(1 << 20)
        except socket.timeout: return None, None
        if not chunk: return None, None
        buf += chunk
def frame(t, p=b""): return struct.pack("<IB", len(p), t) + p
s.sendall(frame(HELLO, struct.pack("<HH", 1, 0)))
t, p = rd(time.time() + 10)
assert t == HELLO_OK, ("no HELLO_OK", t)
s.sendall(frame(ATTACH, struct.pack("<HHBB", 80, 24, 255, len(name)) + name))
announced = -1
dl = time.time() + 5
while True:
    t, p = rd(dl)
    if t is None: break
    if t == SNAPSHOT:
        announced = struct.unpack("<Q", p[4:12])[0]
        if announced >= want_total: break
s.sendall(frame(SB_REQ, struct.pack("<QI", start, maxn)))
first = served = -1
lines = []
dl = time.time() + 20
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
for l in lines[:3] + lines[-1:]: print("LINE:" + l)
PY

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 || fail "daemon did not start"
SOCK="$TMP/.agent-terminal/run/default.sock"

mkfifo "$TMP/in1"
# The C client's own rendering is not under test, so it repaints into /dev/null;
# readiness is taken from the wire below.
"$BIN/agent-terminal" new -s deepsb -- bash --norc -c "
    for i in \$(seq 1 $LINES); do echo \"DEEPSB-\$i\"; done
    echo PRODUCED > '$TMP/produced'
    exec sleep 300
" < "$TMP/in1" > /dev/null 2>&1 &
exec 3>"$TMP/in1"
# The child says when it is done writing; the daemon may still be draining the
# PTY, so the exact total is then waited for on the wire.
wait_for PRODUCED "$TMP/produced" 60 || fail "the child never finished printing $LINES lines"

for _ in $(seq 1 40); do
    python3 "$TMP/sbwire.py" "$SOCK" deepsb 0 "$PAGE" "$EXPECT_TOTAL" > "$TMP/deep.out" 2>&1 \
        || fail "wire client failed: $(cat "$TMP/deep.out")"
    [ "$(sed -n 's/.*announced=\([0-9-]*\).*/\1/p' "$TMP/deep.out")" -ge "$EXPECT_TOTAL" ] && break
    sleep 0.25
done
HEAD="$(head -1 "$TMP/deep.out")"
ANNOUNCED="$(sed -n 's/.*announced=\([0-9-]*\).*/\1/p' "$TMP/deep.out")"
SERVED="$(sed -n 's/.*served=\([0-9-]*\).*/\1/p' "$TMP/deep.out")"
FIRST="$(sed -n 's/.*first_seq=\([0-9-]*\).*/\1/p' "$TMP/deep.out")"

# The premise: the ring really was exceeded. Without this the test could pass on
# a fixture small enough that the ring answered everything.
[ "${ANNOUNCED:-0}" -eq "$EXPECT_TOTAL" ] \
    || fail "snapshot announced $ANNOUNCED lines, expected $EXPECT_TOTAL ($HEAD)"
[ "$EXPECT_TOTAL" -gt 10000 ] \
    || fail "fixture does not exceed SB_MEM_LINES_DEFAULT; the test cannot fail for the bug"

[ "${SERVED:-0}" -eq "$PAGE" ] || fail "served $SERVED of $PAGE requested ($HEAD)"
# The assertion that separates fixed from unfixed: pre-change this is the ring's
# oldest seq (LINES - ROWS + 1 - 10000 = 977), not 0.
[ "${FIRST:-1}" -eq 0 ] \
    || fail "requested seq 0 and got first_seq=$FIRST — the daemon served the RING's oldest line instead of the log's ($HEAD)"

# And the bytes, because the right count from the wrong offset is still wrong.
grep -q "^LINE:.*DEEPSB-1 *$" "$TMP/deep.out" \
    || fail "the log's first line (DEEPSB-1) is not what seq 0 served ($(grep -m1 '^LINE:' "$TMP/deep.out"))"
grep -q "^LINE:.*DEEPSB-$PAGE *$" "$TMP/deep.out" \
    || fail "the page's last line (DEEPSB-$PAGE) is missing; the page is not contiguous from seq 0"

printf '\x1c\x04' >&3 2>/dev/null || true
exec 3>&-
"$BIN/agent-terminal" kill -s deepsb > /dev/null 2>&1

echo "PASS: a page below the ring is served from the log ($HEAD, ring holds the newest 10000 of $ANNOUNCED)"
