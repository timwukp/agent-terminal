#!/usr/bin/env bash
# Session names must never escape ~/.agent-terminal/sessions/.
#
# Exists because a name was interpolated into a path as one component and then
# mkdir'd with no validation: `-s ../escape` created ~/.agent-terminal/escape/
# and wrote a 0600 scrollback log there, and a deeper name reached any directory
# the user could write. The distinct names "." and "./" also resolved to the
# sessions dir itself, so two sessions shared one log and `history -s .` printed
# a different session's output.
#
# Two gates are checked separately because they protect different paths: the
# client's own gate (the only one `history` passes through, since it opens the
# log file itself and never talks to the daemon) and the daemon's gate (a daemon
# must not trust a client, and any other client can speak this protocol). The
# wire half therefore bypasses the client binary entirely — asserting only
# through the CLI would pass even with the daemon completely unguarded.
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

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
# Wait on the listening line rather than a fixed sleep: this test connects to
# the socket directly, so it must not start probing before bind() lands.
wait_for "listening on" "$TMP/daemon.log" 5 || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"
require_alive "$DPID" "daemon"

SOCK="$TMP/.agent-terminal/run/default.sock"
[ -S "$SOCK" ] || fail "daemon socket $SOCK missing"

# --- 1. client-side gate: the CLI refuses the name itself ---------------------
# `history` never reaches the daemon, so this gate is the only one covering it.
for bad in '../escape' '..' '.' './' 'a/b' '/tmp/abs' '.hidden'; do
    for verb in new attach history kill; do
        if "$BIN/agent-terminal" "$verb" -s "$bad" > "$TMP/cli.out" 2>&1; then
            fail "client accepted '$verb -s $bad' (exited 0)"
        fi
        grep -q "invalid session name" "$TMP/cli.out" \
            || fail "'$verb -s $bad' rejected for the wrong reason: $(cat "$TMP/cli.out")"
    done
done

# A valid name must still work, or the gate is just breaking normal use.
mkfifo "$TMP/in_ok"
"$BIN/agent-terminal" new -s cliok -- sleep 300 < "$TMP/in_ok" > /dev/null 2>&1 &
exec 3>"$TMP/in_ok"
sleep 1
"$BIN/agent-terminal" ls | grep -q '^cliok:' || fail "valid name 'cliok' failed to start"

# --- 2. daemon-side gate: speak the protocol directly ------------------------
# Bypasses the client's gate entirely. [u32 payload_len][u8 type][payload], LE.
# HELLO       = 0x01: u16 ver, u16 flags
# NEW_SESSION = 0x12: u16 cols, u16 rows, u8 nlen, name, u16 argv_bytes, argv
# Prints one "name<TAB>type<TAB>code<TAB>message" line per probe.
python3 - "$SOCK" > "$TMP/wire.out" 2>"$TMP/wire.err" <<'PY' || fail "wire probe crashed: $(cat "$TMP/wire.err")"
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

def read_frame(s):
    hdr = read_exactly(s, 5)
    plen, typ = struct.unpack('<IB', hdr)
    return typ, read_exactly(s, plen)

def new_session(sock_path, name):
    nb = name.encode()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(sock_path)
    s.sendall(frame(0x01, struct.pack('<HH', 1, 0)))
    typ, _ = read_frame(s)
    if typ != 0x02:
        raise SystemExit('expected HELLO_OK (0x02), got 0x%02x' % typ)
    argv = b'/bin/sleep\x00300\x00'
    payload = (struct.pack('<HHB', 80, 24, len(nb)) + nb +
               struct.pack('<H', len(argv)) + argv)
    s.sendall(frame(0x12, payload))
    typ, p = read_frame(s)
    code, msg = 0, ''
    if typ == 0x03 and len(p) >= 4:                       # MSG_ERR
        code = struct.unpack('<H', p[0:2])[0]
        mlen = struct.unpack('<H', p[2:4])[0]
        msg = p[4:4 + mlen].decode('utf-8', 'replace')
    s.close()
    return typ, code, msg

sock_path = sys.argv[1]
# The deep name targets $HOME itself: sessions/ is two levels below it, so
# ../../pwned resolves to $HOME/pwned and lands inside the test's own tmpdir.
probes = ['..', '../escape', '../../pwned', '.', './', 'a/b', 'a/../b',
          '/tmp/abs', '.hidden', 'nl\nname']
for name in probes:
    typ, code, msg = new_session(sock_path, name)
    print('%s\t0x%02x\t%d\t%s' % (name.replace('\n', '\\n'), typ, code, msg))
# Positive control on the same code path: a valid name must be accepted, so a
# blanket-reject daemon cannot pass this test.
typ, code, msg = new_session(sock_path, 'wireok')
print('wireok\t0x%02x\t%d\t%s' % (typ, code, msg))
PY

# Every traversal probe must be MSG_ERR (0x03) / ERR_BAD_REQUEST (4).
while IFS=$'\t' read -r name typ code msg; do
    if [ "$name" = "wireok" ]; then
        [ "$typ" = "0x31" ] || fail "valid name over the wire got $typ ($msg), expected MSG_SNAPSHOT 0x31"
        continue
    fi
    [ "$typ" = "0x03" ] || fail "daemon accepted '$name' (reply $typ, not MSG_ERR)"
    [ "$code" = "4" ] || fail "daemon rejected '$name' with code $code, expected ERR_BAD_REQUEST 4"
    case "$msg" in
        *"invalid session name"*) ;;
        *) fail "daemon rejected '$name' for the wrong reason: $msg" ;;
    esac
done < "$TMP/wire.out"

PROBES=$(grep -c . "$TMP/wire.out")
[ "$PROBES" -eq 11 ] || fail "expected 11 wire probes, got $PROBES"

# --- 3. nothing was created outside the sessions directory ------------------
# The assertions that actually distinguish the fix from the bug: pre-fix,
# `escape` and `pwned` existed and sessions/scrollback.log was written.
SESS="$TMP/.agent-terminal/sessions"
for stray in "$TMP/.agent-terminal/escape" "$TMP/pwned" "$SESS/scrollback.log" \
             "$SESS/../escape" "$TMP/.agent-terminal/sessions/.hidden"; do
    [ -e "$stray" ] && fail "traversal created $stray"
done

# The sessions dir must hold exactly the two sessions that used valid names.
ENTRIES=$(ls -A "$SESS" | sort | tr '\n' ' ')
[ "$ENTRIES" = "cliok wireok " ] || fail "unexpected sessions dir contents: '$ENTRIES'"

# `history` for a valid name still reads its own log, not a neighbour's.
"$BIN/agent-terminal" kill -s cliok > /dev/null 2>&1
"$BIN/agent-terminal" kill -s wireok > /dev/null 2>&1
exec 3>&-

require_alive "$DPID" "daemon"
echo "PASS: traversal names rejected at both gates; $PROBES wire probes, no stray paths"
