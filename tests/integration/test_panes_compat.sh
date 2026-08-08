#!/usr/bin/env bash
# The single-pane byte stream is a compatibility surface: at exactly one pane
# the daemon must tee raw PTY bytes with perfect fidelity, exactly as it did
# before panes existed. This pins those bytes against a committed golden
# file. Its fragility against ANY deliberate single-pane byte change is the
# feature — such a change must be visible in review, not incidental.
#
# The old binary cannot be run from the new tree, so a golden byte file is
# the only honest oracle. The child's output is fully deterministic (fixed
# printf script, fixed geometry, TERM pinned by the daemon), and the capture
# starts AFTER the snapshot (which varies with sb_lines) by using a second
# client that attaches before the child starts printing.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

GOLDEN="$ROOT/tests/data/golden/single_pane_stream.bin"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
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

# The child waits for a go-file so the capturing client is attached before
# the first byte; every byte it then writes is deterministic.
GO="$TMP/go"
python3 - "$SOCK" "$TMP/stream.bin" "$GO" > "$TMP/probe.out" 2>&1 <<'PY' \
    || fail "$(cat "$TMP/probe.out")"
import os, socket, struct, sys, time

sock_path, out_path, go_path = sys.argv[1], sys.argv[2], sys.argv[3]

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
    s.settimeout(15)
    s.connect(sock_path)
    s.sendall(frame(0x01, struct.pack('<HH', 1, 0)))
    typ, _ = read_frame(s)
    assert typ == 0x02
    return s

# Deterministic child: SGR runs, wide chars, wraps, a scroll, DECSC/DECRC,
# a scroll region — the byte shapes the raw tee must carry verbatim.
script = (
    "while [ ! -e %s ]; do sleep 0.05; done; "
    "printf 'plain\\n'; "
    "printf '\\033[1;31mbold red\\033[0m\\n'; "
    "printf '\\033[48;5;27mindexed bg\\033[0m\\n'; "
    "printf '\\033[38;2;1;2;3mtruecolor\\033[0m\\n'; "
    "printf 'CJK: \\345\\256\\275\\345\\255\\227\\n'; "
    "printf 'comb: e\\314\\201\\n'; "
    "i=0; while [ $i -lt 30 ]; do printf 'scroll line %%d\\n' $i; i=$((i+1)); done; "
    "printf '\\0337saved\\0338restored\\n'; "
    "printf '\\033[2;10r\\033[2;1Hregion\\033[r'; "
    "printf '\\033[5;1HDONE-MARKER\\n'"
) % go_path

s = connect()
name = b'gold'
argv = b'\x00'.join([b'/bin/sh', b'-c', script.encode()]) + b'\x00'
s.sendall(frame(0x12, struct.pack('<HHB', 80, 24, len(name)) + name +
                struct.pack('<H', len(argv)) + argv))

# Drain until the snapshot arrives (creation attaches us), THEN start the
# child. Everything after this point is raw tee bytes.
typ, _ = read_frame(s)
assert typ == 0x31, 'expected the creation snapshot first, got %r' % typ
open(go_path, 'w').close()

out = open(out_path, 'wb')
deadline = time.time() + 20
seen = b''
s.settimeout(0.3)
while b'DONE-MARKER' not in seen and time.time() < deadline:
    try:
        typ, payload = read_frame(s)
    except socket.timeout:
        continue
    if typ is None:
        break
    if typ == 0x30:
        out.write(payload)
        seen += payload
        seen = seen[-8192:]
out.close()
assert b'DONE-MARKER' in seen, 'child output never completed'
print('captured')
PY

grep -q captured "$TMP/probe.out" || fail "capture failed: $(cat "$TMP/probe.out")"

if [ ! -f "$GOLDEN" ]; then
    if [ "${REGEN_GOLDEN:-0}" = "1" ]; then
        mkdir -p "$(dirname "$GOLDEN")"
        cp "$TMP/stream.bin" "$GOLDEN"
        echo "PASS: golden regenerated ($(wc -c < "$GOLDEN" | tr -d ' ') bytes) — commit it"
        exit 0
    fi
    fail "golden file missing: $GOLDEN (run with REGEN_GOLDEN=1 to create)"
fi

if ! cmp -s "$GOLDEN" "$TMP/stream.bin"; then
    echo "byte diff (golden vs got):"
    cmp "$GOLDEN" "$TMP/stream.bin" | head -3
    echo "golden: $(wc -c < "$GOLDEN") bytes; got: $(wc -c < "$TMP/stream.bin") bytes"
    fail "single-pane byte stream changed — if deliberate, REGEN_GOLDEN=1 and commit"
fi

# Belt and braces: no composite artifacts on the single-pane path.
grep -q "entering composite" "$TMP/daemon.log" && fail "composite engaged for a single pane"

require_alive "$DPID" "daemon"
echo "PASS: single-pane stream is byte-identical to the committed golden ($(wc -c < "$GOLDEN" | tr -d ' ') bytes)"
