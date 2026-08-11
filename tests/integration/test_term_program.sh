#!/usr/bin/env bash
# Sessions must see agent-terminal as their terminal identity — not the
# terminal the DAEMON happened to be launched from, and not a stale
# session id. Programs branch on TERM_PROGRAM (Claude Code does); an
# inherited value describes a terminal the child is not talking to.
#   1. start the daemon with a FAKE inherited identity (the lie case:
#      daemon launched from some terminal emulator)
#   2. a session prints its TERM_PROGRAM / _VERSION / _SESSION_ID
#   3. the child must see agent-terminal / unset / unset
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    [ -n "$DPID" ] && kill -9 "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# The lie this test exists to kill: an identity inherited from whatever
# spawned the daemon. All three variables poisoned on purpose.
TERM_PROGRAM="fake-emulator" \
TERM_PROGRAM_VERSION="9.9.9" \
TERM_SESSION_ID="stale-id-1234" \
    "$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
sleep 1
require_alive "$DPID" "daemon"

# Print the identity, then scroll it into the ring so `history` (which
# reads scrolled-off lines from disk, no attach needed) can recover it.
mkfifo "$TMP/in1"
"$BIN/agent-terminal" new -s tp -- bash -c '
    printf "TP=[%s] TPV=[%s] TSID=[%s]\n" \
        "${TERM_PROGRAM:-unset}" \
        "${TERM_PROGRAM_VERSION:-unset}" \
        "${TERM_SESSION_ID:-unset}"
    seq -f "tp-filler-%.0f" 1 120
    sleep 60' \
    < "$TMP/in1" > /dev/null 2>&1 &
C1=$!
exec 3>"$TMP/in1"
sleep 2.0   # > 1s flush tick, so the scrollback log is durable

"$BIN/agent-terminal" history -s tp > "$TMP/hist.out" 2>&1 || fail "history exited nonzero"
kill -9 "$C1" 2>/dev/null
exec 3>&-

# 120 lines through a 24-row screen: ~96 scrolled off; filler-70 is
# safely in the ring while filler-119 is still on screen.
grep -q "tp-filler-70" "$TMP/hist.out" || fail "session output never reached the ring"
grep -q "TP=\[agent-terminal\]" "$TMP/hist.out" \
    || fail "child does not see TERM_PROGRAM=agent-terminal: $(grep 'TP=' "$TMP/hist.out" | head -1)"
grep -q "TPV=\[unset\]" "$TMP/hist.out" \
    || fail "stale TERM_PROGRAM_VERSION leaked into the session"
grep -q "TSID=\[unset\]" "$TMP/hist.out" \
    || fail "stale TERM_SESSION_ID leaked into the session"

echo "PASS: sessions see TERM_PROGRAM=agent-terminal; inherited terminal identity does not leak"
