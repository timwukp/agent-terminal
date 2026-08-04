# lib.sh — shared setup for integration tests. Sourced, never executed.
#
# Exists because each script used to resolve $BIN itself and default to a
# build variant CI never built: the binaries were simply absent, the daemon
# never started, and the only symptom was "kill: no such process" followed
# by a test-level FAIL. That reads like flakiness and is not. require_bins
# turns a missing build into one unambiguous line.

BUILD="${BUILD:-release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/build/$BUILD"

fail() { echo "FAIL: $1"; exit 1; }

# Assert the binaries under test were actually built for this variant.
require_bins() {
    for b in "$@"; do
        [ -x "$BIN/$b" ] || fail "$BIN/$b missing — run: make BUILD=$BUILD all"
    done
}

# Assert a pid is still alive, naming what died. A daemon that exits during
# startup must not masquerade as a downstream assertion failure.
require_alive() { # pid what
    kill -0 "$1" 2>/dev/null || fail "$2 is not running (exited during startup)"
}

# Poll for a pattern in a file — fixed sleeps are flaky on loaded CI runners.
wait_for() { # pattern file timeout_s
    local i=0
    while [ "$i" -lt "$(($3 * 10))" ]; do
        grep -q "$1" "$2" 2>/dev/null && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
