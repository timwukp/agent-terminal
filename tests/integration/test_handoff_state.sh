#!/usr/bin/env bash
# PR4: the handoff state parser must never trust its input.
#
# The state file is written by the daemon and read microseconds later by its own
# execv'd image, so in normal operation it is never malformed. This test makes it
# malformed anyway, because the file names raw FILE DESCRIPTOR NUMBERS and a
# daemon that believed them would adopt whatever happened to occupy the slot:
# its own stderr as a PTY master, an unrelated socket as its listener. A crash
# here is a crash on the restart path, and the fallback for "state unusable" has
# to be "start clean", never "exit" — an operator with neither their sessions nor
# a daemon is worse off than one with a daemon and no sessions.
#
# Driving the parser through `agent-terminald --handoff <file>` rather than
# linking handoff.c into a unit test keeps the daemon's objects out of the test
# link line (they are not in a library) and exercises the real main() ordering.
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

RUNDIR="$TMP/.agent-terminal/run"
# 0700, not the umask default: at_runtime_dir refuses a runtime dir with any
# group or other bits (ensure_private_dir in src/common/path.c), so a plain
# `mkdir -p` here makes the daemon die with "Operation not permitted" before it
# ever reads the state file.
mkdir -p -m 0700 "$TMP/.agent-terminal" "$RUNDIR"
chmod 0700 "$TMP/.agent-terminal" "$RUNDIR"
STATE="$RUNDIR/handoff.state"

# Build one state file. Header is 24 bytes:
#   magic "ATHANDF\0", u16 version, u16 count, u32 listen_fd, u32 lock_fd,
#   u32 generation. v2 records: u8 nlen, name, u16 view_cols, u16 view_rows,
#   u8 active_id, u8 last_id, u8 next_id, u8 npanes, layout (1 + 11*12 bytes),
#   then per pane: u8 slot, u8 id, i32 master_fd, i32 pid, u16 cols, u16 rows,
#   u16 x, u16 y, u32 blob_len, blob.
mkstate() { # variant -> writes $STATE
    python3 - "$1" "$STATE" <<'PY'
import struct, sys
variant, path = sys.argv[1], sys.argv[2]

def hdr(count=0, listen=0xFFFFFFFF, lock=0xFFFFFFFF, gen=0, magic=b"ATHANDF\0", ver=2):
    return magic + struct.pack("<HHIII", ver, count, listen, lock, gen)

LAYOUT_NODES = 11

def layout_single_leaf():
    # root node 0: in_use|leaf, pane_idx 0, children 0
    nodes = b""
    nodes += bytes([0]) # root index
    nodes += bytes([1 | 2, 0, 0, 0]) + struct.pack("<HHHH", 0, 0, 80, 24)
    for _ in range(LAYOUT_NODES - 1):
        nodes += bytes([0, 0, 0, 0]) + struct.pack("<HHHH", 0, 0, 0, 0)
    return nodes

def pane_rec(slot=0, pid_=99999, fd=7, cols=80, rows=24, blob=b"", blob_len=None):
    if blob_len is None:
        blob_len = len(blob)
    return (bytes([slot, 0])
            + struct.pack("<iiHHHHI", fd, pid_, cols, rows, 0, 0, blob_len) + blob)

def rec(name=b"s1", fd=7, pid=99999, cols=80, rows=24, blob=b"", blob_len=None):
    return (bytes([len(name)]) + name
            + struct.pack("<HH", cols, rows) + bytes([0, 0, 1, 1])
            + layout_single_leaf()
            + pane_rec(fd=fd, pid_=pid, cols=cols, rows=rows, blob=blob,
                       blob_len=blob_len))

data = {
  # Structurally valid, but every fd it names is a lie: nothing was inherited
  # because nothing exec'd us. The daemon must validate rather than adopt.
  "valid_but_stale_fds": hdr(count=1) + rec(),
  "empty":               b"",
  "short_header":        hdr()[:12],
  "bad_magic":           hdr(magic=b"NOTASTAT"),
  "bad_version":         hdr(ver=99),
  # A v1 file (from the binary being swapped mid-reload): refused by version.
  "v1_file":             hdr(ver=1, count=1) + b"\x02s1"
                          + struct.pack("<iiHHI", 7, 99999, 80, 24, 0),
  # count says 3, file holds one truncated record: must stop, not read past EOF.
  "count_overrun":       hdr(count=3) + rec()[:6],
  # 65535 sessions vs a cap of 64.
  "absurd_count":        hdr(count=0xFFFF) + rec(),
  # A 4 GiB screen blob for a 24-row grid.
  "absurd_blob_len":     hdr(count=1) + rec(blob_len=0xFFFFFFFF),
  # blob_len promises 4096 bytes and delivers 8.
  "truncated_blob":      hdr(count=1) + rec(blob=b"12345678", blob_len=4096),
  "zero_name_len":       hdr(count=1) + (bytes([0])
                          + struct.pack("<iiHHI", 7, 99999, 80, 24, 0)),
  # npanes = 0 and npanes past the cap: both corrupt.
  "zero_panes":          hdr(count=1) + (b"\x02s1" + struct.pack("<HH", 80, 24)
                          + bytes([0, 0, 1, 0]) + layout_single_leaf()),
  "absurd_panes":        hdr(count=1) + (b"\x02s1" + struct.pack("<HH", 80, 24)
                          + bytes([0, 0, 1, 200]) + layout_single_leaf()),
  # A name longer than SESSION_NAME_MAX (63).
  "oversize_name":       hdr(count=1) + rec(name=b"x" * 200),
  # stdio as a PTY master, and as the listener: both must be refused outright.
  "stdio_as_master":     hdr(count=1) + rec(fd=2),
  "stdio_as_listener":   hdr(count=1, listen=1) + rec(),
  # fd 2 is not the lock file; st_dev/st_ino must catch it.
  "wrong_lock_fd":       hdr(count=1, lock=2) + rec(),
  "negative_fds":        hdr(count=1) + rec(fd=-5, pid=-1),
  "pid_zero":            hdr(count=1) + rec(pid=0),
  # Header claims a listener on a plausible-looking high fd.
  "bogus_high_listener": hdr(count=1, listen=200) + rec(),
}[variant]
open(path, "wb").write(data)
PY
}

# Every variant must be rejected FOR ITS OWN REASON, so each names the message
# it must produce. Asserting only the outcome ("started clean, restored nothing")
# is not enough and was measured to be too weak: deleting the magic check
# entirely left this test green, because a header with a bad magic still parses
# as count=0 and the daemon still starts clean. The outcome was right by accident
# and the validation it was supposed to prove was gone.
reason_for() { # variant -> grep -E pattern
    case "$1" in
        empty|short_header|bad_magic) echo "is not a state file" ;;
        bad_version)                  echo "state file version 99" ;;
        v1_file)                      echo "state file version 1" ;;
        zero_panes|absurd_panes)      echo "claims [0-9]+ panes" ;;
        absurd_count)                 echo "claims 65535 sessions, cap is" ;;
        absurd_blob_len)              echo "claims a 4294967295-byte screen" ;;
        count_overrun)                echo "truncated (record|at record)" ;;
        truncated_blob)               echo "truncated screen for" ;;
        zero_name_len|oversize_name)  echo "truncated at record 0 of 1" ;;
        stdio_as_master|negative_fds) echo "refusing fd -?[0-9]+ as a PTY master" ;;
        stdio_as_listener|bogus_high_listener)
                                      echo "is not the listener for" ;;
        wrong_lock_fd)                echo "inherited lock fd 2 unusable" ;;
        valid_but_stale_fds|pid_zero) echo "is not a terminal" ;;
        *) echo "" ;;
    esac
}

VARIANTS="valid_but_stale_fds empty short_header bad_magic bad_version v1_file
          count_overrun absurd_count absurd_blob_len truncated_blob zero_name_len
          oversize_name zero_panes absurd_panes stdio_as_master stdio_as_listener
          wrong_lock_fd negative_fds pid_zero bogus_high_listener"

for V in $VARIANTS; do
    mkstate "$V"
    LOG="$TMP/log_$V"

    "$BIN/agent-terminald" --handoff "$STATE" -v > "$LOG" 2>&1 &
    DPID=$!

    # It must come up and serve. "Started clean" is the requirement, so the
    # check is that a client can talk to it, not merely that it did not crash.
    if ! wait_for "listening on\|reloaded on" "$LOG" 10; then
        echo "--- $V log ---"; cat "$LOG"
        fail "[$V] daemon did not start after a malformed state file"
    fi
    kill -0 "$DPID" 2>/dev/null || fail "[$V] daemon exited instead of starting clean"

    OUT="$("$BIN/agent-terminal" ls 2>&1)" || fail "[$V] daemon is up but not serving: $OUT"
    # No variant names a real PTY, so no session may be restored. A daemon that
    # adopted a bogus fd would list a session whose child does not exist.
    case "$OUT" in
        *"no sessions"*) : ;;
        *) fail "[$V] restored a session from a state file naming no valid fd: $OUT" ;;
    esac

    # The specific rejection, not just a clean start.
    WANT="$(reason_for "$V")"
    [ -n "$WANT" ] || fail "[$V] no expected rejection message declared for this variant"
    grep -qE "$WANT" "$LOG" \
        || { echo "--- $V log ---"; cat "$LOG"
             fail "[$V] state file was not rejected for the expected reason (wanted /$WANT/)"; }

    # A crash under ASan prints a sanitizer report; a plain crash leaves a signal.
    grep -qiE 'AddressSanitizer|runtime error|Sanitizer' "$LOG" \
        && { echo "--- $V log ---"; cat "$LOG"; fail "[$V] sanitizer report on the import path"; }

    # The file must be gone. One that survived would be replayed into a *second*
    # process, handing out fd numbers it does not own.
    [ ! -e "$STATE" ] || fail "[$V] state file was not unlinked on import"

    kill "$DPID" 2>/dev/null
    wait "$DPID" 2>/dev/null
    DPID=""
    # The lock must be released, or the next variant cannot start.
    sleep 0.2
done

# A state file that does not exist at all: --handoff naming a missing path.
"$BIN/agent-terminald" --handoff "$RUNDIR/nonexistent.state" -v > "$TMP/log_missing" 2>&1 &
DPID=$!
wait_for "listening on\|reloaded on" "$TMP/log_missing" 10 \
    || { cat "$TMP/log_missing"; fail "[missing] daemon did not start with no state file"; }
grep -q "state unusable" "$TMP/log_missing" \
    || fail "[missing] daemon did not report the unusable state file"
kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null; DPID=""

N=$(echo $VARIANTS | wc -w | tr -d ' ')
echo "PASS: $N malformed handoff state files + a missing one all start clean," \
     "restore nothing, unlink the file, and never crash"
