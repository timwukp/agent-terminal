#!/usr/bin/env bash
# Soak: 100 MB of output through a session; daemon RSS must stay bounded
# and scrollback files must respect the rotation cap.
set -u

BUILD="${BUILD:-release}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/$BUILD"
TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
fail() { echo "FAIL: $1"; exit 1; }
trap cleanup EXIT

"$BIN/agent-terminald" -f > "$TMP/daemon.log" 2>&1 &
DPID=$!
sleep 0.3

# ~100 MB: 1.2M lines x ~84 bytes, with SGR color churn to stress the pen.
mkfifo "$TMP/in1"
"$BIN/agent-terminal" new -s soak -- bash -c '
  i=0
  while [ $i -lt 1200000 ]; do
    printf "\033[3%dmline %d: 0123456789012345678901234567890123456789012345678901234567890123\033[0m\n" $((i % 8)) $i
    i=$((i + 1))
  done
  echo SOAK-DONE
  sleep 30' < "$TMP/in1" > /dev/null 2>&1 &
C1=$!
exec 3>"$TMP/in1"

PEAK_RSS=0
for _ in $(seq 1 120); do
    RSS=$(ps -o rss= -p "$DPID" 2>/dev/null | tr -d ' ')
    [ -z "$RSS" ] && fail "daemon died during soak"
    [ "$RSS" -gt "$PEAK_RSS" ] && PEAK_RSS=$RSS
    "$BIN/agent-terminal" history -s soak 2>/dev/null | tail -1 | grep -q "SOAK-DONE" && break
    sleep 2
done

kill -9 "$C1" 2>/dev/null
exec 3>&-

# RSS bound: grid + 10k-line ring + buffers. 128 MiB is generous headroom;
# unbounded growth would blow past it (100 MB input!).
PEAK_MB=$((PEAK_RSS / 1024))
[ "$PEAK_MB" -le 128 ] || fail "daemon RSS peaked at ${PEAK_MB} MiB (>128)"

# Disk cap: 2 x 32 MiB + slack.
TOTAL=$(du -sk "$TMP/.agent-terminal/sessions/soak" | cut -f1)
TOTAL_MB=$((TOTAL / 1024))
[ "$TOTAL_MB" -le 70 ] || fail "scrollback dir is ${TOTAL_MB} MiB (>70)"

echo "PASS: soak — peak RSS ${PEAK_MB} MiB, scrollback dir ${TOTAL_MB} MiB"
