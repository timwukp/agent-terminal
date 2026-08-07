#!/usr/bin/env bash
# Starting either binary with a std fd closed must not turn that descriptor into
# an internal self-pipe.
#
# Both binaries create a self-pipe during startup — the client for SIGWINCH
# (attach.c), the daemon for signals (main.c) — and pipe() returns the LOWEST
# free descriptors. With fd 0 closed, stdin therefore *became* the read end of
# that pipe. The client polled a pipe nobody writes and blocked forever at 0.0%
# CPU instead of detaching, and the pipe's write end is called from a signal
# handler, so anything that did read "stdin" would be reading SIGWINCH
# notifications. Measured before the fix: lsof showed `fd 0 PIPE` and
# `fd 3 PIPE` — the pair split across 0 and 3.
#
# `sh -c 'exec 0<&-; exec prog'` is the reliable way to hand a program a closed
# fd 0. A bare `prog <&-` in a *backgrounded* shell job is not: bash redirects a
# backgrounded job's stdin to /dev/null when job control is off, so the fd is
# open after all and the case silently does not run. That mistake made a mutation
# look caught when both builds were failing identically for an unrelated reason.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    pkill -9 -f "new -s fd0" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# --- 1. the daemon, started with fd 0 closed --------------------------------
# It must come up and serve. The warning is asserted too: this condition means
# something is wrong with whatever started the daemon, and silently papering
# over it would hide that from an operator.
sh -c "exec 0<&-; exec '$BIN/agent-terminald' -f -v" > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon with fd 0 closed never listened: $(cat "$TMP/daemon.log")"
require_alive "$DPID" "daemon"

grep -q "reopened 1 closed std fd" "$TMP/daemon.log" \
    || fail "daemon did not report reopening fd 0: $(cat "$TMP/daemon.log")"

"$BIN/agent-terminal" ls > "$TMP/ls0.out" 2>&1 \
    || fail "ls failed against a daemon started with fd 0 closed: $(cat "$TMP/ls0.out")"

# --- 2. the client, started with fd 0 closed --------------------------------
# `new` with an unreadable stdin is a detach, exactly as /dev/null is: create the
# session, report it, exit 0. Pre-fix this hung until killed.
#
# Timeout by hand — the failure is a hang, and `timeout` is absent on stock
# macOS. Run in a background subshell and poll for it.
(
    sh -c "exec 0<&-; exec '$BIN/agent-terminal' new -s fd0 -- /bin/sleep 300" \
        > "$TMP/cli.out" 2>&1
    echo $? > "$TMP/cli.rc"
) &
CPID=$!
i=0
while [ "$i" -lt 100 ]; do
    kill -0 "$CPID" 2>/dev/null || break
    sleep 0.1
    i=$((i + 1))
done
if kill -0 "$CPID" 2>/dev/null; then
    # Report the fd that proves the mechanism, not just "it hung", so the next
    # reader does not have to rediscover why.
    RP=$(pgrep -f "new -s fd0" | head -1)
    FD0=$(lsof -p "$RP" 2>/dev/null | awk '$4 == "0" {print $5; exit}')
    kill -9 "$CPID" 2>/dev/null
    pkill -9 -f "new -s fd0" 2>/dev/null
    fail "client with fd 0 closed never exited (fd 0 is a ${FD0:-unknown} — a self-pipe means the startup guard is gone)"
fi
wait "$CPID" 2>/dev/null

RC=$(cat "$TMP/cli.rc" 2>/dev/null || echo missing)
[ "$RC" = "0" ] || fail "client with fd 0 closed exited $RC: $(cat "$TMP/cli.out")"

# The session must really exist. The client claiming success for a session the
# daemon never made is a separate bug this suite already covers, so assert the
# daemon's own log and `ls`, not just the exit code.
grep -q "session 'fd0': pid" "$TMP/daemon.log" \
    || fail "client with fd 0 closed exited 0 but created nothing: $(cat "$TMP/cli.out")"
"$BIN/agent-terminal" ls 2>/dev/null | grep -q '^fd0:' \
    || fail "session fd0 missing from ls"

# --- 3. the mechanism itself, not just its symptom ---------------------------
# The daemon is still running with the fd it was repaired at startup, so inspect
# it directly: fd 0 must be /dev/null (CHR), never a PIPE. Parts 1 and 2 assert
# outcomes, and an outcome can be right by accident — a client that exited for
# some unrelated reason passes part 2. This is what pins the cause.
#
# lsof is not on every minimal image, so skip rather than fail when it is absent:
# a missing tool must not be reported as a passing assertion.
# lsof appends the access mode to the fd number ("0u", "1w"), so match the
# number with the mode stripped — `$4 == "0"` matches nothing and made this whole
# check skip silently, which is exactly the failure mode a skip-on-absent branch
# invites. Hence the explicit "found no fd 0 at all" failure below.
if command -v lsof > /dev/null; then
    DFD0=$(lsof -p "$DPID" 2>/dev/null | awk '{ fd = $4; sub(/[a-zA-Z]+$/, "", fd);
                                                if (fd == "0") { print $5; exit } }')
    case "$DFD0" in
        PIPE|FIFO)
            fail "daemon fd 0 is a $DFD0 — the signal self-pipe landed on stdin" ;;
        CHR)
            : ;; # /dev/null, as intended
        "")
            fail "lsof found no fd 0 for the running daemon — it should have been reopened onto /dev/null" ;;
        *)
            fail "daemon fd 0 is $DFD0, expected CHR (/dev/null)" ;;
    esac
else
    echo "note: lsof absent; skipping the fd-type check"
fi

require_alive "$DPID" "daemon"
echo "PASS: daemon and client both start with fd 0 closed; stdin is not the self-pipe"
