#!/usr/bin/env bash
# S4: the install path must not be able to leave a STALE daemon answering the
# socket, because a stale daemon is an unpatched daemon.
#
# The hazard was not a crash, it was a documentation fork. The Makefile defaults
# PREFIX=/usr/local; AGENTS.md tells you (and tells coding agents especially,
# since sudo blocks on a password prompt with no tty) to use $HOME/.local; and
# both service units hardcoded /usr/local/bin. Follow both documents and launchd
# or systemd starts whatever is at /usr/local/bin — which, after an earlier
# install, is an OLDER build. It answers the socket first, so the client's
# sibling-first autospawn never runs (it only spawns when nothing answers), and
# the protocol's skip-unknown-frames rule turns every message the old daemon
# predates into a silent no-op. Nothing prints an error anywhere.
#
# So there are two things to hold: the shipped units must name the prefix you
# actually installed to, and an install that leaves a different binary at another
# prefix must say so out loud.
#
# The strongest assertion here is the one about the repo rather than the output:
# no copyable unit file may exist in contrib/ at all. A test that only checked
# the rendered copy would keep passing if someone re-added a ready-made
# .plist next to the template — and that file is exactly the bug.
set -u

. "$(dirname "$0")/lib.sh"

require_bins agent-terminald agent-terminal

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

pass() { echo "ok: $1"; }

# ---------------------------------------------------------------------------
# 1. contrib/ ships templates only.
# ---------------------------------------------------------------------------
copyable="$(find "$ROOT/contrib" -type f ! -name '*.in' | sort)"
[ -z "$copyable" ] || fail "contrib/ must ship templates only, found copyable unit file(s):
$copyable"
pass "contrib/ contains no ready-to-copy unit file"

for t in launchd/dev.agentterminal.daemon.plist systemd/agent-terminald.service; do
    [ -f "$ROOT/contrib/$t.in" ] || fail "missing template contrib/$t.in"
    grep -q '@BINDIR@' "$ROOT/contrib/$t.in" ||
        fail "contrib/$t.in has no @BINDIR@ placeholder — it would install a fixed path"
done
pass "both templates carry the @BINDIR@ placeholder"

# ---------------------------------------------------------------------------
# 2. A non-default PREFIX is rendered into both units by `make install`.
#    DESTDIR keeps this out of the real filesystem; PREFIX is a path that
#    exists nowhere, so a hardcoded /usr/local cannot pass by coincidence.
# ---------------------------------------------------------------------------
ROOTDIR="$TMP/root"
PFX="/opt/at-install-test"
( cd "$ROOT" && make -s install BUILD="$BUILD" DESTDIR="$ROOTDIR" PREFIX="$PFX" ) > "$TMP/install.log" 2>&1 ||
    { cat "$TMP/install.log"; fail "make install failed"; }

SHARE="$ROOTDIR$PFX/share/agent-terminal"
PLIST="$SHARE/dev.agentterminal.daemon.plist"
UNIT="$SHARE/agent-terminald.service"

[ -x "$ROOTDIR$PFX/bin/agent-terminald" ] || fail "daemon not installed under DESTDIR"
[ -f "$ROOTDIR$PFX/share/man/man1/agent-terminal.1" ] || fail "man page not installed"
[ -f "$PLIST" ] || fail "rendered plist missing at $PLIST"
[ -f "$UNIT" ] || fail "rendered systemd unit missing at $UNIT"
pass "install placed binaries, man page and both rendered units"

for f in "$PLIST" "$UNIT"; do
    ! grep -q '@BINDIR@\|@UNIT_PATH@' "$f" || fail "$(basename "$f"): placeholder survived rendering"
    ! grep -q 'TEMPLATE' "$f" ||
        fail "$(basename "$f"): the template-only note reached the installed copy"
    ! grep -q '/usr/local/bin/agent-terminald' "$f" ||
        fail "$(basename "$f"): still names /usr/local/bin — this is the stale-daemon bug"
    grep -q "$PFX/bin/agent-terminald" "$f" || fail "$(basename "$f"): does not name $PFX/bin"
done
pass "neither unit names /usr/local/bin; both name $PFX/bin"

# The whole point is that a service manager starts the RIGHT binary, so assert
# the field each one actually execs, not merely that the string appears.
grep -qx "ExecStart=$PFX/bin/agent-terminald -f" "$UNIT" ||
    fail "systemd ExecStart is not $PFX/bin/agent-terminald -f: $(grep '^ExecStart' "$UNIT")"

# Parse the plist rather than grep it: ProgramArguments[0] is what launchd execs,
# and a string in some other key would satisfy a grep.
python3 - "$PLIST" "$PFX" <<'PY' || fail "plist did not validate"
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
pfx = sys.argv[2]
argv = d['ProgramArguments']
assert argv[0] == f'{pfx}/bin/agent-terminald', f'ProgramArguments[0] = {argv[0]!r}'
assert argv[1:] == ['-f'], f'unexpected argv tail {argv[1:]!r}'
path = d['EnvironmentVariables']['PATH'].split(':')
assert path[0] == f'{pfx}/bin', f'install prefix is not first on PATH: {path!r}'
# Session commands inherit this PATH; losing the system entries would break
# every session command that is not in the prefix.
for req in ('/usr/bin', '/bin'):
    assert req in path, f'{req} missing from unit PATH: {path!r}'
PY
pass "launchd execs $PFX/bin/agent-terminald and puts $PFX/bin first on PATH"

# ---------------------------------------------------------------------------
# 3. The default prefix must not produce a duplicated PATH entry — /usr/local/bin
#    is already in the base list. A duplicate is harmless to the shell and
#    confusing in a file people read, which is why the Makefile bothers.
# ---------------------------------------------------------------------------
( cd "$ROOT" && make -s install BUILD="$BUILD" DESTDIR="$TMP/root2" ) > "$TMP/install2.log" 2>&1 ||
    { cat "$TMP/install2.log"; fail "make install with the default PREFIX failed"; }
DEFPLIST="$TMP/root2/usr/local/share/agent-terminal/dev.agentterminal.daemon.plist"
python3 - "$DEFPLIST" <<'PY' || fail "default-prefix plist did not validate"
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
path = d['EnvironmentVariables']['PATH'].split(':')
assert path.count('/usr/local/bin') == 1, f'/usr/local/bin appears {path.count("/usr/local/bin")}x: {path!r}'
assert d['ProgramArguments'][0] == '/usr/local/bin/agent-terminald', d['ProgramArguments']
PY
pass "the default prefix renders /usr/local/bin exactly once on PATH"

# A rendered unit depends on the VALUE of PREFIX, which no file timestamp
# records. Installing to a second prefix right after the first must not reuse the
# first rendering — that would put the previous prefix's path into the new unit,
# which is the stale-path bug arriving via the build system instead of the docs.
# (It did, when the rule depended only on the template: make found the output
# newer than its prerequisite and skipped the recipe. The rule now re-runs
# unconditionally and compares the rendered bytes instead.)
PFX2="/opt/at-install-test-two"
( cd "$ROOT" && make -s install BUILD="$BUILD" DESTDIR="$TMP/root3" PREFIX="$PFX2" ) \
    > "$TMP/install3.log" 2>&1 || { cat "$TMP/install3.log"; fail "second-prefix install failed"; }
UNIT2="$TMP/root3$PFX2/share/agent-terminal/agent-terminald.service"
grep -qx "ExecStart=$PFX2/bin/agent-terminald -f" "$UNIT2" ||
    fail "second install reused the first prefix's rendering: $(grep '^ExecStart' "$UNIT2")"
pass "a second install with a different PREFIX re-renders instead of reusing"

# `make install` under DESTDIR must skip the stale-binary check: the prefixes it
# inspects belong to this build host, not to the staging root, so a packaging
# build would warn about binaries that have nothing to do with what it produced.
grep -q 'skipping the stale-daemon check' "$TMP/install.log" ||
    fail "DESTDIR install did not skip the stale-daemon check: $(cat "$TMP/install.log")"
pass "DESTDIR install skips the host-prefix check and says so"

# ---------------------------------------------------------------------------
# 4. The stale-binary warning itself. Driven with synthetic prefixes so the
#    result does not depend on what happens to be installed on this machine.
# ---------------------------------------------------------------------------
CHECK="$ROOT/tools/check_install_paths.sh"
A="$TMP/a/bin"; B="$TMP/b/bin"; C="$TMP/c/bin"
mkdir -p "$A" "$B" "$C"
printf 'new build\n' > "$A/agent-terminald"
printf 'OLD build\n' > "$B/agent-terminald"
printf 'new build\n' > "$C/agent-terminald"
chmod +x "$A/agent-terminald" "$B/agent-terminald" "$C/agent-terminald"

# Differing bytes at another prefix: must warn, and must name BOTH paths — a
# warning that does not say which copy is the other one is not actionable.
out="$(AT_INSTALL_CANDIDATES="$B:$C" sh "$CHECK" "$A" 2>&1)"; rc=$?
[ "$rc" -eq 0 ] || fail "the check must not fail an install (rc=$rc)"
grep -q 'WARNING' <<< "$out" || fail "no warning for a differing binary at another prefix: $out"
grep -q "$A/agent-terminald" <<< "$out" || fail "warning does not name the installed copy: $out"
grep -q "$B/agent-terminald" <<< "$out" || fail "warning does not name the stale copy: $out"
grep -q "$C/agent-terminald" <<< "$out" && fail "warning names an IDENTICAL copy at $C: $out"
pass "warns about the differing copy, names both paths, ignores the identical one"

# Identical bytes everywhere: silence. Without this the check would be a
# permanent warning that everyone learns to ignore.
out="$(AT_INSTALL_CANDIDATES="$C" sh "$CHECK" "$A" 2>&1)"
[ -z "$out" ] || fail "expected silence when the other copy is byte-identical, got: $out"
# A symlink to the installed copy is the same file, so it must also be silent.
ln -sf "$A/agent-terminald" "$TMP/c/bin/agent-terminald"
out="$(AT_INSTALL_CANDIDATES="$C" sh "$CHECK" "$A" 2>&1)"
[ -z "$out" ] || fail "expected silence for a symlink to the installed copy, got: $out"
# And a candidate prefix with nothing in it.
out="$(AT_INSTALL_CANDIDATES="$TMP/nonexistent/bin" sh "$CHECK" "$A" 2>&1)"
[ -z "$out" ] || fail "expected silence for an empty candidate prefix, got: $out"
pass "silent on identical bytes, on a symlink to itself, and on an absent prefix"

# A check that cannot run must not report clean. cmp is hidden by handing the
# script a PATH with nothing on it. /bin/sh is spelled out because the empty PATH
# applies to resolving the interpreter too — with a bare `sh` this exits 127
# before the script starts, which would "pass" a weaker assertion than the one
# below and prove nothing about the missing-cmp branch.
out="$(AT_INSTALL_CANDIDATES="$B" PATH="$TMP/empty" /bin/sh "$CHECK" "$A" 2>&1)"; rc=$?
[ "$rc" -eq 0 ] || fail "the no-cmp path must still not fail an install (rc=$rc)"
grep -q 'DID NOT RUN' <<< "$out" ||
    fail "with cmp unavailable the check reported nothing — silence reads as clean: $out"
pass "says so loudly when cmp is unavailable instead of reporting clean"

echo "PASS: install units are rendered per-PREFIX and stale copies are reported"
