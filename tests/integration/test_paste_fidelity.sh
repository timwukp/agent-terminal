#!/usr/bin/env bash
# A paste larger than the kernel PTY input queue (~1KiB on macOS) must arrive
# whole. Before the stdin staging buffer, pane_stdin() broke out of its write
# loop on EAGAIN and the tail of every paste silently vanished — measured
# live: 1016 bytes delivered of a multi-KB paste, four times at the same
# offset. This sends 64KiB through the client in one shot and requires the
# child to receive every byte, in order.
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

# 64 KiB of position-verifiable pattern: 4096 numbered 16-byte lines. Any
# dropped or reordered byte shifts every line after it and cmp names the spot.
awk 'BEGIN{for(i=0;i<4096;i++)printf "%015d\n", i}' > "$TMP/pattern.bin"
[ "$(wc -c < "$TMP/pattern.bin")" -eq 65536 ] || fail "pattern generation is off"

OUT="$TMP/out.bin"

# Raw mode, no echo: the same tty state an interactive CLI (the real victim
# of the truncation) runs in, and the only mode where the canonical line
# limit cannot interfere with what this test measures.
mkfifo "$TMP/in1"
"$BIN/agent-terminal" new -s paste -- \
    bash -c "stty raw -echo; exec cat > '$OUT'" \
    < "$TMP/in1" > /dev/null 2>&1 &
exec 3>"$TMP/in1"

# Probe before flooding: input sent while the attach handshake is still in
# flight is a DIFFERENT (pre-existing) hazard, and this test measures the PTY
# write path, not the attach race. A byte that comes back proves the whole
# pipe — client, daemon, stty raw, cat — is warm.
# The probe RETRIES: input sent while the attach handshake is in flight is
# dropped on trees without the staging fix (measured: an immediate 6-byte
# READY never arrives on such a tree; the same bytes 2s later do). Retrying
# makes the probe converge on any tree, so a tree WITHOUT the fix fails at
# the truncation assertion below — the claim this test exists for — instead
# of failing here at bring-up.
deadline=$(( $(date +%s) + 15 ))
while :; do
    printf 'READY\n' >&3
    sleep 0.3
    got=$(wc -c < "$OUT" 2>/dev/null || echo 0)
    [ "$got" -ge 6 ] && break
    [ "$(date +%s)" -ge "$deadline" ] && fail "probe never reached the child — session did not come up"
done
# Several probes may have landed; snapshot what arrived so the flood
# accounting below is exact regardless.
sleep 0.5
probe_len=$(wc -c < "$OUT")

# dd bs=4096, NOT `cat`: cat issues one 64KiB write(), and macOS poll() on a
# FIFO under-reports POLLIN while a writer sits blocked in a single write
# larger than the pipe buffer — the reader stalls after ~8KiB with data still
# queued (verified with a 30-line C probe; the writer never unblocks). That is
# an OS quirk of the TEST TRANSPORT, not of the daemon under test: the real
# client feeds stdin over a Unix socket. 4KiB writes keep the FIFO inside the
# well-behaved regime and still flood the ~1KiB PTY queue 4x over per frame,
# which is exactly what exercises the staging path.
dd if="$TMP/pattern.bin" bs=4096 >&3 2>/dev/null

# The drain is event-driven; 64KiB at ~1KiB per queue-fill takes well under a
# second once POLLOUT fires, but give slow CI 15s.
deadline=$(( $(date +%s) + 15 ))
while :; do
    got=$(wc -c < "$OUT" 2>/dev/null || echo 0)
    [ "$got" -ge $((probe_len + 65536)) ] && break
    [ "$(date +%s)" -ge "$deadline" ] && fail "child received $((got - probe_len)) of 65536 flood bytes — the paste tail was dropped"
    sleep 0.2
done

# Byte-exact: strip the probe, then any dropped or reordered byte shifts every
# numbered line after it and cmp names the offset.
tail -c 65536 "$OUT" > "$TMP/flood.bin"
cmp "$TMP/pattern.bin" "$TMP/flood.bin" || fail "bytes arrived but differ from what was sent (reorder or corruption)"

exec 3>&-
echo "PASS: a 64 KiB paste arrives byte-exact — the PTY queue overflow is staged, not dropped"
