#!/usr/bin/env bash
# PR4 acceptance: a GRACEFUL daemon restart keeps every child process alive.
#
# The daemon re-execs itself in place, so its pid does not change — that is the
# mechanism, not an oversight. Which makes "did it actually restart?" the hard
# part of this test: the only observable that moves is the generation counter in
# HELLO_OK, which `reload` reports and the log line records. A test that only
# checked "session still there" would pass against a daemon that ignored the
# request entirely.
#
# Both trigger paths are covered: SIGHUP (an operator, or a service manager's
# reload verb) and `agent-terminal reload` (the protocol path).
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

# Bare argv[0] + a cwd far from the binary, exactly like client autospawn:
# resolve_exe() must find its own executable WITHOUT an absolute argv[0] or
# /proc (absent on macOS). Starting with an absolute path here was the blind
# spot that let the macOS reload break ship — the harness was kinder than
# reality (every real daemon is autospawned with the bare name).
(cd / && PATH="$BIN:$PATH" exec agent-terminald -f -v) > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 10 || fail "daemon did not start"
require_alive "$DPID" "daemon"

MARKER="$TMP/marker"

# A child that keeps writing proves it is not merely *present* but *running*:
# a stopped or zombie process would still satisfy `kill -0`. It also prints a
# distinctive line first, so the screen the daemon carries over is checkable.
#
# 250 lines through a 24-row PTY also puts ~226 lines into scrollback before the
# restart, which is what makes the seq-continuity assertion below meaningful.
mkfifo "$TMP/in1"
"$BIN/agent-terminal" new -s rst -- bash -c "
    printf '\033[1;33mBEFORE-RESTART-MARKER\033[0m\n'
    seq -f 'pre-line-%.0f' 1 250
    i=0
    while true; do i=\$((i+1)); echo \"tick \$i\" >> '$MARKER'; sleep 0.2; done" \
    < "$TMP/in1" > /dev/null 2>&1 &
C1=$!
exec 3>"$TMP/in1"

wait_for "tick 3" "$MARKER" 10 || fail "child never started writing the marker"
sleep 1.2   # > 1s flush tick, so pre-restart scrollback is on disk

CHILD_BEFORE="$("$BIN/agent-terminal" ls | sed -n 's/^rst: .*pid \([0-9]*\).*/\1/p')"
[ -n "$CHILD_BEFORE" ] || fail "could not read the child pid before restart"
DAEMON_BEFORE="$DPID"

# Socket identity: an adopted listener keeps its inode. A daemon that rebound
# would still serve clients, so nothing else here would notice — but every
# connection queued at the moment of the restart would have been dropped.
SOCK="$TMP/.agent-terminal/run/default.sock"
[ -S "$SOCK" ] || fail "socket $SOCK missing"
SOCK_INO_BEFORE="$(ls -i "$SOCK" | awk '{print $1}')"

TICKS_BEFORE="$(wc -l < "$MARKER")"
SB_BEFORE="$("$BIN/agent-terminal" history -s rst | grep -c 'pre-line-' || true)"
[ "$SB_BEFORE" -ge 150 ] || fail "expected >=150 scrollback lines before restart, got $SB_BEFORE"

# ---- path 1: SIGHUP ----------------------------------------------------------

kill -HUP "$DPID"
wait_for "reloaded on" "$TMP/daemon.log" 10 || fail "daemon did not report a reload after SIGHUP"

# Exact value, not just "it moved". One restart must advance the counter by
# exactly one: the export writes its own generation plus one and the import
# adopts that verbatim. An earlier revision also incremented on import, so every
# reload advanced by two — invisible to any assertion that only checks for an
# increase, and it makes the number useless for saying *how many* reloads ran.
grep -q "generation 1," "$TMP/daemon.log" \
    || fail "first restart did not land on generation 1: $(grep 'reloaded on' "$TMP/daemon.log")"

grep -q "adopted inherited listener" "$TMP/daemon.log" \
    || fail "listener was rebound rather than inherited across the restart"

# The re-exec keeps the pid, so the shell's job is still the daemon.
require_alive "$DAEMON_BEFORE" "daemon (after SIGHUP re-exec)"

CHILD_AFTER="$("$BIN/agent-terminal" ls | sed -n 's/^rst: .*pid \([0-9]*\).*/\1/p')"
[ -n "$CHILD_AFTER" ] || fail "session rst is gone after the restart"
[ "$CHILD_AFTER" = "$CHILD_BEFORE" ] \
    || fail "child pid changed $CHILD_BEFORE -> $CHILD_AFTER: it was respawned, not preserved"
kill -0 "$CHILD_BEFORE" 2>/dev/null || fail "child $CHILD_BEFORE died across the restart"

SOCK_INO_AFTER="$(ls -i "$SOCK" | awk '{print $1}')"
[ "$SOCK_INO_AFTER" = "$SOCK_INO_BEFORE" ] \
    || fail "socket inode changed $SOCK_INO_BEFORE -> $SOCK_INO_AFTER: listener was rebound"

# Alive is not the same as running. The marker must keep growing on its own.
TICKS_MID="$(wc -l < "$MARKER")"
wait_for_growth() { # file baseline timeout_s
    local i=0
    while [ "$i" -lt "$(($3 * 10))" ]; do
        [ "$(wc -l < "$1")" -gt "$2" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
wait_for_growth "$MARKER" "$TICKS_MID" 10 \
    || fail "child survived but stopped producing output (SIGSTOP'd or lost its PTY)"
[ "$TICKS_MID" -ge "$TICKS_BEFORE" ] || fail "marker file was truncated across the restart"

# Scrollback numbering must continue, not restart from zero: sb_open resumes
# next_seq from what is already on disk. A restart that reset it would silently
# overwrite pre-restart history.
SB_AFTER="$("$BIN/agent-terminal" history -s rst | grep -c 'pre-line-' || true)"
[ "$SB_AFTER" -ge "$SB_BEFORE" ] \
    || fail "scrollback shrank across the restart ($SB_BEFORE -> $SB_AFTER): seq numbering reset"
"$BIN/agent-terminal" history -s rst | grep -q 'pre-line-1$' \
    || fail "earliest pre-restart scrollback line lost"

# A client attaching after the restart must receive the screen the daemon
# carried over — this is what proves the vt_snapshot blob made the trip, not
# just the file descriptor.
mkfifo "$TMP/in2"
"$BIN/agent-terminal" attach -s rst < "$TMP/in2" > "$TMP/out2" 2>&1 &
C2=$!
exec 4>"$TMP/in2"
sleep 2
printf '\x1c\x04' >&4   # detach
exec 4>&-
wait "$C2" 2>/dev/null

# BEFORE-RESTART-MARKER has long scrolled off a 24-row screen, so the live
# screen shows the tail. `tick` output goes to the marker file, not the PTY,
# so the visible screen is the last rows of the seq run.
grep -q "pre-line-250" "$TMP/out2" \
    || fail "post-restart snapshot does not contain pre-restart screen content"

# ---- path 2: the reload verb ------------------------------------------------

GEN_LINES_BEFORE="$(grep -c 'reloaded on' "$TMP/daemon.log")"
RELOAD_OUT="$("$BIN/agent-terminal" reload 2>&1)" || fail "reload verb exited nonzero: $RELOAD_OUT"
echo "$RELOAD_OUT" | grep -q "generation 2" \
    || fail "reload did not report generation 2 (got: $RELOAD_OUT)"

# The verb's own success criterion is the generation moving, so also require a
# second log line: a reload that reported success without re-execing would be
# the exact failure this whole PR must not ship.
[ "$(grep -c 'reloaded on' "$TMP/daemon.log")" -gt "$GEN_LINES_BEFORE" ] \
    || fail "reload verb returned success but the daemon never re-execed"

CHILD_FINAL="$("$BIN/agent-terminal" ls | sed -n 's/^rst: .*pid \([0-9]*\).*/\1/p')"
[ "$CHILD_FINAL" = "$CHILD_BEFORE" ] \
    || fail "child pid changed across the reload verb ($CHILD_BEFORE -> $CHILD_FINAL)"

TICKS_FINAL="$(wc -l < "$MARKER")"
wait_for_growth "$MARKER" "$TICKS_FINAL" 10 \
    || fail "child stopped producing output after the reload verb"

# Job-control notices for our own deliberate kill would otherwise print a
# multi-line "Killed: 9" banner that buries a real failure message.
set +m
kill -9 "$C1" 2>/dev/null
exec 3>&-
wait "$C1" 2>/dev/null
"$BIN/agent-terminal" kill -s rst > /dev/null 2>&1

# ---- the single-instance lock ------------------------------------------------
#
# Last, because the decisive case unlinks the socket and no client can reach the
# daemon afterwards.
#
# A second daemon must refuse to start rather than bind over the socket and sit
# there invisibly, still parenting its own children and still holding their PTY
# masters — a state in which "re-exec the daemon" has no defined meaning.

if "$BIN/agent-terminald" -f > "$TMP/second.log" 2>&1; then
    fail "a second daemon started successfully; the single-instance lock does not hold"
fi
# The specific message matters. server.c's probe-connect ALSO refuses with
# "daemon already running on <socket>", so grepping for "already running" alone
# passes with no lock at all — measured: removing lock_acquire left that
# assertion green. Only the lock path suggests `reload`.
grep -q "restarts it in place" "$TMP/second.log" \
    || fail "second daemon was refused by the socket probe, not the lock: $(cat "$TMP/second.log")"

# The case the probe cannot cover, and the reason the lock exists. With the
# socket gone there is nothing to connect to, so a daemon relying on the probe
# would find "nothing answering", bind a fresh socket, and run as a second
# invisible instance. The flock is held against an open file description and
# does not care that the socket is missing.
rm -f "$SOCK"
[ ! -e "$SOCK" ] || fail "could not remove the socket for the lock test"
if "$BIN/agent-terminald" -f > "$TMP/third.log" 2>&1; then
    fail "a second daemon started once the socket was removed: exclusion depends on the socket probe, which is racy"
fi
grep -q "restarts it in place" "$TMP/third.log" \
    || fail "socketless second daemon failed for the wrong reason: $(cat "$TMP/third.log")"

# And the recorded pid must name the daemon that actually holds the lock,
# otherwise the message sends the operator after an unrelated process.
LOCK_PID="$(sed -n 1p "$TMP/.agent-terminal/run/daemon.lock" 2>/dev/null)"
[ "$LOCK_PID" = "$DAEMON_BEFORE" ] \
    || fail "daemon.lock records pid '$LOCK_PID', but the daemon is $DAEMON_BEFORE"
echo "PASS: child pid $CHILD_BEFORE survived 2 in-place restarts (SIGHUP + reload verb);" \
     "listener inherited, scrollback continuous, snapshot carried over, lock holds"
