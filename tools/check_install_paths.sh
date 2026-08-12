#!/bin/sh
# Warn when a DIFFERENT agent-terminald is installed at another prefix.
#
# Why this is worth a check rather than a sentence in the README: the two copies
# do not conflict at install time, they conflict at *connect* time, and the loser
# is whichever one you meant to run.
#
# The client autospawns the agent-terminald sitting next to its own binary, but
# only when nothing is already answering the socket. A service unit pointed at
# the other prefix — or a shell whose PATH finds the other client first — starts
# the old daemon, the old daemon answers, and sibling-first never runs. The
# protocol skips frames it does not recognize, so every message the old build
# predates becomes a silent no-op: new key bindings do nothing, new fields read
# as absent, and nothing anywhere prints an error. A stale daemon is an
# unpatched daemon, which is why this lives in the security work and not in a
# packaging cleanup.
#
# Usage: check_install_paths.sh <bindir-just-installed-to>
#
# Candidate prefixes to compare against are the ones the two documented install
# paths and the common package managers produce. Override for tests:
#   AT_INSTALL_CANDIDATES=/a/bin:/b/bin sh tools/check_install_paths.sh /a/bin
#
# Always exits 0. A leftover binary elsewhere is a hazard to tell the user
# about, not a reason to fail their install — and a failing `sudo make install`
# would be a worse outcome than the warning it replaced.
set -u

BINDIR="${1:?usage: check_install_paths.sh <bindir>}"
CANDIDATES="${AT_INSTALL_CANDIDATES:-/usr/local/bin:$HOME/.local/bin:/opt/homebrew/bin:/usr/bin}"

mine="$BINDIR/agent-terminald"

# Nothing to compare against. Reachable when DESTDIR-less installs are run in a
# sandbox, or when this script is called before the binary lands.
[ -f "$mine" ] || exit 0

# A comparison tool that cannot run must not report "clean" — the whole point of
# the check is that the dangerous state is invisible. cmp is POSIX and present
# everywhere we build, so this branch is a guard against a stripped-down
# container rather than an expected path.
if ! command -v cmp > /dev/null 2>&1; then
    echo "agent-terminal: WARNING: cmp not found, so the stale-daemon check DID NOT RUN." >&2
    echo "  Compare $mine by hand against any other copy on this machine." >&2
    exit 0
fi

# Split the candidate list on ':' via the positional parameters, then put IFS
# back before the loop body runs — the body needs normal word splitting, and the
# later "for f in $found" needs it too.
old_ifs="$IFS"
IFS=:
# shellcheck disable=SC2086  # deliberate split of a colon-separated list
set -- $CANDIDATES
IFS="$old_ifs"

found=""
for dir in "$@"; do
    [ -n "$dir" ] || continue
    other="$dir/agent-terminald"
    [ -f "$other" ] || continue
    # cmp follows symlinks and compares bytes, so the copy just installed
    # compares equal to itself and to a symlink pointing at it. That makes the
    # candidate list safe to contain BINDIR: skipping it would be an
    # optimization, not a correctness requirement.
    cmp -s "$mine" "$other" && continue
    found="$found $other"
done

[ -n "$found" ] || exit 0

{
    echo "agent-terminal: WARNING: a DIFFERENT agent-terminald is installed at another prefix."
    echo "  just installed: $mine"
    for f in $found; do echo "  also present:   $f"; done
    echo
    echo "  Whichever one starts first answers the socket, and the client only autospawns"
    echo "  its own sibling when nothing is answering. Since the protocol skips frames it"
    echo "  predates, the older daemon makes newer features silently do nothing instead of"
    echo "  reporting a version mismatch. Check which one you are actually running:"
    echo "      agent-terminal version      # compares client and daemon builds"
    echo "  then either remove the other copy, or re-run 'make install' with that PREFIX"
    echo "  and re-copy the service unit from PREFIX/share/agent-terminal/."
} >&2

exit 0
