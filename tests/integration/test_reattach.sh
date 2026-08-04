#!/usr/bin/env bash
# M1 acceptance: kill -9 the client; the session (child process) must survive
# and a new client must be able to reattach and keep interacting.
set -u

BUILD="${BUILD:-debug}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/$BUILD"
TMP="$(mktemp -d)"
export HOME="$TMP"            # isolate runtime dir + socket
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
fail() { echo "FAIL: $1"; exit 1; }
trap cleanup EXIT

# Poll for a pattern in a file — fixed sleeps are flaky on loaded CI runners.
wait_for() { # pattern file timeout_s
    local i=0
    while [ "$i" -lt "$((${3} * 10))" ]; do
        grep -q "$1" "$2" 2>/dev/null && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
sleep 0.3

SESSION_LOG="$TMP/child.log"

# Client 1: stdin from a FIFO we keep open, so the client stays attached
# until we kill -9 it.
mkfifo "$TMP/in1"
"$BIN/agent-terminal" new -s t1 -- \
    bash -c "while IFS= read -r line; do echo \"\$line\" >> '$SESSION_LOG'; done" \
    < "$TMP/in1" > /dev/null 2>&1 &
C1=$!
exec 3>"$TMP/in1"   # hold the FIFO open
sleep 1

printf 'before-crash\r' >&3
wait_for "before-crash" "$SESSION_LOG" 10 || fail "input via client 1 did not reach child"

kill -9 "$C1"
exec 3>&-
sleep 0.5

# Child must still be alive after its client was killed.
"$BIN/agent-terminal" ls | grep -Eq "t1: .*pid [0-9]+" || fail "session t1 not alive after client kill -9"

# Client 2: reattach and type again.
mkfifo "$TMP/in2"
"$BIN/agent-terminal" attach -s t1 < "$TMP/in2" > /dev/null 2>&1 &
C2=$!
exec 4>"$TMP/in2"
sleep 1

printf 'after-reattach\r' >&4
wait_for "after-reattach" "$SESSION_LOG" 10 || fail "reattached input did not reach the child"

# Detach politely: Ctrl-\ Ctrl-d, client should exit 0.
printf '\x1c\x04' >&4
exec 4>&-
wait "$C2"
RC=$?
[ "$RC" -eq 0 ] || fail "detach chord did not exit cleanly (rc=$RC)"

"$BIN/agent-terminal" kill -s t1 > /dev/null
echo "PASS: session survived client kill -9; reattach + input + detach all work"
