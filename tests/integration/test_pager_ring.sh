#!/usr/bin/env bash
# Copy-mode must include the scrollback tail that has NOT reached disk yet.
#
# Scrollback lines are only durable on the daemon's 1 s flush tick, so a client
# reading just the on-disk log would silently miss up to a second of the most
# recent history — the part a user is most likely to be looking for. This test
# enters copy-mode immediately after a burst, with no flush wait, so the newest
# lines exist ONLY in the daemon's in-memory ring and can be reached only via
# MSG_SCROLLBACK_REQ over the wire. The unit tests cover want_from's arithmetic;
# only this one proves the request is actually sent and the reply parsed.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    exec 3>&- 2>/dev/null
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 || fail "daemon did not start"

OUT="$TMP/client.out"
mkfifo "$TMP/in"

# 100 lines through a 24-row screen: 77 scroll off, 23 stay on screen.
"$BIN/agent-terminal" new -s ring -- bash --norc -c '
    for i in $(seq 1 100); do echo "RING-$i"; done
    exec sleep 300
' < "$TMP/in" > "$OUT" 2>&1 &
CPID=$!
exec 3>"$TMP/in"

wait_for "RING-100" "$OUT" 10 || fail "child output never reached the client"

# Confirm the premise instead of assuming it: with no flush wait the log must
# still be short of the full 77 lines. If the daemon flushed faster than this
# test can type, the ring assertion below would pass for the wrong reason.
LOG="$TMP/.agent-terminal/sessions/ring/scrollback.log"
DISK_LINES=$(wc -l < "$LOG" 2>/dev/null || echo 0)
[ "$DISK_LINES" -lt 77 ] || fail "log already holds all 77 lines ($DISK_LINES); nothing left in the ring to test"

MARK=$(( $(wc -c < "$OUT") + 1 ))
printf '\x1c[' >&3          # copy-mode, before the flush tick
sleep 0.6
TAIL=$(tail -c "+$MARK" "$OUT")

# The status line reports the total the pager holds. It must be all 77
# scrolled-off lines, not just the $DISK_LINES that reached disk.
case "$TAIL" in
    *"/77 "*) : ;;
    *) fail "copy-mode holds only the flushed lines, not the ring tail (disk had $DISK_LINES; status: $(printf '%s' "$TAIL" | grep -ao 'scrollback [0-9]*-[0-9]*/[0-9]*' | tail -1))" ;;
esac

# And the newest scrolled-off line is readable, not merely counted.
case "$TAIL" in
    *RING-77*) : ;;
    *) fail "the newest scrolled-off line (RING-77) is not in copy-mode" ;;
esac

printf 'q' >&3
sleep 0.4
require_alive "$CPID" "client"
require_alive "$DPID" "daemon"

printf '\x1c\x04' >&3
exec 3>&-
wait "$CPID" || fail "client did not detach cleanly"
"$BIN/agent-terminal" kill -s ring > /dev/null

echo "PASS: copy-mode includes the un-flushed scrollback tail from the daemon's ring (disk had $DISK_LINES/77)"
