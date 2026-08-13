#!/bin/sh
# Every workflow must (a) declare a top-level `permissions:` block that grants
# no write scope, and (b) pin every action it uses to a full 40-hex commit SHA.
#
# Why both, and why a guard instead of a convention: the repo-default token is
# read/write on contents, and a workflow with no `permissions:` block hands
# that token to every action it runs. A tag like @v4 is a moving target — the
# publisher (or whoever takes over the publisher's account) can re-point it at
# arbitrary code after review. The SHA pin freezes what runs; Dependabot's
# github-actions entry is how a pin moves, via a reviewable PR. A guard exists
# because a convention holds only until the next workflow is added in a hurry —
# and a NEW workflow is exactly where a missing permissions block or an
# unpinned action arrives.
#
# Usage: check_workflow_pins.sh [workflows-dir]   (default .github/workflows)
# The directory argument exists so the mutation tests can point this at
# fixtures; CI runs it bare.
set -u

dir="${1:-.github/workflows}"
[ -d "$dir" ] || { echo "ERROR: no such directory: $dir" >&2; exit 3; }

fail=0
n=0
for f in "$dir"/*.yml "$dir"/*.yaml; do
    [ -f "$f" ] || continue
    n=$((n + 1))

    # (a) A top-level permissions block: the key at column 0. An indented
    # `permissions:` belongs to a single job, which still leaves every OTHER
    # job on the default token, so it does not count.
    if ! grep -q '^permissions:' "$f"; then
        echo "FAIL $f: no top-level permissions: block — every job runs on the repo-default token" >&2
        fail=1
    else
        # No scope in the block may grant write. The block ends at the next
        # column-0 key; awk scopes the scan so a job-level `contents: write`
        # elsewhere (which would be its own finding) cannot hide in it, and a
        # commented-out line cannot satisfy it.
        if awk '/^permissions:/{inblk=1; next} inblk && /^[^ #]/{inblk=0} inblk' "$f" \
                | grep -q 'write'; then
            echo "FAIL $f: top-level permissions block grants a write scope" >&2
            fail=1
        fi
    fi

    # (b) Every `uses:` names action@<40-hex-sha>. Local composite actions
    # (uses: ./...) carry the repo's own review and need no pin; none exist
    # today, but the exemption is stated rather than discovered.
    while IFS= read -r line; do
        ref="$(printf '%s\n' "$line" | sed -E 's/.*uses:[[:space:]]*//; s/[[:space:]]*(#.*)?$//')"
        case "$ref" in
            ./*) continue ;;
            *@*)
                sha="${ref##*@}"
                if ! printf '%s\n' "$sha" | grep -qE '^[0-9a-f]{40}$'; then
                    echo "FAIL $f: not pinned to a 40-hex commit SHA: $ref" >&2
                    fail=1
                fi
                ;;
            *)
                echo "FAIL $f: uses with no @ref at all: $ref" >&2
                fail=1
                ;;
        esac
    done <<EOF
$(grep -E '^[[:space:]]*-?[[:space:]]*uses:' "$f")
EOF
done

# A guard that scanned nothing must not report clean — an empty or mistyped
# directory reads exactly like a passing run otherwise.
if [ "$n" -eq 0 ]; then
    echo "ERROR: scanned 0 workflow files in $dir" >&2
    exit 3
fi

[ "$fail" -eq 0 ] && echo "PASS: $n workflow(s) — top-level read-only permissions, all uses SHA-pinned"
exit "$fail"
