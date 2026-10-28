#!/bin/sh
# Check that tools/check-doc-api-version.sh answers each kind of tree the way
# it is meant to.
#
#     tools/check-doc-api-version-selftest.sh
#
# The check it exercises has two halves that pull against each other: it
# refuses a version pinned into a sample, and it excuses four shapes where a
# number is deliberate. An allowlist that is too wide passes the stale samples
# this check exists to find, and one that is too narrow goes red on the fixed
# numbers in the real tree. So the cases below plant a number in each shape --
# excused and not -- and assert which way the check answers.
#
# Each case builds a throwaway repository, because the check reads the
# repository its working directory is in. Every repository gets enough
# documentation files to clear the check's own floor on how few may be in
# scope, a firmware header to read the version from, and the two documents
# that carry the real number.
#
# Prints each case with its exit status and both output streams, so a failing
# run says what happened without a rerun. Exits 0 when every case holds, 1
# otherwise.

set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 1
check="$here/check-doc-api-version.sh"

if [ ! -x "$check" ]; then
    printf '%s\n' "error: $check is missing or not executable" >&2
    exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

# The version the throwaway trees carry, and a stale one to plant. Held in
# variables so a reader can see at a glance which number each case is about.
current_digits=0135
current=1.35
stale=1.33

failures=0
case_name=
repos=0
repo=

fail() {
    printf '%s\n' "FAIL: $case_name: $*" >&2
    failures=$((failures + 1))
}

# A repository shaped like this one as far as the check can tell: 80 filler
# pages with nothing pinned in them, the firmware header, and TEST_PLAN.md and
# COMPATIBILITY.md both at the current version -- 82 files in the check's
# scope, since the two documents are Markdown. Raising the check's floor past
# 82 fails every case here at once, which is the right way round to find out.
new_repo() {
    repos=$((repos + 1))
    repo="$work/repo$repos"
    mkdir -p "$repo/docs/source" "$repo/host/libubertooth/src" \
        "$repo/host/doc/man" || exit 1
    i=0
    while [ "$i" -lt 80 ]; do
        printf 'Filler page %s\n\n    Firmware version: bluetooth_rxtx git-test (API:<version>)\n' \
            "$i" > "$repo/docs/source/filler$i.rst"
        i=$((i + 1))
    done
    printf '#define UBERTOOTH_API_VERSION 0x%s\n' "$current_digits" \
        > "$repo/host/libubertooth/src/ubertooth_interface.h"
    printf -- '- [ ] Confirm: returns Firmware version: <image> <revision> (API:%s)\n' \
        "$current" > "$repo/TEST_PLAN.md"
    printf '| **Ubertooth firmware (API)** | %s (this tree), exactly |\n' \
        "$current" > "$repo/COMPATIBILITY.md"
    git -C "$repo" init -q
    git -C "$repo" config user.email selftest@example.invalid
    git -C "$repo" config user.name "Self test"
    git -C "$repo" add -A
    git -C "$repo" commit -q -m "a tree with nothing pinned"
}

# Add a file to the repository and track it, so git ls-files can see it.
add_doc() {
    path=$1
    text=$2
    mkdir -p "$repo/$(dirname "$path")" || exit 1
    printf '%s\n' "$text" > "$repo/$path"
    git -C "$repo" add -- "$path"
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

# ---- a tree written the way the rule asks is clean -----------------------

case_name="samples written as the placeholder are reported clean"
new_repo
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
grep -q "no pinned version" "$out" || fail "stdout does not report a clean tree"
[ -s "$err" ] && fail "wrote to standard error on a clean tree"

# ---- a pinned version is caught, in each kind of file --------------------

case_name="a pinned version in the README is caught"
new_repo
add_doc README.md "    Firmware version: bluetooth_rxtx git-test (API:$stale)"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "README.md:1: .*API:$stale" "$err" ||
    fail "the planted line is not named on standard error"
[ -s "$out" ] && fail "wrote to standard output on a failure"

case_name="a pinned version in a man page's Markdown source is caught"
new_repo
add_doc host/doc/ubertooth-util.md \
    "    Firmware version: bluetooth_rxtx git-test (API:$stale)"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "host/doc/ubertooth-util.md:1:" "$err" ||
    fail "the planted line is not reported"

case_name="a pinned version in a hand-maintained roff page is caught"
# The one that could reach the tree without passing through a .md: the roff
# escapes the dot, so the check has to see API:1\.33 as a pinned version too.
new_repo
add_doc host/doc/man/ubertooth-debug.1 \
    "Firmware version: bluetooth_rxtx git\\-test (API:1\\.33)"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "host/doc/man/ubertooth-debug.1:1:" "$err" ||
    fail "the planted roff line is not reported"

case_name="the current version pinned into a sample is caught too"
# Not only a stale one: a sample that happens to be right today is the one
# that goes stale at the next bump, which is the whole point of the rule.
new_repo
add_doc README.md "    Firmware version: bluetooth_rxtx git-test (API:$current)"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "README.md:1:" "$err" || fail "the planted line is not reported"

# ---- the deliberate shapes stay green -----------------------------------

case_name="1.06 is no longer excused, in a btbr sample or any other"
# btbr reported a fixed bcdDevice of 1.06 once; it reports the version the
# tree carries now, and no sample writes 1.06, so neither spelling is
# excused.
new_repo
add_doc docs/source/firmware.rst \
    "		Firmware version: btbr git-test (API:1.06)"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "docs/source/firmware.rst:1:" "$err" ||
    fail "a btbr sample at 1.06 was excused"

new_repo
add_doc docs/source/firmware.rst \
    "		Firmware version: bluetooth_rxtx git-test (API:1.06)"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "docs/source/firmware.rst:1:" "$err" ||
    fail "a bluetooth_rxtx sample at 1.06 was excused"

case_name="the 2020-12-R1 release's 1.07 is excused, in Markdown and in roff"
new_repo
add_doc host/doc/ubertooth-util.md \
    "    Firmware version: 2020-12-R1 (API:1.07)"
add_doc host/doc/man/ubertooth-util.1 \
    "Firmware version: 2020\\-12\\-R1 (API:1\\.07)"
run_check
[ "$status" -eq 0 ] || fail "expected exit 0, got $status"
[ -s "$err" ] && fail "wrote to standard error on an excused line"

# ---- the two documents that carry the real number -----------------------

case_name="a stale TEST_PLAN.md is caught"
new_repo
printf -- '- [ ] Confirm: returns (API:%s)\n' "$stale" > "$repo/TEST_PLAN.md"
git -C "$repo" add TEST_PLAN.md
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "TEST_PLAN.md expects an API version this tree does not carry" "$err" ||
    fail "a stale expected version was not reported"
grep -q "TEST_PLAN.md:   $stale" "$err" ||
    fail "the stale value is not named"

case_name="a TEST_PLAN.md with no expected version at all is caught"
new_repo
printf -- '- [ ] Confirm: it reports a version\n' > "$repo/TEST_PLAN.md"
git -C "$repo" add TEST_PLAN.md
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "states no expected API: version at all" "$err" ||
    fail "a plan with no expected version was not reported"

case_name="a COMPATIBILITY.md that never mentions this version is caught"
new_repo
printf '| **Ubertooth firmware (API)** | %s (this tree), exactly |\n' "$stale" \
    > "$repo/COMPATIBILITY.md"
git -C "$repo" add COMPATIBILITY.md
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "firmware row requires an API version" "$err" ||
    fail "a stale firmware row was not reported"

case_name="a firmware row whose required value is stale but whose prose is current"
# The real row names several versions on purpose -- the one required, the ones
# refused, the one last verified -- so asking whether the file mentions this
# version anywhere would pass a row that requires a version two bumps old.
new_repo
printf '| **Ubertooth firmware (API)** | %s (this tree), exactly | verified at %s |\n' \
    "$stale" "$current" > "$repo/COMPATIBILITY.md"
git -C "$repo" add COMPATIBILITY.md
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "firmware row requires an API version" "$err" ||
    fail "a stale required value beside current prose was not reported"

case_name="a firmware row the check cannot find is a failure, not a pass"
new_repo
printf '| firmware | %s |\n' "$current" > "$repo/COMPATIBILITY.md"
git -C "$repo" add COMPATIBILITY.md
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "has no" "$err" || fail "a missing firmware row was not reported"

case_name="a plan line carrying a stale version beside the current one is caught"
# One line, two versions: reading one version per line would see the current
# one and stop.
new_repo
printf -- '- [ ] Confirm: was (API:%s), this tree reports (API:%s)\n' \
    "$stale" "$current" > "$repo/TEST_PLAN.md"
git -C "$repo" add TEST_PLAN.md
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "TEST_PLAN.md expects an API version this tree does not carry" "$err" ||
    fail "a second version on the same line was not reported"

# ---- the version the check reads itself ---------------------------------

case_name="a header the version cannot be read from is a failure, not a pass"
new_repo
printf '/* no version here */\n' \
    > "$repo/host/libubertooth/src/ubertooth_interface.h"
git -C "$repo" add host/libubertooth/src/ubertooth_interface.h
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "could not read UBERTOOTH_API_VERSION" "$err" ||
    fail "an unreadable version was not reported"
[ -s "$out" ] && fail "wrote to standard output on a failure"

case_name="a version that is not binary-coded decimal is refused"
# 0x013a is not a version this project can have, and reading it as one would
# print 1.3a to a tester as the string to expect.
new_repo
printf '#define UBERTOOTH_API_VERSION 0x013a\n' \
    > "$repo/host/libubertooth/src/ubertooth_interface.h"
git -C "$repo" add host/libubertooth/src/ubertooth_interface.h
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "could not read UBERTOOTH_API_VERSION" "$err" ||
    fail "a non-decimal version was not reported"

# ---- the scan itself ----------------------------------------------------

case_name="a tracked file missing from the worktree fails the scan"
new_repo
rm "$repo/docs/source/filler3.rst"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "the scan itself failed" "$err" ||
    fail "a missing tracked file was not reported as a failed scan"
[ -s "$out" ] && fail "reported a file count on a failed scan"

case_name="too few documentation files in scope is refused"
# Everything else about this tree is in order -- the header reads, both
# documents are current -- so the only thing left for the check to answer
# about is the size of the scope.
repos=$((repos + 1))
repo="$work/repo$repos"
mkdir -p "$repo/host/libubertooth/src"
printf 'Only page\n' > "$repo/README.md"
printf '#define UBERTOOTH_API_VERSION 0x%s\n' "$current_digits" \
    > "$repo/host/libubertooth/src/ubertooth_interface.h"
printf -- '- [ ] Confirm: returns (API:%s)\n' "$current" > "$repo/TEST_PLAN.md"
printf '| firmware | %s |\n' "$current" > "$repo/COMPATIBILITY.md"
git -C "$repo" init -q
git -C "$repo" config user.email selftest@example.invalid
git -C "$repo" config user.name "Self test"
git -C "$repo" add -A
git -C "$repo" commit -q -m "one page"
run_check
[ "$status" -eq 1 ] || fail "expected exit 1, got $status"
grep -q "documentation files in scope, which cannot be right" "$err" ||
    fail "the floor did not fire"
[ -s "$out" ] && fail "wrote to standard output on a failure"

printf '\n'
if [ "$failures" -eq 0 ]; then
    printf '%s\n' "check-doc-api-version-selftest: every case holds."
    exit 0
fi
printf '%s\n' "error: $failures case(s) failed" >&2
exit 1
