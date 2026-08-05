#!/usr/bin/env bash
# The final screen of an ended session must be recoverable via `history`.
#
# Exists because scrollback only ever received lines that scrolled OFF the
# primary screen. Anything still visible when a session ended was lost: a
# child that printed a short fatal message and exited left `history`
# returning zero bytes, while the identical message survived once 100 filler
# lines had pushed it off. For a tool whose purpose is recovering a dead AI
# agent's output, that inverted the priority.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal

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
sleep 0.3
require_alive "$DPID" "daemon"

# Run a child to completion, then read its history. The session is gone by
# then, which is the whole point: nothing but the flush can preserve this.
run_and_wait() { # name script
    "$BIN/agent-terminal" new -s "$1" -- bash -c "$2" < /dev/null > /dev/null 2>&1 || true
    sleep 2   # let the child exit and the 1s scrollback flush tick fire
}

# --- 1. a short message that never scrolled off must survive ---
run_and_wait fatal "echo 'FATAL-marker: died at line 441'; echo 'HINT-marker: use --resume'"
"$BIN/agent-terminal" history -s fatal > "$TMP/h1" 2>&1
grep -q "FATAL-marker" "$TMP/h1" \
    || fail "final screen lost: FATAL-marker absent from history ($(wc -c < "$TMP/h1") bytes)"
grep -q "HINT-marker" "$TMP/h1" || fail "final screen only partly recovered"

# --- 2. no duplication: N lines printed => exactly N lines recovered ---
# 100 lines on a 24-row screen means most scrolled off and the rest were
# flushed; a line must not arrive by both routes.
run_and_wait dup 'for i in $(seq 1 100); do echo "N$i"; done'
"$BIN/agent-terminal" history -s dup > "$TMP/h2" 2>&1
TOTAL=$(grep -c '^N[0-9]*$' "$TMP/h2" || true)
UNIQ=$(grep '^N[0-9]*$' "$TMP/h2" | sort -u | wc -l | tr -d ' ')
[ "$TOTAL" = "100" ] || fail "expected 100 recovered lines, got $TOTAL"
[ "$UNIQ" = "100" ] || fail "duplicate lines in history: $TOTAL total, $UNIQ distinct"
grep -q '^N100$' "$TMP/h2" || fail "last line (still on screen at exit) not recovered"

# --- 3. an idle screen must not append blank records ---
# Counting non-blank text would prove nothing: serialize_line already trims
# trailing empty cells, so untrimmed blank rows become empty strings either
# way. The cost is on disk — measured 2 records / 38 bytes trimmed versus
# 24 records / 390 bytes untrimmed for the same two lines — so assert on the
# CRC-framed record count, which is what actually changes.
run_and_wait idle "echo ONE-marker; echo TWO-marker"
"$BIN/agent-terminal" history -s idle > "$TMP/h3" 2>&1
grep -q "ONE-marker" "$TMP/h3" || fail "idle session lost its output"
LOG="$TMP/.agent-terminal/sessions/idle/scrollback.log"
[ -f "$LOG" ] || fail "scrollback log missing at $LOG"
BYTES=$(wc -c < "$LOG" | tr -d ' ')
[ "$BYTES" -lt 100 ] \
    || fail "idle 24-row screen padded the log: $BYTES bytes for 2 lines (trimmed is ~38)"

# --- 4. alternate-screen content must not leak into scrollback ---
# vt.h: scrollback holds primary-screen content only.
run_and_wait alt \
    "printf '\033[?1049h'; echo ALT-NOISE-marker; printf '\033[10;1HMORE-ALT-marker'"
"$BIN/agent-terminal" history -s alt > "$TMP/h4" 2>&1
grep -q "ALT-NOISE-marker" "$TMP/h4" && fail "alt-screen content leaked into scrollback"
grep -q "MORE-ALT-marker" "$TMP/h4" && fail "alt-screen content leaked into scrollback"

# --- 5. an explicit kill preserves the final screen too ---
# session_kill also routes through session_free_slot.
mkfifo "$TMP/in5"
"$BIN/agent-terminal" new -s live -- bash -c 'echo LIVE-marker; sleep 300' \
    < "$TMP/in5" > /dev/null 2>&1 &
exec 5>"$TMP/in5"
sleep 1.5
"$BIN/agent-terminal" kill -s live > /dev/null 2>&1 || fail "kill of live session failed"
exec 5>&-
sleep 1.5
"$BIN/agent-terminal" history -s live > "$TMP/h5" 2>&1
grep -q "LIVE-marker" "$TMP/h5" || fail "explicit kill discarded the visible screen"

# --- 6. graceful daemon shutdown preserves visible screens ---
# The children die with the daemon, so SIGTERM is the last chance to save
# them. A service restart used to discard every session's visible output.
mkfifo "$TMP/in6"
"$BIN/agent-terminal" new -s term -- bash -c 'echo TERM-marker; sleep 300' \
    < "$TMP/in6" > /dev/null 2>&1 &
exec 6>"$TMP/in6"
sleep 1.5
require_alive "$DPID" "daemon"
kill -TERM "$DPID"
wait "$DPID" 2>/dev/null || true
DPID=""
exec 6>&-
sleep 0.5
"$BIN/agent-terminal" history -s term > "$TMP/h6" 2>&1
grep -q "TERM-marker" "$TMP/h6" \
    || fail "graceful daemon shutdown discarded visible screens ($(wc -c < "$TMP/h6") bytes)"

echo "PASS: final screen recovered on child exit, explicit kill and daemon shutdown"
