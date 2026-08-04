#!/usr/bin/env bash
# M3 acceptance: scrollback survives daemon kill -9.
#   1. session generates enough output to scroll off-screen
#   2. kill -9 the DAEMON (worst case: children die, grids die)
#   3. `history` recovers the scrolled-off lines from disk — no daemon needed
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

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
sleep 1
require_alive "$DPID" "daemon"

# 200 numbered lines through a 24-row PTY: ~176 lines must scroll into
# the scrollback ring + disk log.
mkfifo "$TMP/in1"
"$BIN/agent-terminal" new -s hist -- bash -c 'seq -f "history-line-%.0f" 1 200; sleep 60' \
    < "$TMP/in1" > /dev/null 2>&1 &
C1=$!
exec 3>"$TMP/in1"
sleep 2.0   # > 1s flush tick, so the log is durable

# The kill must be the cause of death. If the daemon already exited, the
# scrollback below would be testing a different scenario than advertised.
require_alive "$DPID" "daemon (before kill -9)"
kill -9 "$DPID"   # daemon dies hard: sessions gone, grids gone
kill -9 "$C1" 2>/dev/null
exec 3>&-
DPID=""
sleep 1

# history must work with NO daemon running (reads disk directly).
"$BIN/agent-terminal" history -s hist > "$TMP/hist.out" 2>&1 || fail "history exited nonzero"

grep -q "history-line-1\b" "$TMP/hist.out" || fail "first scrolled line missing"
grep -q "history-line-170" "$TMP/hist.out" || fail "later scrolled line missing"
COUNT=$(grep -c "history-line-" "$TMP/hist.out")
[ "$COUNT" -ge 150 ] || fail "expected >=150 recovered lines, got $COUNT"

# Torn-tail resilience: garbage at EOF must not break recovery.
LOG="$TMP/.agent-terminal/sessions/hist/scrollback.log"
[ -f "$LOG" ] || fail "scrollback.log missing"
printf '\x99\x99GARBAGE' >> "$LOG"
"$BIN/agent-terminal" history -s hist > "$TMP/hist2.out" 2>&1
grep -q "history-line-170" "$TMP/hist2.out" || fail "recovery after torn tail failed"

echo "PASS: scrollback survives daemon kill -9; history recovers $COUNT lines (torn tail OK)"
