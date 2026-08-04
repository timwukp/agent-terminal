#!/usr/bin/env bash
# M2 acceptance: content written BEFORE a client attaches must be visible to
# that client via the VT-engine snapshot (raw passthrough alone cannot do
# this — it proves the daemon reconstructs the screen, not just relays).
set -u

BUILD="${BUILD:-debug}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/$BUILD"
TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
fail() { echo "FAIL: $1"; exit 1; }
trap cleanup EXIT

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
sleep 0.3

# Session prints a marker (with SGR color to exercise pen state) and stays alive.
mkfifo "$TMP/in1"
"$BIN/agent-terminal" new -s snap -- \
    bash -c "printf '\033[1;32mMARKER-ALPHA\033[0m\nplain-beta\n'; sleep 60" \
    < "$TMP/in1" > /dev/null 2>&1 &
C1=$!
exec 3>"$TMP/in1"
sleep 0.8

# Kill client 1 hard; no client is attached while the content sits in the daemon.
kill -9 "$C1"
exec 3>&-
sleep 0.3

# Client 2 attaches fresh: everything it knows must come from the snapshot.
mkfifo "$TMP/in2"
"$BIN/agent-terminal" attach -s snap < "$TMP/in2" > "$TMP/out2" 2>&1 &
C2=$!
exec 4>"$TMP/in2"
sleep 0.8

printf '\x1c\x04' >&4   # detach
exec 4>&-
wait "$C2" 2>/dev/null

grep -q "MARKER-ALPHA" "$TMP/out2" || fail "snapshot did not restore pre-attach screen content"
grep -q "plain-beta"   "$TMP/out2" || fail "snapshot missing second line"
grep -q $'\x1b\[?2004' "$TMP/out2" && true  # mode re-arm present is a bonus, not asserted

"$BIN/agent-terminal" kill -s snap > /dev/null 2>&1
echo "PASS: reattach snapshot restores screen content written before attach"
