#!/usr/bin/env bash
# kill must actually kill, and must report what the daemon did.
#
# Exists because `kill` was silently a no-op for the whole life of the client:
# it wrote the frame and closed the fd immediately, so the daemon saw EOF and
# tore the connection down before dispatching the buffered frame. The client
# still printed "killed 'name'" and exited 0 because it only ever checked its
# own write(). Every assertion below distinguishes the fix from that bug —
# checking only the exit code, as test_reattach.sh used to, cannot.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal

TMP="$(mktemp -d)"
export HOME="$TMP"            # isolate runtime dir + socket
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
sleep 0.3
require_alive "$DPID" "daemon"

# Start a session and leave it running with zero clients, the state a killed
# session is most likely to be in.
start_session() { # name fifo_fd_num fifo_path
    mkfifo "$3"
    "$BIN/agent-terminal" new -s "$1" -- sleep 300 < "$3" > /dev/null 2>&1 &
    eval "exec $2>\"$3\""
    sleep 1
}

session_pid() { # name
    "$BIN/agent-terminal" ls | sed -n "s/^$1: .*pid \([0-9]*\).*/\1/p"
}

# --- 1. kill with a client still attached ---
start_session a1 3 "$TMP/in1"
PID_A="$(session_pid a1)"
[ -n "$PID_A" ] || fail "session a1 never registered"
"$BIN/agent-terminal" kill -s a1 > "$TMP/k1.out" 2>&1 \
    || fail "kill of attached session exited nonzero"
grep -q "killed 'a1'" "$TMP/k1.out" || fail "kill did not report success: $(cat "$TMP/k1.out")"
"$BIN/agent-terminal" ls | grep -q "^a1:" && fail "a1 still listed after kill"
kill -0 "$PID_A" 2>/dev/null && fail "child $PID_A survived kill of a1"
exec 3>&-

# --- 2. kill a session with zero clients ---
start_session a2 4 "$TMP/in2"
printf '\x1c\x04' >&4          # detach, leaving the session running
exec 4>&-
sleep 0.5
"$BIN/agent-terminal" ls | grep -q "a2: .*0 clients" || fail "a2 not detached as expected"
PID_B="$(session_pid a2)"
"$BIN/agent-terminal" kill -s a2 > /dev/null 2>&1 || fail "kill of detached session exited nonzero"
"$BIN/agent-terminal" ls | grep -q "^a2:" && fail "a2 still listed after kill"
kill -0 "$PID_B" 2>/dev/null && fail "child $PID_B survived kill of a2"

# --- 3. killing an unknown session fails, and spares the live one ---
start_session a3 5 "$TMP/in3"
if "$BIN/agent-terminal" kill -s nosuch > "$TMP/k3.out" 2>&1; then
    fail "kill of a nonexistent session exited 0"
fi
grep -q "no such session" "$TMP/k3.out" \
    || fail "kill of unknown session did not surface the daemon's error: $(cat "$TMP/k3.out")"
"$BIN/agent-terminal" ls | grep -q "^a3:" || fail "a3 died from a kill aimed at another name"
"$BIN/agent-terminal" kill -s a3 > /dev/null 2>&1 || fail "cleanup kill of a3 failed"
exec 5>&-

require_alive "$DPID" "daemon"
echo "PASS: kill removes the session and its child, and reports the daemon's verdict"
