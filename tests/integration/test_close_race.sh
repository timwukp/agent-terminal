#!/usr/bin/env bash
# A request already in the socket buffer must be honored even though the client
# has closed. POLLHUP means "the peer will send nothing more", not "discard what
# it already sent".
#
# Exists because client_io() checked POLLHUP before it read, and the kernel
# reports POLLIN|POLLHUP together when bytes are still queued on a closed
# socket. A client that wrote MSG_NEW_SESSION and closed at once had its request
# thrown away: the daemon created nothing, logged nothing, and — because the
# client had already stopped listening — nobody ever learned. `agent-terminal
# new ... < /dev/null` hits this every time, since attach.c treats an instant
# stdin EOF as a detach, so it printed "[detached — session 'x' keeps running]"
# and exited 0 for a session that did not exist.
#
# It surfaced as a 3-in-8 flake across test_lastscreen/test_history/
# test_reattach, always as a *later* assertion failing ("idle session lost its
# output", "expected 100 recovered lines, got 0"), which is why it read as
# flakiness in unrelated code for so long.
#
# Three probes, because they fail differently:
#   1. wire, close() immediately — the exact regression; pre-fix 20/20 lost
#   2. wire, shutdown(SHUT_WR) only — half-open peer, must behave identically
#   3. the real client with stdin at /dev/null — the user-visible path, and the
#      only one that also checks the client does not lie about the outcome
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
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

# -v so every accepted session logs "session '<name>': pid N" — that line is the
# ground truth here. The client's own exit code cannot be, since reporting a
# success the daemon never performed is the bug under test.
"$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"
require_alive "$DPID" "daemon"

SOCK="$TMP/.agent-terminal/run/default.sock"
[ -S "$SOCK" ] || fail "daemon socket $SOCK missing"

# 3*N sessions are created and none are killed until cleanup, so N is bounded by
# MAX_SESSIONS (64 in session.h). 15 leaves headroom; the pre-fix failure rate
# was 20 of 20, so this is not a sample size that needs to be large.
N=15

# --- 1 & 2. over the wire, bypassing the client -------------------------------
# [u32 payload_len][u8 type][payload], LE. HELLO 0x01: u16 ver, u16 flags.
# NEW_SESSION 0x12: u16 cols, u16 rows, u8 nlen, name, u16 argv_bytes, argv.
#
# No read of the reply and no sleep before the teardown — waiting for the reply
# would serialize against the daemon and hide the race, which is the whole point.
python3 - "$SOCK" "$N" > "$TMP/wire.out" 2>"$TMP/wire.err" <<'PY' \
    || fail "wire probe crashed: $(cat "$TMP/wire.err")"
import socket, struct, sys

def frame(t, payload):
    return struct.pack('<IB', len(payload), t) + payload

def read_exactly(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise SystemExit('daemon closed the connection early')
        buf += chunk
    return buf

def new_then_go_away(sock_path, name, half):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.sendall(frame(0x01, struct.pack('<HH', 1, 0)))
    hdr = read_exactly(s, 5)
    plen, typ = struct.unpack('<IB', hdr)
    read_exactly(s, plen)
    if typ != 0x02:
        raise SystemExit('expected HELLO_OK (0x02), got 0x%02x' % typ)
    nb = name.encode()
    argv = b'/bin/sleep\x00300\x00'
    payload = (struct.pack('<HHB', 80, 24, len(nb)) + nb +
               struct.pack('<H', len(argv)) + argv)
    s.sendall(frame(0x12, payload))
    if half:
        s.shutdown(socket.SHUT_WR)   # half-open: read side still up
    s.close()

sock_path, n = sys.argv[1], int(sys.argv[2])
for i in range(n):
    new_then_go_away(sock_path, 'wc%d' % i, False)
for i in range(n):
    new_then_go_away(sock_path, 'wh%d' % i, True)
print('sent %d close + %d half-close' % (n, n))
PY

grep -q "sent $N close" "$TMP/wire.out" || fail "wire probe did not report sending: $(cat "$TMP/wire.out")"

# Give the daemon time to log every accepted request. wait_for polls for the
# LAST name of each batch, so it cannot pass early on a partial result.
wait_for "session 'wc$((N - 1))': pid" "$TMP/daemon.log" 10 \
    || echo "note: last close-probe session never logged within 10s"
wait_for "session 'wh$((N - 1))': pid" "$TMP/daemon.log" 10 \
    || echo "note: last half-close-probe session never logged within 10s"

MISS_CLOSE=""
MISS_HALF=""
for i in $(seq 0 $((N - 1))); do
    grep -q "session 'wc$i': pid" "$TMP/daemon.log" || MISS_CLOSE="$MISS_CLOSE wc$i"
    grep -q "session 'wh$i': pid" "$TMP/daemon.log" || MISS_HALF="$MISS_HALF wh$i"
done
[ -z "$MISS_CLOSE" ] || fail "close-immediately dropped$MISS_CLOSE (pre-fix: all $N)"
[ -z "$MISS_HALF" ] || fail "shutdown(SHUT_WR) dropped$MISS_HALF"

# The daemon must also still be serving: a deferred disconnect that leaked the
# fd or corrupted the client table would show up here, not above.
require_alive "$DPID" "daemon"

# --- 3. the real client, with stdin already at EOF ---------------------------
# `new` here both creates and reports. Assert the two agree: rc, the message,
# and the daemon's own log. Pre-fix this printed the detach line and exited 0
# with nothing created.
#
# BOTH an empty pipe and /dev/null, because the platforms disagree about what an
# exhausted stdin looks like and either one alone passes while a real bug hides
# behind the other. Measured with a standalone poll() probe:
#
#   stdin        Linux      macOS             read()
#   /dev/null    POLLIN     POLLNVAL          0
#   empty pipe   POLLHUP    POLLIN|POLLHUP    0
#   a directory  POLLNVAL   POLLNVAL         -1 EISDIR
#
# The client tested POLLIN only, so five of those six cells matched no branch at
# all and spun the poll loop at 99.6% CPU. Only Linux + /dev/null happened to
# work, which is exactly the combination the pre-existing suite used — so the bug
# was invisible.
#
# The directory row is here to cover the read()-error path specifically: it is
# the only shape where read() returns -1 rather than 0, so it is what separates
# "handles EOF" from "handles an unusable stdin". Without it, dropping the errno
# branch entirely still passes.
#
# It is a directory rather than the more obvious closed fd 0 (`<&-`) because
# closing fd 0 does NOT test this path at all: attach_run() calls pipe() for its
# SIGWINCH self-pipe before the poll loop, pipe() returns the lowest free fds, so
# with fd 0 closed stdin silently BECOMES the read end of that self-pipe — a live
# pipe nobody ever writes, on which the client correctly blocks forever at 0.0%
# CPU. Measured: lsof shows `fd 0 PIPE` and `fd 3 PIPE`. That is a separate
# pre-existing bug with its own test; using it here measured neither bug and
# reported a mutation as caught when both the good and mutated builds failed
# identically.
#
# Every invocation is therefore wrapped in a hard timeout: a regression here is
# a hang or a spin, not a wrong answer, and without the timeout this test would
# hang the whole CI job instead of failing it. `timeout` is coreutils on Linux
# and absent on stock macOS, so run each attempt in a background subshell and
# poll for it, writing the result to a file.
#
# The stdin redirection is set up INSIDE that subshell, which is not a style
# choice. Bash redirects a backgrounded job's stdin to /dev/null when job
# control is off, so `printf '' | client &` silently becomes the /dev/null case
# and the empty-pipe half of this test would never run. That cost a debugging
# detour: the shim looked like it had found a hang in the client when it had in
# fact substituted the other platform's stdin shape.
attempt() { # tag shape secs -> writes "<rc>" to $TMP/$tag.rc, output to $TMP/$tag.out
    local tag="$1" shape="$2" secs="$3"
    (
        case "$shape" in
            empty-pipe)
                printf '' | "$BIN/agent-terminal" new -s "$tag" -- /bin/sleep 300 \
                    > "$TMP/$tag.out" 2>&1 ;;
            devnull)
                "$BIN/agent-terminal" new -s "$tag" -- /bin/sleep 300 < /dev/null \
                    > "$TMP/$tag.out" 2>&1 ;;
            directory)
                # A directory fd polls POLLNVAL and read()s -1 EISDIR on both
                # platforms — the only shape that reaches the client's stdin
                # errno branch. It must exit rather than spin on an fd it can
                # never read. See the table above for why not `<&-`.
                "$BIN/agent-terminal" new -s "$tag" -- /bin/sleep 300 < "$TMP" \
                    > "$TMP/$tag.out" 2>&1 ;;
        esac
        echo $? > "$TMP/$tag.rc"
    ) &
    local p=$! i=0
    while [ "$i" -lt "$((secs * 10))" ]; do
        kill -0 "$p" 2>/dev/null || { wait "$p" 2>/dev/null; return 0; }
        sleep 0.1
        i=$((i + 1))
    done
    # Kill the whole subshell's process group is overkill here; the client is the
    # only long-lived thing in it, and it is named uniquely by its session.
    kill -9 "$p" 2>/dev/null
    pkill -9 -f "new -s $tag " 2>/dev/null
    wait "$p" 2>/dev/null
    echo "timeout" > "$TMP/$tag.rc"
    return 0
}

MISS_CLI=""
LIED=""
STUCK=""
SHAPES="empty-pipe devnull directory"
NSHAPES=3
SEEN_SHAPES=""
for i in $(seq 0 $((N - 1))); do
    # Rotate through all three stdin shapes so each is exercised on each platform.
    SHAPE=$(echo "$SHAPES" | cut -d' ' -f$((i % NSHAPES + 1)))
    case " $SEEN_SHAPES " in *" $SHAPE "*) ;; *) SEEN_SHAPES="$SEEN_SHAPES $SHAPE" ;; esac
    rm -f "$TMP/c$i.rc" "$TMP/c$i.out"
    attempt "c$i" "$SHAPE" 10
    RC=$(cat "$TMP/c$i.rc" 2>/dev/null || echo missing)
    OUT=$(cat "$TMP/c$i.out" 2>/dev/null || true)
    if [ "$RC" = "timeout" ] || [ "$RC" = "missing" ] || [ "$RC" = "137" ]; then
        STUCK="$STUCK c$i($SHAPE)"
        continue
    fi
    if grep -q "session 'c$i': pid" "$TMP/daemon.log"; then
        [ "$RC" -eq 0 ] || LIED="$LIED c$i($SHAPE,created,rc=$RC)"
    else
        MISS_CLI="$MISS_CLI c$i($SHAPE)"
        case "$OUT" in
            *"keeps running"*) LIED="$LIED c$i($SHAPE,absent,claimed-running)" ;;
        esac
    fi
done
[ -z "$STUCK" ] || fail "client never exited with stdin at EOF:$STUCK (spin or block in poll)"
[ -z "$MISS_CLI" ] || fail "client with stdin at EOF dropped$MISS_CLI"
[ -z "$LIED" ] || fail "client misreported:$LIED"

# Guard the loop above rather than trusting it: if N ever drops below the number
# of shapes, or the rotation breaks, the missing shape would go untested silently
# and this test would still print PASS.
SEEN_COUNT=$(echo "$SEEN_SHAPES" | wc -w | tr -d ' ')
[ "$SEEN_COUNT" -eq "$NSHAPES" ] \
    || fail "only $SEEN_COUNT of $NSHAPES stdin shapes were exercised:$SEEN_SHAPES"

# `ls` is the third independent view — the log says the daemon accepted the
# request, this says the session is really in its table and addressable.
LISTED=$("$BIN/agent-terminal" ls 2>/dev/null | grep -c '^\(wc\|wh\|c\)[0-9]*:')
[ "$LISTED" -eq $((3 * N)) ] || fail "expected $((3 * N)) sessions in ls, got $LISTED"

require_alive "$DPID" "daemon"

# --- 4. deferring the disconnect must not mean skipping it -------------------
# The counterpart to parts 1-3: those check nothing is dropped too early, this
# checks the client is still reaped. A peer that shuts down its write side and
# then lingers has hit EOF with its fd still open, so only the explicit
# end-of-function disconnect can free its slot — POLLHUP alone no longer does.
#
# 40 such peers against MAX_CLIENTS=32 (server.c): if EOF stopped reaping,
# server_accept() runs out of slots and closes the next connection immediately,
# so an ordinary `ls` fails. That is the observable, and it is why this probe
# holds all 40 sockets open instead of closing them.
python3 - "$SOCK" 40 "$TMP/slots.ready" > "$TMP/slots.out" 2>"$TMP/slots.err" <<'PY' &
import os, socket, struct, sys, time

def read_exactly(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise SystemExit('daemon closed the connection early')
        buf += chunk
    return buf

sock_path, n, ready = sys.argv[1], int(sys.argv[2]), sys.argv[3]
held = []
for _ in range(n):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.sendall(struct.pack('<IB', 4, 0x01) + struct.pack('<HH', 1, 0))
    hdr = read_exactly(s, 5)
    plen, typ = struct.unpack('<IB', hdr)
    read_exactly(s, plen)
    if typ != 0x02:
        raise SystemExit('expected HELLO_OK (0x02), got 0x%02x' % typ)
    s.shutdown(socket.SHUT_WR)   # EOF for the daemon; fd still open here
    held.append(s)
with open(ready, 'w') as f:
    f.write('READY %d\n' % len(held))
time.sleep(15)                   # keep every fd open across the shell's checks
PY
SLOTPID=$!

if wait_for "READY 40" "$TMP/slots.ready" 10; then
    "$BIN/agent-terminal" ls > "$TMP/ls2.out" 2>&1 \
        || fail "ls failed while 40 half-closed peers were held open — client slots leaked: $(cat "$TMP/ls2.out")"
    LISTED2=$(grep -c '^\(wc\|wh\|c\)[0-9]*:' "$TMP/ls2.out")
    [ "$LISTED2" -eq $((3 * N)) ] \
        || fail "expected $((3 * N)) sessions while slots were held, got $LISTED2: $(cat "$TMP/ls2.out")"
else
    kill "$SLOTPID" 2>/dev/null
    # The probe dying on "daemon closed the connection early" IS the leak: with
    # EOF no longer reaped, the first 32 half-closed peers hold every slot and
    # server_accept() closes the 33rd on arrival. Name that explicitly, because
    # the raw stderr reads like a probe bug rather than the condition under test.
    case "$(cat "$TMP/slots.err")" in
        *"closed the connection early"*)
            fail "daemon stopped accepting after 32 half-closed peers — EOF no longer frees a client slot" ;;
        *)
            fail "slot probe never became ready: $(cat "$TMP/slots.err")" ;;
    esac
fi
kill "$SLOTPID" 2>/dev/null
wait "$SLOTPID" 2>/dev/null

require_alive "$DPID" "daemon"
echo "PASS: $((3 * N)) sessions survived an immediate client close (wire close, wire half-close, cli stdin at EOF); none misreported; 40 half-closed peers still reaped"
