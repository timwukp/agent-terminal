#!/bin/sh
# No cloud metadata or personal paths may be tracked by this repo: account
# IDs, ARNs, bucket URLs, provider hostnames, work e-mail addresses, or a
# developer's home directory. Everything here is public, so the time to catch
# one is before it is served, and the place is CI — a pre-push habit is only
# as durable as the person who has it.
#
# Two properties this guard holds that a casual grep does not:
#
#   - It can never report clean by accident. A scan that reads zero files
#     exits non-zero (an empty list looks exactly like a passing run
#     otherwise), and a grep that FAILS — as opposed to finding nothing —
#     exits non-zero too, because a guard that could not run has not passed.
#   - It scans itself. The patterns below are assembled from concatenated
#     pieces so this file is not its own match; no file is exempted by path,
#     because a path exemption is where the next leak hides.
#
# Scope is git-tracked files (`git grep`): that is what GitHub serves. Local
# build residue (mutants.out/, target/, node_modules/) is ignored by git and
# so, correctly, by this scan.
#
# Usage: check_redaction.sh [dir]   (default: the current checkout)
# The dir argument exists so tests can point it at fixture repos.
set -u

cd "${1:-.}" || { echo "ERROR: cannot cd to ${1:-.}" >&2; exit 3; }
git rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    { echo "ERROR: ${1:-.} is not a git checkout — nothing was scanned" >&2; exit 3; }

# macOS home paths; ARNs; AWS hostnames; S3 URLs; work e-mail; an account id
# in context (a bare 12-digit run matches test data and lockfile hashes, so
# it needs the word next to it).
P1='/Use''rs/[A-Za-z0-9._-]+'
P2='arn:aw''s'
P3='amazonaw''s\.com'
P4='s3:''//'
P5='[A-Za-z0-9._%+-]+@amaz''on\.com'
P6='[Aa]ccoun''t[^0-9]{0,12}[0-9]{12}'
PAT="$P1|$P2|$P3|$P4|$P5|$P6"

n="$(git ls-files | wc -l | tr -d ' ')"
[ "$n" -gt 0 ] || { echo "ERROR: this checkout tracks 0 files — nothing was scanned" >&2; exit 3; }

out="$(git grep -nIE "$PAT" -- . 2>&1)"; rc=$?
case "$rc" in
    0)
        echo "FAIL: redaction violations in tracked files:" >&2
        printf '%s\n' "$out" >&2
        exit 1
        ;;
    1)
        echo "PASS: $n tracked files, no cloud metadata or personal paths"
        exit 0
        ;;
    *)
        echo "ERROR: git grep itself failed (rc=$rc) — nothing was verified: $out" >&2
        exit 3
        ;;
esac
