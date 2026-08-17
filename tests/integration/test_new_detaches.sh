#!/usr/bin/env bash
# MSG_NEW_SESSION must leave the session this connection was on, the way
# MSG_ATTACH already does. Without that detach the old session keeps this
# client in its clients[] forever, and g_clients is a static table of REUSED
# slots — so the stale entry stops naming a dead client and starts naming
# whoever lands in that slot next.
#
# This is the mirror image of test_slot_reuse.sh: there a client held a stale
# pointer to a reused SESSION slot, here a session holds a stale pointer to a
# reused CLIENT slot. Same static-table shape, opposite direction.
#
# Four properties, each aimed at a different wrong implementation. All four
# were confirmed to go red on their own against the unfixed daemon, with the
# earlier assertions relaxed so each one had to fail for its own reason:
#
#   1. After NEW a; NEW b on one connection, the list reports ZERO clients on
#      'a'. This is the cheap marker, and it stays wrong after the connection
#      closes: handle_list counts non-NULL pointers, and client_disconnect
#      only detaches from c->attached, which by then is 'b'.
#   2. A FRESH connection that never asked for 'a' receives none of its
#      output. This is the harm, not the count: 'a' runs a child that prints
#      continuously, the leaking connection closes so its slot is free, and
#      the next client to connect takes that slot. Measured against the
#      unfixed daemon: 7 MSG_OUTPUT frames from a session that client never
#      named. An implementation that only fixed the count still fails here.
#   3. That same connection can then attach to 'a' for real and get a
#      snapshot. session_attach returns early when the client is already in
#      clients[], so the stale entry turns a legitimate attach into silence —
#      a terminal that never paints, which is what a user would report.
#   4. ATTACH a; NEW c leaks the same way. The detach in handle_attach covers
#      attach-then-attach only, so a fix that trusts it fails this.
#
# The clients here are wire clients, not `agent-terminal ls`, because the CLI
# would take a slot of its own and the slot the daemon hands out next is what
# property 2 is about.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald
command -v python3 > /dev/null || fail "python3 required (already a gate dependency)"

TMP="$(mktemp -d)"
export HOME="$TMP"
unset XDG_RUNTIME_DIR

DPID=""
cleanup() {
    # Kill by tracked pid, never by name — a broad pkill hits the production
    # daemon on this machine (it has).
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

SHELL=/bin/sh "$BIN/agent-terminald" -f -v > "$TMP/daemon.log" 2>&1 &
DPID=$!
wait_for "listening on" "$TMP/daemon.log" 5 \
    || fail "daemon never logged a listen: $(cat "$TMP/daemon.log")"

python3 - "$TMP/.agent-terminal/run/default.sock" <<'PY' || fail "wire probe failed"
import socket, struct, sys, time

sock_path = sys.argv[1]

HELLO, HELLO_OK = 0x01, 0x02
LIST2_REQ, SESSION_LIST2 = 0x1a, 0x37
NEW, KILL, ATTACH = 0x12, 0x13, 0x14
OUTPUT, SNAPSHOT = 0x30, 0x31
CAP_PANES = 0x0001

# Something that keeps printing on its own, so property 2 does not depend on
# anyone typing into the session.
NOISY = ['/bin/sh', '-c', 'while :; do printf LEAK; sleep 0.2; done']


def frame(t, payload=b''):
    return struct.pack('<IB', len(payload), t) + payload


def read_exactly(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_frame(s):
    hdr = read_exactly(s, 5)
    if hdr is None:
        return None, None
    plen, typ = struct.unpack('<IB', hdr)
    return typ, (read_exactly(s, plen) if plen else b'')


def connect(flags=CAP_PANES):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.sendall(frame(HELLO, struct.pack('<HH', 1, flags)))
    typ, _ = read_frame(s)
    assert typ == HELLO_OK, f"expected HELLO_OK, got {typ}"
    return s


def new_frame(name, argv=NOISY, cols=80, rows=24):
    nb = name.encode()
    ab = b''.join(a.encode() + b'\0' for a in argv)
    return frame(NEW, struct.pack('<HHB', cols, rows, len(nb)) + nb
                 + struct.pack('<H', len(ab)) + ab)


def attach_frame(name, cols=80, rows=24):
    nb = name.encode()
    return frame(ATTACH, struct.pack('<HHBB', cols, rows, 0, len(nb)) + nb)


def kill_frame(name):
    nb = name.encode()
    return frame(KILL, bytes([len(nb)]) + nb)


def wait_for_type(s, want, seconds=5):
    end = time.time() + seconds
    s.settimeout(0.2)
    got = None
    while time.time() < end:
        try:
            typ, payload = read_frame(s)
        except socket.timeout:
            continue
        if typ is None:
            break
        if typ == want:
            got = payload
            break
    s.settimeout(10)
    return got


def clients_per_session(s):
    """{name: nclients} from MSG_SESSION_LIST2, honouring entry_len."""
    s.sendall(frame(LIST2_REQ))
    payload = wait_for_type(s, SESSION_LIST2)
    assert payload is not None, "no MSG_SESSION_LIST2 came back"
    count = struct.unpack('<H', payload[:2])[0]
    off, out = 2, {}
    for _ in range(count):
        entry_len = struct.unpack('<H', payload[off:off + 2])[0]
        e = payload[off + 2:off + 2 + entry_len]
        off += 2 + entry_len
        nlen = e[0]
        name = e[1:1 + nlen].decode()
        # nlen + name + cols + rows + alive, then nclients
        out[name] = e[1 + nlen + 5]
    return out


def drain(s, seconds):
    out = []
    end = time.time() + seconds
    s.settimeout(0.2)
    while time.time() < end:
        try:
            typ, payload = read_frame(s)
        except socket.timeout:
            continue
        if typ is None:
            break
        out.append((typ, payload))
    s.settimeout(10)
    return out


# --- 1: creating a second session leaves the first --------------------------
leaker = connect()
leaker.sendall(new_frame('a'))
assert wait_for_type(leaker, SNAPSHOT) is not None, "NEW a never attached us"
leaker.sendall(new_frame('b'))
assert wait_for_type(leaker, SNAPSHOT) is not None, "NEW b never attached us"

seen = clients_per_session(leaker)
assert set(seen) == {'a', 'b'}, f"unexpected session table: {seen}"
assert seen['b'] == 1, f"the creating client is not attached to 'b': {seen}"
assert seen['a'] == 0, (
    f"session 'a' still counts the client that moved on to 'b': {seen} — "
    "handle_new set c->attached without detaching")
print("ok: NEW b left session 'a' with 0 clients")

# --- 2: the freed slot does not inherit 'a' ---------------------------------
# 'a' is printing every 200 ms. Close the leaking connection so its slot is
# the next one handed out, then take that slot with a client that asks for
# nothing. A stale pointer in a->clients[] now names THIS client.
leaker.close()
time.sleep(0.5)
stranger = connect()
after = clients_per_session(stranger)
assert after['a'] == 0, (
    f"session 'a' still counts a client after the leak closed: {after} — "
    "the pointer outlived the connection")
frames = drain(stranger, 2.0)
strays = [t for t, _ in frames if t in (OUTPUT, SNAPSHOT)]
assert not strays, (
    f"a client that never attached received {len(strays)} render frames "
    f"({[hex(t) for t in strays]}) — it inherited a stale clients[] entry")
print("ok: the next client to take that slot receives none of 'a's output")

# --- 3: that slot can still attach to 'a' for real --------------------------
# session_attach returns early when the client is already in clients[], so a
# stale entry does not merely add noise: the legitimate attach that follows
# sends no snapshot at all, which reads as a terminal that never paints.
stranger.sendall(attach_frame('a'))
assert wait_for_type(stranger, SNAPSHOT) is not None, (
    "ATTACH a produced no snapshot — session_attach found this client already "
    "in clients[] and returned early, i.e. the slot inherited a stale entry")
print("ok: a real attach to 'a' still repaints")

# --- 4: attach, then create, leaks the same way -----------------------------
# A connection of its own, because the assertion is about what NEW does to the
# session this connection was attached to — reusing a connection that an
# earlier property already polluted would test the pollution instead.
mover = connect()
mover.sendall(attach_frame('a'))
assert wait_for_type(mover, SNAPSHOT) is not None, "ATTACH a produced no snapshot"
mover.sendall(new_frame('c'))
assert wait_for_type(mover, SNAPSHOT) is not None, "NEW c never attached us"
final = clients_per_session(mover)
assert final['c'] == 1, f"the creating client is not attached to 'c': {final}"
assert final['a'] == 1, (
    f"'a' should still hold the client from property 3 and nothing else: {final}")
print("ok: ATTACH a; NEW c left 'a' with only its other client")

for name in ('a', 'b', 'c'):
    mover.sendall(kill_frame(name))
time.sleep(0.3)
mover.close()
stranger.close()
PY

echo "PASS: MSG_NEW_SESSION detaches from the session the connection was on"
