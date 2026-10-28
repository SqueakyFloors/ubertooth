#!/bin/sh
# Check that tools/check-issue-references.sh answers each kind of tree the way
# it is meant to.
#
#     tools/check-issue-references-selftest.sh
#
# The check it exercises used to read xargs's exit status where it meant
# grep's, which is wrong in both directions and in a different one on each
# platform. GNU xargs answers 123 for a grep that merely found nothing, so a
# clean tree read as a scan that went wrong; the BSD one answers 1 for that
# and 1 again for a grep that really failed, so a scan that could not read a
# file read as a clean tree. Hence the two dull cases below -- a tree with no
# references at all, and a tracked file missing from the worktree -- which are
# the ones that caught it.
#
# Each case builds a throwaway repository, because the check scans the
# repository its working directory is in. Every repository gets enough files to
# clear the check's own floor on how few files may be in scope.
#
# What this does not cover, so the next reader knows: a file list long enough
# for xargs to split into several batches. The limit is a megabyte of arguments
# on the systems this runs on, and the tree is nowhere near it.
#
# Prints each case with its exit status and both output streams, so a failing
# run says what happened without a rerun. Exits 0 when every case holds, 1
# otherwise.

set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 1
check="$here/check-issue-references.sh"

if [ ! -x "$check" ]; then
    printf '%s\n' "error: $check is missing or not executable" >&2
    exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

# The number the cases plant, held in a variable because this script is itself
# one of the shell scripts the check scans: a literal here would be a
# reference of exactly the kind it refuses, and the check would fail on its own
# self-test. The workflow's planting step holds it the same way.
planted=4242

failures=0
case_name=
repos=0
repo=

fail() {
    printf '%s\n' "FAIL: $case_name: $*" >&2
    failures=$((failures + 1))
}

# A repository with enough plain source files to clear the check's floor, and
# nothing in any of them that looks like a reference.
new_repo() {
    repos=$((repos + 1))
    repo="$work/repo$repos"
    mkdir -p "$repo/src" || exit 1
    i=0
    while [ "$i" -lt 260 ]; do
        printf 'int filler%s(void) { return %s; }\n' "$i" "$i" \
            > "$repo/src/filler$i.c"
        i=$((i + 1))
    done
    git -C "$repo" init -q
    git -C "$repo" config user.email selftest@example.invalid
    git -C "$repo" config user.name "Self test"
    git -C "$repo" add -A
    git -C "$repo" commit -q -m "filler"
}

# Run the check inside the repository and report its status and streams.
run_check() {
    out="$work/out"
    err="$work/err"
    ( cd "$repo" && "$check" ) > "$out" 2> "$err"
    status=$?
    printf '\ncase: %s\n' "$case_name"
    printf '  exit status %s\n' "$status"
    sed 's/^/  stdout: /' "$out"
    sed 's/^/  stderr: /' "$err"
}

# ---- a tree with nothing to report is clean ------------------------------
# The case the fix is for: before it, grep exiting 1 because it matched
# nothing reached this script as an xargs failure, and the check reported
# "the scan itself failed" on a perfectly good tree.

case_name="a tree with no references at all is reported clean"
new_repo
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "no issue references" "$out" || fail "stdout does not report a clean tree"
[ -s "$err" ] && fail "wrote to standard error on a clean tree"

# ---- a reference is caught, wherever the rule names ----------------------

case_name="a reference in a source file is caught"
new_repo
printf '/* see #%s */\n' "$planted" >> "$repo/src/filler7.c"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "src/filler7.c:2: */\* see #$planted" "$err" ||
    fail "the planted line is not named on standard error"
[ -s "$out" ] && fail "wrote to standard output on a failure"

case_name="a reference in a per-directory build file is caught"
# Its own pathspec in the check ('*/Makefile'), which has been wrong before.
new_repo
mkdir -p "$repo/firmware/image"
printf '# see #%s\n' "$planted" > "$repo/firmware/image/Makefile"
git -C "$repo" add -A
git -C "$repo" commit -q -m "a build file"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "firmware/image/Makefile:1: *# see #$planted" "$err" ||
    fail "the planted line is not named on standard error"
[ -s "$out" ] && fail "wrote to standard output on a failure"

case_name="a full issue URL is caught as well as a bare number"
new_repo
printf '/* https://github.com/owner/repo/issues/%s */\n' "$planted" \
    >> "$repo/src/filler9.c"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "src/filler9.c:2:" "$err" || fail "the planted URL is not reported"
[ -s "$out" ] && fail "wrote to standard output on a failure"

# ---- a scan that really did go wrong still says so ----------------------
# The wrapper the fix adds must pass grep's own errors through: a file git
# knows about but the working tree does not have is a broken scan, not a
# clean tree.

case_name="a tracked file missing from the worktree fails the scan"
new_repo
rm "$repo/src/filler3.c"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "the scan itself failed" "$err" ||
    fail "a missing tracked file was not reported as a failed scan"
[ -s "$out" ] && fail "reported a file count on a failed scan"

# ---- the floor on how few files may be in scope -------------------------

case_name="too few files in scope is refused"
repos=$((repos + 1))
repo="$work/repo$repos"
mkdir -p "$repo/src"
printf 'int main(void) { return 0; }\n' > "$repo/src/only.c"
git -C "$repo" init -q
git -C "$repo" config user.email selftest@example.invalid
git -C "$repo" config user.name "Self test"
git -C "$repo" add -A
git -C "$repo" commit -q -m "one file"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "files in scope, which cannot be right" "$err" ||
    fail "the floor did not fire"
[ -s "$out" ] && fail "wrote to standard output on a failure"

# ---- outside a checkout -------------------------------------------------

# The wording here is git's, not the check's: `git rev-parse` fails and the
# check exits on that, so what reaches the user is "not a git repository".
# The check's own "not inside a git checkout" line is for the other way out,
# a rev-parse that answers with nothing.
case_name="outside a git checkout it says so"
repo="$work/not-a-repo"
mkdir -p "$repo"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
[ -s "$err" ] || fail "said nothing about not being in a checkout"
[ -s "$out" ] && fail "wrote to standard output on a failure"

printf '\n'
if [ "$failures" -eq 0 ]; then
    printf '%s\n' "check-issue-references-selftest: every case holds."
    exit 0
fi
printf '%s\n' "error: $failures case(s) failed" >&2
exit 1
