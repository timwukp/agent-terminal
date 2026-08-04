#!/usr/bin/env bash
# Verify every relative link and image path in the docs resolves.
#
# A README that points at a moved file is a broken promise to both humans and
# agents, and it is invisible until someone clicks. Anchors (#section) are
# checked too: AGENTS.md is linked by section from README.md.
set -u

FAIL=0
DOCS="README.md AGENTS.md CONTRIBUTING.md SECURITY.md"

# GitHub's anchor slug: lowercase, drop punctuation, spaces to hyphens.
slug() {
    printf '%s' "$1" \
      | tr '[:upper:]' '[:lower:]' \
      | sed -e 's/[^a-z0-9 _-]//g' -e 's/ /-/g'
}

for f in $DOCS; do
    [ -f "$f" ] || { echo "missing doc: $f"; FAIL=1; continue; }

    # markdown links ](target) and html src="target"
    targets=$(
        { grep -oE '\]\([^)]+\)' "$f" | sed -e 's/^](//' -e 's/)$//'
          grep -oE 'src="[^"]+"' "$f" | sed -e 's/^src="//' -e 's/"$//'
        } | sort -u
    )

    for t in $targets; do
        case "$t" in
            http*|mailto:*|"") continue ;;
            \#*)   # same-file anchor
                want="${t#\#}"
                found=0
                while IFS= read -r h; do
                    [ "$(slug "$h")" = "$want" ] && { found=1; break; }
                done < <(grep -E '^#+ ' "$f" | sed -E 's/^#+ //')
                [ "$found" -eq 1 ] || { echo "$f: broken anchor -> $t"; FAIL=1; }
                continue ;;
        esac

        path="${t%%#*}"
        anchor="${t#*#}"
        [ -e "$path" ] || { echo "$f: broken link -> $path"; FAIL=1; continue; }

        # cross-file anchor: the heading must exist in the target file
        if [ "$anchor" != "$t" ] && [ -n "$anchor" ]; then
            found=0
            while IFS= read -r h; do
                [ "$(slug "$h")" = "$anchor" ] && { found=1; break; }
            done < <(grep -E '^#+ ' "$path" | sed -E 's/^#+ //')
            [ "$found" -eq 1 ] || { echo "$f: broken anchor -> $path#$anchor"; FAIL=1; }
        fi
    done
done

[ "$FAIL" -eq 0 ] && echo "PASS: all doc links and anchors resolve"
exit "$FAIL"
