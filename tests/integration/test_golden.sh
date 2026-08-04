#!/usr/bin/env bash
# Golden replay: real vttest captures fed through libvt must reproduce the
# committed grid dumps byte-for-byte. Catches VT-engine regressions with
# real-terminal-suite traffic, not synthetic cases.
set -u

. "$(dirname "$0")/lib.sh"
FAIL=0

make -C "$ROOT" tools BUILD="$BUILD" > /dev/null 2>&1
require_bins vtdump

for rec in "$ROOT"/tests/data/recordings/*.raw; do
    base=$(basename "$rec" .raw)
    golden="$ROOT/tests/data/golden/$base.txt"
    [ -f "$golden" ] || { echo "SKIP $base (no golden)"; continue; }
    got=$("$BIN/vtdump" -r 24 -c 80 "$rec" 2>/dev/null)
    want=$(cat "$golden")
    if [ "$got" = "$want" ]; then
        echo "ok   $base"
    else
        echo "FAIL $base"
        diff <(echo "$want") <(echo "$got") | head -10
        FAIL=1
    fi
done

[ "$FAIL" -eq 0 ] && echo "PASS: all golden replays match" || echo "FAIL: golden mismatch"
exit $FAIL
