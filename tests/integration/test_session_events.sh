#!/usr/bin/env bash
# MSG_SESSIONS_CHANGED (0x39): the daemon tells capable clients when the
# session table changed, so a client's session list stops being a poll. Each
# property below is aimed at a specific wrong implementation:
#
#   1. HELLO_OK's server_flags carries BOTH bits (panes 0x0001 AND session
#      events 0x0002). An implementation that ASSIGNS the new bit instead of
#      OR-ing it fails here — and so does one that never advertises it, which
#      would leave a client unable to tell "will notify" from "too old".
#   2. An UNATTACHED capable client is notified when someone else creates a
#      session. A fan-out copied from MSG_PANE_BELL — which walks one
#      session's clients[] — reaches nobody here and fails.
#   3. A client ATTACHED to session A is notified about session B. Same
#      delivery set, opposite starting point.
#   4. Attach/detach notifies too: nclients is a field the list reports, so a
#      gate watching only "which sessions exist" fails this.
#   5. A client that did NOT set CLIENT_CAP_SESSION_EVENTS never receives
#      0x39 — including one that set CLIENT_CAP_PANES, so reusing 0x0001 as
#      the gate fails. Checked across a window in which a session really is
#      created and the capable clients really are notified: an absence measured
#      while nothing changed is equally true of a daemon with no gate at all,
#      which is not a hypothetical — a mutant that deletes the capability test
#      survived exactly that shape of check. The incapable clients are attached
#      and demonstrably receiving other frames in the same window too.
#   6. A busy session produces ZERO notifications: 3 s of child output paced
#      ~20 ms apart, so it lands on ~150 separate compositing ticks rather
#      than in one burst. An implementation that emits every tick, or whose
#      change detector reads a field that moves on its own, fails here.
#   7. A burst of three creates, and then three kills, delivered in ONE write
#      each, produce exactly ONE notification each — the tick is the
#      coalescing window. (client_io drains its whole input ring and
#      dispatches every complete frame before returning, so all three land in
#      the same tick by construction, not by luck.)
#   8. The payload is empty. The message is "ask again"; a client must not
#      start depending on bytes that are not there.
set -u

. "$(dirname "$0")/lib.sh"
require_bins agent-terminald agent-terminal
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
import select, socket, struct, sys, time

sock_path = sys.argv[1]

HELLO, HELLO_OK = 0x01, 0x02
LIST2_REQ, SESSION_LIST2 = 0x1a, 0x37
NEW, KILL, ATTACH, STDIN = 0x12, 0x13, 0x14, 0x20
SNAPSHOT, OUTPUT = 0x31, 0x30
CHANGED = 0x39

CAP_PANES, CAP_EVENTS = 0x0001, 0x0002


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


def connect(flags):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.sendall(frame(HELLO, struct.pack('<HH', 1, flags)))
    typ, payload = read_frame(s)
    assert typ == HELLO_OK, f"expected HELLO_OK, got {typ:#x}"
    return s, payload


def new_frame(name, cols=80, rows=24):
    nb = name.encode()
    return frame(NEW, struct.pack('<HHB', cols, rows, len(nb)) + nb + struct.pack('<H', 0))


def kill_frame(name):
    nb = name.encode()
    return frame(KILL, bytes([len(nb)]) + nb)


def attach_frame(name, pane_id=0, cols=80, rows=24):
    nb = name.encode()
    return frame(ATTACH, struct.pack('<HHBB', cols, rows, pane_id, len(nb)) + nb)


def collect(s, seconds):
    """Drain frames for a fixed window; returns list of (type, payload)."""
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


def wait_for_type(s, want, seconds):
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
            got = (typ, payload)
            break
    s.settimeout(10)
    return got


def collect_multi(named, seconds):
    """Drain several sockets over ONE window.

    Properties 5 and 6 are about the same window — "while the child is
    producing output, these clients got frames and none of them was 0x39" —
    and reading the sockets one after another would not test that: by the time
    the second socket is read the producer may be finished, and the sockets
    left undrained meanwhile are the ones the daemon's 4 MiB backlog ceiling
    disconnects.
    """
    out = {k: [] for k in named}
    end = time.time() + seconds
    while True:
        left = end - time.time()
        if left <= 0:
            break
        ready, _, _ = select.select(list(named.values()), [], [], min(0.2, left))
        for s in ready:
            name = [k for k, v in named.items() if v is s][0]
            typ, payload = read_frame(s)
            if typ is None:
                continue
            out[name].append((typ, payload))
    return out


def count(frames, typ):
    return len([f for f in frames if f[0] == typ])


# --- 1: the handshake advertises both capabilities -------------------------
drv, ok = connect(flags=0)
assert len(ok) >= 12, f"HELLO_OK has no server_flags field ({len(ok)} bytes)"
flags = struct.unpack('<H', ok[10:12])[0]
assert flags & CAP_PANES, f"SERVER_CAP_PANES vanished from server_flags ({flags:#06x})"
assert flags & CAP_EVENTS, f"SERVER_CAP_SESSION_EVENTS not advertised ({flags:#06x})"
print(f"ok: HELLO_OK server_flags = {flags:#06x} (panes + session events)")

watcher, _ = connect(flags=CAP_EVENTS)          # never attaches to anything
sidebar, _ = connect(flags=CAP_PANES | CAP_EVENTS)  # will attach to 'a'
nocap, _ = connect(flags=0)                     # pre-capability client
panes_only, _ = connect(flags=CAP_PANES)        # knows panes, not this message

# --- 2: an unattached capable client hears about a create ------------------
drv.sendall(new_frame('a'))
assert wait_for_type(drv, SNAPSHOT, 5), "no snapshot after create"
got = wait_for_type(watcher, CHANGED, 5)
assert got, "an unattached capable client was never told the table changed"
# --- 8: the payload is empty
assert got[1] == b'', f"MSG_SESSIONS_CHANGED carried {len(got[1])} bytes; it is empty by contract"
print("ok: an unattached capable client is notified, with an empty payload")
assert wait_for_type(sidebar, CHANGED, 5), "second capable client missed the broadcast"

# --- 4: attaching changes nclients, which the list reports ----------------
for s in (sidebar, nocap, panes_only):
    s.sendall(attach_frame('a'))
    assert wait_for_type(s, SNAPSHOT, 5), "attach produced no snapshot"
got = wait_for_type(watcher, CHANGED, 5)
assert got, "attach changed nclients and nobody was told"
print("ok: attach/detach is a change too (nclients is a listed field)")

# --- 3: a client attached to 'a' hears about 'b' --------------------------
# 'b' is created on its OWN connection, which is also how both shipped clients
# behave (the CLI opens one connection per command; the GUI's control calls are
# short-lived). It matters here because MSG_NEW_SESSION re-points c->attached
# without detaching from the previous session, so a connection that created two
# sessions is left registered in both — and `drv` has to stay solely on 'a' to
# drive the output the checks below depend on.
mk_b, _ = connect(flags=0)
mk_b.sendall(new_frame('b'))
assert wait_for_type(mk_b, SNAPSHOT, 5), "no snapshot after creating b"
assert wait_for_type(sidebar, CHANGED, 5), \
    "a client attached to 'a' was not told about 'b' — per-session fan-out?"
print("ok: a client attached to one session hears about another")

# --- 6: while a child produces output on tick after tick, nobody hears a
#     change. The sleep is what makes this a test of TICKS rather than of bytes:
#     200 lines each ~20 ms apart spans well over 100 compositing ticks, where
#     the same 200 lines emitted in a burst would land in one or two.
all_socks = {'watcher': watcher, 'sidebar': sidebar,
             'nocap': nocap, 'panes_only': panes_only}
collect_multi(all_socks, 0.8)
drv.sendall(frame(STDIN, b"i=0; while [ $i -lt 200 ]; do echo busy-$i; sleep 0.02;"
                         b" i=$((i+1)); done\n"))
got = collect_multi(all_socks, 3.0)
for name in ('watcher', 'sidebar'):
    n = count(got[name], CHANGED)
    assert n == 0, f"{n} notifications reached {name} while only child output changed"
print("ok: 3 s of output spread over ~150 ticks produced no notification")

# --- 5: the capability gate, measured across a REAL change ----------------
# One window, four clients, and a session created inside it. The window above
# is the wrong place for this check even though the sockets are busy: nothing
# changed there, so nobody was notified, and "the incapable clients received no
# 0x39" is just as true of a daemon whose gate is `return c->hello_done`. That
# mutant survived this test in exactly that form.
#
# The echo gives the two attached-but-incapable clients traffic of their own
# during the window, so their zero is an absence among frames rather than an
# absence of frames.
collect_multi(all_socks, 0.8)
drv.sendall(frame(STDIN, b"echo gate-window\n"))
mk_g, _ = connect(flags=0)
mk_g.sendall(new_frame('g'))
got = collect_multi(all_socks, 1.5)
for name in ('watcher', 'sidebar'):
    n = count(got[name], CHANGED)
    # Exactly one: handle_new creates and attaches inside a single dispatch, so
    # both effects are visible to the same tick.
    assert n == 1, f"{name} got {n} notifications for one create, expected 1"
for label, name in (("pre-capability", 'nocap'), ("panes-only", 'panes_only')):
    frames = got[name]
    assert frames, f"{label} client went silent — the check below would be vacuous"
    assert count(frames, OUTPUT) > 0, f"{label} client saw no output (control failed)"
    assert count(frames, CHANGED) == 0, f"0x39 leaked to the {label} client"
    print(f"ok: no 0x39 for the {label} client while a create was announced "
          f"({len(frames)} other frames arrived)")

# --- 7: a burst in one write is one notification --------------------------
# One write, one connection: client_io drains its whole input ring and
# dispatches every complete frame before returning, so the three requests are
# served between two ticks by construction rather than by timing luck.
#
# Killing a session disconnects its attached clients, so the kills below come
# from a connection attached to none of c/d/e — `maker` is attached to all three
# and would be dropped by the first one, taking the other two frames with it.
killer, _ = connect(flags=0)


def session_count():
    killer.sendall(frame(LIST2_REQ))
    reply = wait_for_type(killer, SESSION_LIST2, 5)
    assert reply, "no SESSION_LIST2 reply"
    return struct.unpack('<H', reply[1][:2])[0]


# Derived, not remembered: the sessions this test has created so far are counted
# here rather than written down, so adding a step above cannot turn this into a
# false failure — or, worse, leave a stale number that happens to still match.
before = session_count()
collect(watcher, 0.8)
maker, _ = connect(flags=0)
maker.sendall(new_frame('c') + new_frame('d') + new_frame('e'))
frames = collect(watcher, 1.5)
n = count(frames, CHANGED)
assert n == 1, f"three creates in one write produced {n} notifications, not 1"
print("ok: three creates in one write coalesce into one notification")

# The list really did grow by three — otherwise "one notification" could mean
# the burst was dropped rather than coalesced.
after = session_count()
assert after == before + 3, \
    f"expected {before + 3} sessions after the burst, list says {after}"

collect(watcher, 0.8)
killer.sendall(kill_frame('c') + kill_frame('d') + kill_frame('e'))
frames = collect(watcher, 1.5)
n = count(frames, CHANGED)
assert n == 1, f"three kills in one write produced {n} notifications, not 1"
print("ok: three kills in one write coalesce into one notification")

assert session_count() == before, "the kills did not land"

# --- and an idle table stays silent afterwards ---------------------------
frames = collect(watcher, 1.0)
assert count(frames, CHANGED) == 0, "the table stopped changing but notifications did not"
print("ok: no trailing notifications once the table settles")
PY

echo "PASS: session-table changes are pushed, capability-gated, and coalesced"
