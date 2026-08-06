#!/usr/bin/env bash
# Scrollback copy-mode: Ctrl-\ [ must show history that has scrolled off a
# LIVE session, and q must restore the live screen.
#
# The load-bearing property is that this is impossible without the feature: a
# client with no pager forwards "\x1c[" to the child, which echoes it back, so
# an old line can never appear on screen while the session is still running.
# Both halves are asserted — the old line appears, AND the pager's own control
# sequences appear — because either alone could pass for the wrong reason.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal

TMP="$(mktemp -d)"
export HOME="$TMP"            # isolate runtime dir + socket
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
require_alive "$DPID" "daemon"

OUT="$TMP/client.out"
mkfifo "$TMP/in"

# A 24-row PTY, so ~200 printed lines push the early ones well off screen.
# `head -c` on the client output keeps the file bounded if anything loops.
"$BIN/agent-terminal" new -s pg -- bash --norc -c '
    for i in $(seq 1 200); do echo "LINE-$i"; done
    exec sleep 300
' < "$TMP/in" > "$OUT" 2>&1 &
CPID=$!
exec 3>"$TMP/in"

wait_for "LINE-200" "$OUT" 10 || fail "child output never reached the client"
require_alive "$CPID" "client"

# Lines are only durable on the daemon's 1 s flush tick; copy-mode also pulls
# the unflushed tail over the wire, but waiting makes the disk half real too.
sleep 1.5

# ---- 1. mark the transcript, and confirm nothing is arriving on its own -----
# $OUT is a transcript, not a screen: LINE-1 is in it from the initial burst.
# So every assertion below looks only at the bytes written AFTER this mark.
# LINE-1[^0-9] avoids matching LINE-10..LINE-19; the terminator is \r when the
# child printed it live and ESC when the pager redrew it.
BEFORE_BYTES=$(( $(wc -c < "$OUT") + 1 ))

# Nothing must arrive while the session sits in `sleep 300`; otherwise a "the
# tail contains LINE-1" assertion could pass on unrelated late output.
sleep 0.5
IDLE_TAIL=$(tail -c "+$BEFORE_BYTES" "$OUT" | wc -c | tr -d ' ')
[ "$IDLE_TAIL" -eq 0 ] || fail "session is still emitting output ($IDLE_TAIL bytes); test cannot isolate the pager"

# ---- 2. enter copy-mode and page to the very top ---------------------------
printf '\x1c[' >&3          # prefix Ctrl-\ then [
sleep 0.5
printf 'g' >&3              # jump to the oldest line
sleep 0.7
PAGER_TAIL=$(tail -c "+$BEFORE_BYTES" "$OUT")
# The oldest scrolled-off line must be back on screen. Impossible without the
# feature: "\x1c[" would have gone to the child and echoed back instead.
case "$PAGER_TAIL" in
    *LINE-1[!0-9]*) : ;;
    *) fail "copy-mode did not show scrolled-off history (LINE-1)" ;;
esac

# The pager must have taken over the terminal, not just echoed bytes: alt
# screen on and autowrap off. Without the autowrap reset a stored line wider
# than the terminal would occupy two rows and desynchronise everything below.
case "$PAGER_TAIL" in
    *$'\x1b[?1049h'*) : ;;
    *) fail "copy-mode never entered the alternate screen" ;;
esac
case "$PAGER_TAIL" in
    *$'\x1b[?7l'*) : ;;
    *) fail "copy-mode did not disable autowrap" ;;
esac
# The status line reports the position, and 'g' means we are at the top.
case "$PAGER_TAIL" in
    *scrollback*1-*) : ;;
    *) fail "copy-mode status line missing or not at the top after 'g'" ;;
esac

# The keystrokes must NOT have reached the child. If they had, bash would have
# reported a command-not-found for the g/j/q characters.
grep -qi "command not found" "$OUT" && fail "copy-mode keys leaked to the child"

# ---- 3. scrolling moves the view ------------------------------------------
# Scrollback holds only what scrolled OFF: 200 printed lines through a 24-row
# screen leaves the last 23 on the live screen, so history is LINE-1..LINE-177
# and the newest scrollback line is 177, not 200. Asserting the count as well as
# the line makes that arithmetic a checked claim rather than a lucky substring.
MID_BYTES=$(( $(wc -c < "$OUT") + 1 ))
printf 'G' >&3              # back to the newest line
sleep 0.7
TAIL2=$(tail -c "+$MID_BYTES" "$OUT")
case "$TAIL2" in
    *LINE-177*) : ;;
    *) fail "'G' did not scroll back to the newest scrollback line (LINE-177)" ;;
esac
case "$TAIL2" in
    *"/177 (100%)"*) : ;;
    *) fail "'G' did not land at the end of scrollback (expected .../177 (100%))" ;;
esac
# The view moved: the top page is gone from the newly drawn frame.
case "$TAIL2" in
    *LINE-2[!0-9]*) fail "'G' redrew the top of scrollback instead of the bottom" ;;
    *) : ;;
esac

# ---- 4. q leaves copy-mode and the live screen is restored ----------------
EXIT_BYTES=$(( $(wc -c < "$OUT") + 1 ))
printf 'q' >&3
sleep 0.7
EXIT_TAIL=$(tail -c "+$EXIT_BYTES" "$OUT")
# Match pager_leave's exact ordered sequence, not the individual sequences: the
# daemon's own repaint snapshot emits "?1049l" and "?7h" too (vt_render.c:131,157),
# so asserting them separately passes even with pager_leave emitting nothing.
# Autowrap must be restored BEFORE leaving the alt screen, or a client that dies
# in between leaves the user's real screen unable to wrap.
case "$EXIT_TAIL" in
    *$'\x1b[?7h\x1b[0m\x1b[?25h\x1b[?1049l'*) : ;;
    *) fail "quitting copy-mode did not restore the terminal (expected ?7h ?25h ?1049l in order)" ;;
esac

# The repaint is a real daemon snapshot, not the terminal's saved alt buffer:
# the client detaches and re-attaches, so the session must still be listed with
# its original pid and the client must still be attached afterwards.
"$BIN/agent-terminal" ls | grep -Eq "pg: .*pid [0-9]+, 1 client" \
    || fail "after copy-mode the client is no longer attached (ls: $("$BIN/agent-terminal" ls))"

# ---- 5. the session still works ------------------------------------------
require_alive "$CPID" "client"
require_alive "$DPID" "daemon"

# ---- 6. detach still works (the chord machine was modified) --------------
printf '\x1c\x04' >&3
exec 3>&-
wait "$CPID"
RC=$?
[ "$RC" -eq 0 ] || fail "detach chord broke after adding copy-mode (rc=$RC)"

"$BIN/agent-terminal" ls | grep -q "pg:" || fail "session died on detach"
"$BIN/agent-terminal" kill -s pg > /dev/null

echo "PASS: copy-mode shows scrolled-off history on a live session, restores it on exit"
