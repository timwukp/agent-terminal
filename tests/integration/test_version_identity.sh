#!/usr/bin/env bash
# #23: `agent-terminal version` printed the SAME string for two different
# builds. "-dirty" is one name for infinitely many trees, so a pre-fix and a
# post-fix binary both said `<head>-dirty` and the README's documented skew
# check ("compare the hashes") could not tell the patched daemon from the
# vulnerable one. And a release tarball (no .git) said "unknown", which is one
# name for every release ever shipped.
#
# tools/version.sh now names trees, not just commits:
#   clean checkout     <head12>
#   edited checkout    <head12>-dirty.<content8>   (content8 hashes the tree)
#   tarball            contents of .tarball-version, else "unknown"
#
# Everything below drives a throwaway fixture repo, not this checkout — the
# real repo's dirtiness is whatever it happens to be today, which is exactly
# the kind of input a test must not depend on. The last section ties the
# Makefile to the script using the real checkout, where "they agree" is
# assertable regardless of that state.
set -u

. "$(dirname "$0")/lib.sh"

# Mutation harness hook: point AT_VERSION_SH at a mutated copy.
V="${AT_VERSION_SH:-$ROOT/tools/version.sh}"

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

pass() { echo "ok: $1"; }
ver() { (cd "$1" && sh "$V"); }

# ---------------------------------------------------------------------------
# Fixture: a two-file repo with one commit.
# ---------------------------------------------------------------------------
R="$TMP/repo"
mkdir -p "$R"
(
    cd "$R" &&
    git init -q &&
    git config user.email test@invalid && git config user.name test &&
    printf 'alpha\n' > a.c && printf 'beta\n' > b.c &&
    git add . && git commit -qm one
) || fail "could not build the fixture repo"
HEAD12="$(cd "$R" && git rev-parse --short=12 HEAD)"
cp "$R/a.c" "$TMP/a.c.orig"   # restore via cp, never git checkout

# 1. Clean checkout: the bare hash, no suffix.
v="$(ver "$R")"
[ "$v" = "$HEAD12" ] || fail "clean checkout: got '$v', want '$HEAD12'"
pass "a clean checkout prints the bare 12-char hash"

# 2. An edited tree gets the -dirty.<8 hex> form.
printf 'edit-one\n' >> "$R/a.c"
v1="$(ver "$R")"
case "$v1" in
    "$HEAD12"-dirty.????????) ;;
    *) fail "edited tree: got '$v1', want $HEAD12-dirty.<8 hex>" ;;
esac
pass "an edited tree prints $HEAD12-dirty.<content8>"

# 3. A DIFFERENT edit must get a DIFFERENT name — the whole point. Same
#    commit, same file, different bytes.
cp "$TMP/a.c.orig" "$R/a.c"; printf 'edit-two\n' >> "$R/a.c"
v2="$(ver "$R")"
[ "$v2" != "$v1" ] || fail "two different edits share one version: $v1"
case "$v2" in "$HEAD12"-dirty.????????) ;; *) fail "second edit malformed: $v2" ;; esac
pass "a different edit gets a different suffix ($v1 vs $v2)"

# 4. Determinism: restoring the first edit's bytes restores its name.
#    Without this, the suffix could be a timestamp in disguise and still
#    pass checks 2 and 3.
cp "$TMP/a.c.orig" "$R/a.c"; printf 'edit-one\n' >> "$R/a.c"
v1again="$(ver "$R")"
[ "$v1again" = "$v1" ] || fail "same tree, different name: '$v1' then '$v1again'"
pass "the suffix is a pure function of the tree ($v1 reproduced)"

# 5. An untracked file is dirt too — this is the case a diff-only hash
#    misses, since `git diff HEAD` shows nothing for it.
cp "$TMP/a.c.orig" "$R/a.c"
vclean="$(ver "$R")"
[ "$vclean" = "$HEAD12" ] || fail "tree did not come back clean: $vclean"
printf 'new code\n' > "$R/c.c"
vu1="$(ver "$R")"
case "$vu1" in "$HEAD12"-dirty.????????) ;; *) fail "untracked file did not dirty the tree: $vu1" ;; esac
printf 'other code\n' > "$R/c.c"
vu2="$(ver "$R")"
[ "$vu2" != "$vu1" ] || fail "two different untracked contents share one version: $vu1"
rm "$R/c.c"
pass "untracked files count: presence dirties, contents distinguish"

# 6. A tarball (no .git) reads .tarball-version; absent or empty means
#    "unknown", never "-dirty" — a tarball is not dirty, it is not a checkout.
X="$TMP/export"
mkdir -p "$X" && (cd "$R" && git archive HEAD | tar -x -C "$X")
[ ! -e "$X/.git" ] || fail "export unexpectedly carries .git"
v="$(ver "$X")"
[ "$v" = "unknown" ] || fail "tarball without .tarball-version: got '$v', want 'unknown'"
printf 'v9.9.9-rc1\n' > "$X/.tarball-version"
v="$(ver "$X")"
[ "$v" = "v9.9.9-rc1" ] || fail "tarball with .tarball-version: got '$v'"
: > "$X/.tarball-version"
v="$(ver "$X")"
[ "$v" = "unknown" ] || fail "an EMPTY .tarball-version must read as unknown, got '$v'"
pass "a tarball identifies as its .tarball-version, else unknown"

# 7. A stray .tarball-version inside a checkout must NOT win: git is the
#    truth there, and the stray file is itself untracked dirt.
printf 'v0.0.0-imposter\n' > "$R/.tarball-version"
v="$(ver "$R")"
case "$v" in
    "$HEAD12"-dirty.????????) ;;
    *) fail "a checkout with a stray .tarball-version printed '$v' — the file must not mask git" ;;
esac
rm "$R/.tarball-version"
pass "inside a checkout, git wins over a stray .tarball-version"

# ---------------------------------------------------------------------------
# 8. The Makefile actually uses the script. Generate the version header in
#    the REAL checkout and require it to quote the script verbatim — if the
#    Makefile still computed its own string, everything above tests a
#    bystander.
# ---------------------------------------------------------------------------
want="$(cd "$ROOT" && sh tools/version.sh)"
( cd "$ROOT" && make -s BUILD="$BUILD" "build/$BUILD/include/at_version.h" ) ||
    fail "could not generate the version header"
got="$(sed -n 's/#define AT_VERSION "\(.*\)"/\1/p' "$ROOT/build/$BUILD/include/at_version.h")"
[ "$got" = "$want" ] || fail "Makefile disagrees with tools/version.sh: header '$got' vs script '$want'"
pass "the generated header quotes tools/version.sh verbatim ($got)"

echo "PASS: version identity names trees, tarballs and checkouts distinctly"
