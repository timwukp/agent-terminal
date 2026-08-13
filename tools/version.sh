#!/bin/sh
# The build's version identity, on stdout. Three regimes:
#
#   git checkout, clean    <head12>
#   git checkout, edited   <head12>-dirty.<content8>
#   no checkout            contents of ./.tarball-version, else "unknown"
#
# The suffix exists because "-dirty" alone is one name for infinitely many
# trees: a pre-fix and a post-fix build printed the SAME string, so the
# README's documented skew check ("compare the hashes") could not tell the
# patched binary from the vulnerable one. <content8> hashes what the tree
# actually holds — the diff against HEAD plus every untracked-but-not-ignored
# file's name and bytes — so two different edited trees never share a version.
# Untracked files count as dirty on purpose: a new source file changes the
# binary exactly as much as an edit to a tracked one, and a diff-only hash
# would miss it.
#
# A checkout NEVER reads .tarball-version: the file is written by the release
# workflow into the tarball it ships, and a stray copy inside a checkout must
# not be able to make an edited tree impersonate a release.
#
# Plumbing only (diff-index, hash-object, ls-files): porcelain diff output
# bends to user config (external drivers, mnemonic prefixes), and this string
# must be identical for identical trees on every machine. hash-object without
# -w writes nothing to the object store, so building does not grow .git.
set -u

if git rev-parse --short=12 HEAD >/dev/null 2>&1; then
    head="$(git rev-parse --short=12 HEAD)"
    # Refresh the stat cache first: diff-index trusts it, and a stale one
    # (fresh clone, touched files) reports phantom dirtiness.
    git update-index -q --refresh >/dev/null 2>&1 || true
    untracked="$(git ls-files --others --exclude-standard | LC_ALL=C sort)"
    if git diff-index --quiet HEAD -- 2>/dev/null && [ -z "$untracked" ]; then
        printf '%s\n' "$head"
        exit 0
    fi
    content="$(
        {
            git diff-index -p HEAD -- 2>/dev/null
            printf '%s\n' "$untracked"
            printf '%s\n' "$untracked" | while IFS= read -r f; do
                [ -n "$f" ] && cat "$f" 2>/dev/null
            done
        } | git hash-object --stdin 2>/dev/null
    )"
    if [ -n "$content" ]; then
        printf '%s-dirty.%.8s\n' "$head" "$content"
    else
        # hash-object itself failed — still say dirty, just less precisely,
        # rather than failing the build over a version string.
        printf '%s-dirty\n' "$head"
    fi
    exit 0
fi

if [ -f .tarball-version ]; then
    v="$(tr -d ' \t\n\r' < .tarball-version)"
    if [ -n "$v" ]; then
        printf '%s\n' "$v"
        exit 0
    fi
fi
printf 'unknown\n'
