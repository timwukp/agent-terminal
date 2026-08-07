#!/usr/bin/env bash
# A client of a dead session must not be able to talk to whatever session
# reuses the slot.
#
# Exists because session_reap_children freed a session's slot without clearing
# s->clients[] or each client's c->attached. Not a use-after-free — the slot
# lives in static g_sessions[] — which is exactly why nothing crashed: the
# stale c->attached silently aliased the NEXT session allocated into that
# slot, so a client that had attached to session A could inject stdin into,
# resize, and read scrollback from session B, a session it never named.
# session_kill always did this correctly; only the reap path forgot.
#
# The probe is wire-level, not the real client, deliberately: the real client
# exits when it reads MSG_SESSION_EXITED, so it can never be attached-to-a-dead
# -session long enough to alias. A peer that simply does not read its socket
# stays in exactly that state, and nothing requires a client to read promptly.
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

# -v: "session '<name>': pid" and "child exited" lines are the sync points.
"$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"
require_alive "$DPID" "daemon"

SOCK="$TMP/.agent-terminal/run/default.sock"
[ -S "$SOCK" ] || fail "daemon socket $SOCK missing"

INJECT_LOG="$TMP/inject.log"

python3 - "$SOCK" "$TMP/daemon.log" "$INJECT_LOG" > "$TMP/probe.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/probe.out")"
import socket, struct, sys, time

sock_path, daemon_log, inject_log = sys.argv[1], sys.argv[2], sys.argv[3]

def frame(t, payload):
    return struct.pack('<IB', len(payload), t) + payload

def read_exactly(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            return None          # daemon closed on us
        buf += chunk
    return buf

def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.sendall(frame(0x01, struct.pack('<HH', 1, 0)))
    hdr = read_exactly(s, 5)
    plen, typ = struct.unpack('<IB', hdr)
    read_exactly(s, plen)
    assert typ == 0x02, 'expected HELLO_OK, got 0x%02x' % typ
    return s

def new_session(s, name, argv):
    nb = name.encode()
    ab = b'\x00'.join(a.encode() for a in argv) + b'\x00'
    s.sendall(frame(0x12, struct.pack('<HHB', 80, 24, len(nb)) + nb +
                    struct.pack('<H', len(ab)) + ab))

def wait_log(pattern, timeout=10):
    for _ in range(timeout * 10):
        with open(daemon_log, errors='replace') as f:
            if pattern in f.read():
                return True
        time.sleep(0.1)
    return False

# 1. Session A on s1; its child exits at once. s1 stays open and unread, so
#    the daemon cannot mistake it for a gone peer — the alias, if present,
#    lives exactly as long as this socket.
s1 = connect()
new_session(s1, 'aa', ['/bin/true'])
assert wait_log("session 'aa': pid"), 'session aa never created'
assert wait_log("session 'aa': child exited"), 'aa child never reaped'

# 2. Session B from a fresh connection. A fresh daemon allocated A slot 0 and
#    the reap freed it, so B lands in the same slot — the aliasing condition.
#    B's child appends every line it reads to a file we can grep.
s2 = connect()
new_session(s2, 'bb', ['/bin/sh', '-c',
                       'while IFS= read -r line; do echo "$line" >> %s; done' % inject_log])
assert wait_log("session 'bb': pid"), 'session bb never created'

# 3. The attack: MSG_STDIN_DATA on the dead session's connection. Pre-fix the
#    daemon routed these bytes into B's PTY. Post-fix s1 was disconnected when
#    A's slot was freed, so the send may fail — either shape of failure is the
#    fix working; what must not happen is the bytes reaching B.
try:
    s1.sendall(frame(0x20, b'INJECT\r'))
    time.sleep(1.0)              # give any (buggy) routing time to land
except (BrokenPipeError, ConnectionResetError):
    pass

# 4. Independent post-fix observable: s1 must be at EOF, not merely ignored —
#    a slot the daemon still thinks is attached would keep the socket open.
s1.settimeout(3)
try:
    leftover = s1.recv(65536)    # drain SESSION_EXITED etc. until EOF
    while leftover:
        leftover = s1.recv(65536)
    eof = True
except socket.timeout:
    eof = False
except (ConnectionResetError, OSError):
    eof = True
assert eof, "dead session's client socket still open: daemon kept the stale attach"

s1.close(); s2.close()
print('probe done')
PY

grep -q "probe done" "$TMP/probe.out" || fail "probe did not complete: $(cat "$TMP/probe.out")"

# The heart of the test: nothing typed on the dead session's connection may
# reach the session that reused its slot.
if grep -q "INJECT" "$INJECT_LOG" 2>/dev/null; then
    fail "stdin from a dead session's client reached the session reusing its slot"
fi

# And session B must be alive and unharmed by any of this.
"$BIN/agent-terminal" ls | grep -Eq "bb: .*pid [0-9]+" || fail "session bb not alive"

require_alive "$DPID" "daemon"
echo "PASS: dead session's client was disconnected; no stdin aliased into the slot's next tenant"
