#!/usr/bin/env bash
# tools/check_redaction.sh keeps cloud metadata and personal paths out of the
# public tree. This test proves the guard can actually see each thing it
# claims to catch — a scanner is a list of assertions about regexes, and an
# untested regex is a guess — and that it can never report clean by accident.
#
# Every planted sample below is assembled from concatenated pieces, for the
# same reason the guard's own patterns are: this file is tracked, so the real
# scan reads it, and a literal sample here would be a self-inflicted finding.
set -u

. "$(dirname "$0")/lib.sh"

CHECK="$ROOT/tools/check_redaction.sh"
[ -f "$CHECK" ] || fail "missing $CHECK"

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

pass() { echo "ok: $1"; }

mkrepo() { # dir
    mkdir -p "$1"
    (
        cd "$1" &&
        git init -q &&
        git config user.email test@invalid && git config user.name test &&
        printf 'nothing to see\n' > ok.txt &&
        git add -A && git commit -qm fixture
    ) || fail "could not build fixture repo $1"
}
commit_all() { (cd "$1" && git add -A && git commit -qm more); }

# 1. A clean repo passes and says how many files it read.
R="$TMP/clean"; mkrepo "$R"
out="$(sh "$CHECK" "$R")" || fail "clean fixture failed: $out"
grep -q '1 tracked files' <<< "$out" || fail "did not report the file count: $out"
pass "clean fixture passes and reports the count"

# 2. Each pattern the guard claims to catch, planted one at a time. The
#    samples are assembled so THIS file stays clean under the real scan.
s_home='/Use''rs/somedeveloper'
s_arn='arn:aw''s:iam::1:role/x'
s_host='sqs.us-east-1.amazonaw''s.com'
s_s3='s3:''//some-bucket/key'
s_mail='someone@amaz''on.com'
s_acct='Accoun''t id: 123456789012'
plant() { # name contents
    R="$TMP/bad-$1"; mkrepo "$R"
    printf '%s\n' "$2" > "$R/leak.md"; commit_all "$R"
    out="$(sh "$CHECK" "$R" 2>&1)"; rc=$?
    [ "$rc" -eq 1 ] || fail "sample '$1' not caught (rc=$rc): $out"
    grep -q 'leak.md' <<< "$out" || fail "sample '$1' caught but the file not named: $out"
}
plant home     "logs in $s_home/x.log"
plant arn      "role is $s_arn"
plant hostname "endpoint $s_host here"
plant s3url    "fetch from $s_s3"
plant email    "contact $s_mail please"
plant account  "$s_acct"
pass "all six pattern families are caught, each naming the file"

# 3. An UNTRACKED violation is invisible by design — the scan's scope is what
#    GitHub serves. This pins the scope so a future rewrite that switches to
#    a filesystem walk (and starts flagging local build residue) fails here
#    and has to say why.
R="$TMP/untracked"; mkrepo "$R"
printf '%s\n' "$s_home/secret" > "$R/local-scratch.log"
out="$(sh "$CHECK" "$R")" || fail "untracked violation must not fail the scan (scope is the tree): $out"
pass "untracked files are out of scope, tracked ones are the claim"

# 4. A scan that cannot see anything must not read as clean.
out="$(sh "$CHECK" "$TMP/does-not-exist" 2>&1)"; rc=$?
[ "$rc" -eq 3 ] || fail "missing dir: want rc=3, got rc=$rc: $out"
D="$TMP/notarepo"; mkdir -p "$D"
out="$(sh "$CHECK" "$D" 2>&1)"; rc=$?
[ "$rc" -eq 3 ] || fail "non-repo dir: want rc=3, got rc=$rc: $out"
R="$TMP/empty"; mkdir -p "$R"
( cd "$R" && git init -q )
out="$(sh "$CHECK" "$R" 2>&1)"; rc=$?
[ "$rc" -eq 3 ] || fail "repo tracking 0 files: want rc=3, got rc=$rc: $out"
grep -q '0 files' <<< "$out" || fail "the zero-file refusal does not say so: $out"
pass "missing dir, non-repo and zero tracked files all refuse loudly (rc=3)"

# 5. The real checkout is clean — including this file and the guard itself,
#    which the scan reads like anything else.
out="$(sh "$CHECK" "$ROOT" 2>&1)" || fail "the repository itself fails its own scan: $out"
pass "the real tree passes its own scan"

echo "PASS: the redaction guard catches what it claims and cannot pass vacuously"
