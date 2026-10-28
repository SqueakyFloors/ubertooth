#!/bin/sh
# Fail when documentation pins the firmware API version into a sample, and
# when the two documents that state the version for real have gone stale.
#
# The rule is in .kiro/steering/branch-and-review.md, "Firmware API version":
# sample output writes the version as API:<version> rather than a number, in
# the man pages, the README, docs/ and CHANGELOG.md alike, because every bump
# would otherwise have one more place to chase. What a bump does update is
# TEST_PLAN.md's expected string and COMPATIBILITY.md's firmware row, which
# state the requirement rather than illustrate output, and a version a sample
# is making a point about -- btbr's fixed 1.06, or 2020-12-R1's 1.07 -- which
# is a fixed number and stays.
#
# Nothing enforced that. The samples that went stale were found by a reviewer
# two bumps later, so this script asks both halves of the rule:
#
#   1. no documentation file writes API: followed by a digit, except where
#      the allowlist below says it is deliberate;
#   2. where it is deliberate because the document states the requirement --
#      TEST_PLAN.md and COMPATIBILITY.md -- the value is the one this tree
#      carries, so allowlisting those two files cannot hide a stale number.
#
# Run it from anywhere in a checkout:
#
#     tools/check-doc-api-version.sh
#
# It reports every problem as file:line: text on standard error and exits 1,
# or says on standard output that the documentation is clean and exits 0. CI
# runs this same script, so what fails there fails here.
#
# What it does not catch, so the next reader knows: a version written in words
# ("API version 1.35", "firmware 1.35"), and a number in a file kind the scope
# below does not name. It is a net for the spelling the tools print, which is
# the one that gets copied into a sample.

set -u

top=$(git rev-parse --show-toplevel) || exit 1
if [ -z "$top" ]; then
    echo "error: not inside a git checkout" >&2
    exit 1
fi
cd "$top" || exit 1

header=host/libubertooth/src/ubertooth_interface.h

# The version this tree carries, as the documentation spells it. Read with the
# same shape check-api-version.sh uses, and binary-coded decimal: 0x0135 is
# 1.35, so the digits are read as text and not as a number.
digits=$(sed -n \
    's/^#[ 	]*define[ 	][ 	]*UBERTOOTH_API_VERSION[ 	][ 	]*0x\([0-9][0-9][0-9][0-9]\).*/\1/p' \
    "$header" 2> /dev/null | head -1)
if [ -z "$digits" ]; then
    {
        echo "error: could not read UBERTOOTH_API_VERSION from"
        echo "       $header"
        echo ""
        echo "       Expected a line of the form"
        echo "       '#define UBERTOOTH_API_VERSION 0x0135', four decimal"
        echo "       digits. A check that had stopped finding the define would"
        echo "       pass every branch, so this is a failure rather than a"
        echo "       scan of nothing."
    } >&2
    exit 1
fi
# ${digits%??} is the major pair, ${digits#??} the minor pair. The leading
# zero of the major pair is stripped as text rather than by arithmetic: a
# POSIX shell would read "01" as octal, and 08 and 09 are versions this
# project can have.
major=$(printf '%s' "${digits%??}" | sed 's/^0*//')
[ -n "$major" ] || major=0
version="$major.${digits#??}"

# In scope: the files a reader takes output samples from. An inclusion list,
# so a new kind of documentation cannot quietly join the check and a new man
# page cannot quietly leave it. The roff pages are here as well as their
# Markdown sources: ubertooth-debug.1 is maintained by hand, so a number can
# reach it without passing through a .md at all.
#
# CMakeLists.txt and the other *.txt files are not documentation and are left
# out; so is the vendored USB stack, whose text belongs to upstream.
files=$(git ls-files -- '*.md' '*.rst' 'host/doc/man/*.1' |
        grep -v -E '^firmware/common/lpcusb/')

# A floor, so a pathspec that stops matching cannot turn this check into a
# scan of nothing that passes.
count=$(printf '%s\n' "$files" | grep -c .)
if [ "$count" -lt 70 ]; then
    echo "error: only $count documentation files in scope, which cannot be right:" >&2
    echo "       a pathspec in this script has stopped matching" >&2
    exit 1
fi

# The allowlist. Each entry is one extended regular expression matched against
# the whole "path:line:text" of a hit, so it names both the file and the shape
# of the line. Keep it narrow: a number excused here is a number no bump will
# ever chase.
#
# The roff pages escape the characters of a sample line, so "2020-12-R1"
# arrives as "2020\-12\-R1" and "1.07" as "1\.07". The entries below allow
# that backslash where one can appear rather than carrying two spellings of
# each line.
allowlist=$(mktemp) || exit 1
matches=$(mktemp) || exit 1
reported=$(mktemp) || exit 1
trap 'rm -f "$allowlist" "$matches" "$reported"' EXIT HUP INT TERM
grep -v '^#' > "$allowlist" <<'EOF'
# The btbr image's bcdDevice is a fixed 1.06 that no bump moves, and a sample
# showing btbr is making that point. Tied to the image name, so the number
# cannot be excused in a bluetooth_rxtx sample.
:[0-9]+:.*btbr .*\(API:1(\\)?\.06\)
# The 2020-12-R1 release's 1.07, likewise: the samples that show it are about
# reading a board this tree did not build.
:[0-9]+:.*2020(\\)?-12(\\)?-R1 \(API:1(\\)?\.07\)
# TEST_PLAN.md states the version a board must report, rather than
# illustrating output, so it carries the real number -- and part 2 below
# pins that number to this tree's, which is what makes excusing the file safe.
^TEST_PLAN\.md:[0-9]+:
# COMPATIBILITY.md's firmware row, for the same reason and with the same
# backstop.
^COMPATIBILITY\.md:[0-9]+:
EOF

# What the tools print: API: followed by a digit. The placeholder the rule asks
# for, API:<version>, does not match, so a sample written the right way is
# silent here.
pinned='API:[0-9]'

# grep's own errors are kept: a tracked file missing from the worktree should
# be heard about, not scanned silently as nothing.
#
# grep's status cannot be read through xargs, which reports a command that
# exited 1 as a failure of its own (123 with GNU xargs, 1 with the BSD one),
# so a scan that found nothing would read as a scan that went wrong. The
# wrapper below turns grep's two ordinary answers -- 0 matched, 1 did not --
# into success, and lets anything above that through as the failure it is. A
# batch with no files is success too, so that a grep with no file arguments
# never runs: it would read empty standard input and answer about nothing.
#
# shellcheck disable=SC2016 # $0 and $@ below belong to that shell, not this one
printf '%s\n' "$files" | tr '\n' '\0' |
    xargs -0 sh -c '
        [ "$#" -gt 0 ] || exit 0
        grep -n -H -E -- "$0" "$@"
        status=$?
        [ "$status" -le 1 ] && exit 0
        exit "$status"
    ' "$pinned" > "$matches"
status=$?
if [ "$status" -ne 0 ]; then
    echo "error: the scan itself failed (status $status)" >&2
    exit 1
fi

grep -v -E -f "$allowlist" "$matches" > "$reported"

problems=0

if [ -s "$reported" ]; then
    {
        echo "error: the firmware API version is pinned into a documentation sample:"
        sed 's/^\([^:]*:[0-9]*\):/  \1: /' "$reported"
        cat <<'EOF'

.kiro/steering/branch-and-review.md asks samples to write the version as
API:<version>, so that a bump has one place to change and not a dozen. Write
the placeholder instead of the number.

If a line is making a point about a particular version -- a release this tree
did not build, or the btbr image's fixed value -- add it to the allowlist in
tools/check-doc-api-version.sh, with the reason.
EOF
    } >&2
    problems=$((problems + 1))
fi

# Part 2: the two documents that carry the real number have to carry this
# tree's. Each is allowlisted above, so without this they could hold a version
# two bumps old and nothing would say so -- which is how the samples went
# stale in the first place.
plan_versions=$(sed -n 's/.*(API:\([0-9][0-9]*\.[0-9][0-9]*\)).*/\1/p' TEST_PLAN.md |
                sort -u)
if [ -z "$plan_versions" ]; then
    {
        echo "error: TEST_PLAN.md states no expected API: version at all."
        echo ""
        echo "       It is the plan a tester reads to confirm a board reports"
        echo "       the version this tree requires, so the line naming"
        echo "       (API:$version) has to be there."
    } >&2
    problems=$((problems + 1))
elif [ "$plan_versions" != "$version" ]; then
    {
        echo "error: TEST_PLAN.md expects an API version this tree does not carry."
        echo ""
        echo "         this tree:      $version ($header)"
        echo "         TEST_PLAN.md:   $(printf '%s' "$plan_versions" | tr '\n' ' ')"
        echo ""
        echo "       A bump moves TEST_PLAN.md's expected string with it."
        grep -n "API:[0-9]" TEST_PLAN.md | sed 's/^/         /'
    } >&2
    problems=$((problems + 1))
fi

if ! grep -q -F "$version" COMPATIBILITY.md; then
    {
        echo "error: COMPATIBILITY.md does not mention API version $version."
        echo ""
        echo "       Its firmware row states the version a board must report,"
        echo "       so a bump moves that row with it."
    } >&2
    problems=$((problems + 1))
fi

if [ "$problems" -ne 0 ]; then
    exit 1
fi

echo "check-doc-api-version: $count documentation files scanned, no pinned version; TEST_PLAN.md and COMPATIBILITY.md are at $version."
exit 0
