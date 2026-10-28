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
# is making a point about -- 2020-12-R1's 1.07 -- which is a fixed number and
# stays.
#
# Nothing enforced that. The samples that went stale were found by a reviewer
# two bumps later, so this script asks both halves of the rule:
#
#   1. no documentation file writes API: followed by a digit, except where
#      the allowlist below says it is deliberate;
#   2. where it is deliberate because the document states the requirement --
#      TEST_PLAN.md and COMPATIBILITY.md -- the value is the one this tree
#      carries, so allowlisting those two files cannot hide a stale number;
#   3. the CHANGELOG's top section, which says in words what the version is
#      now, says this tree's.
#
# Run it from anywhere in a checkout:
#
#     tools/check-doc-api-version.sh
#
# It reports every problem as file:line: text on standard error and exits 1,
# or says on standard output that the documentation is clean and exits 0. CI
# runs this same script, so what fails there fails here.
#
# What parts 1 and 2 do not catch is a version written in words, because the
# number alone appears in prose that is about an older version on purpose: a
# note about the 2020-12-R1 release should go on saying 1.07 forever. So there
# is no rule that could be applied to the whole of a document. Part 3 asks the
# one question that has a single right answer -- what is the version now? --
# of the one sentence that answers it, in the CHANGELOG's top section.
#
# Still not caught, so the next reader knows: a version stated in any other
# wording ("requires API version 1.35", "requires firmware 1.35"), a version
# named in a release section below the top one, and a number in a file kind
# the scope below does not name. Part 3 reads one sentence on purpose: the
# comment above it says why.

set -u

if ! top=$(git rev-parse --show-toplevel 2> /dev/null) || [ -z "$top" ]; then
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
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM
allowlist=$work/allowlist
matches=$work/matches
reported=$work/reported
grep -v '^#' > "$allowlist" <<'EOF'
# The 2020-12-R1 release's 1.07: the samples that show it are about reading a
# board this tree did not build, so the number is fixed and stays. btbr's
# 1.06 was a second such entry while that image reported a fixed bcdDevice;
# it reports the tree's version now, and no sample writes 1.06, so nothing
# excuses it.
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
did not build, say -- add it to the allowlist in
tools/check-doc-api-version.sh, with the reason.
EOF
    } >&2
    problems=$((problems + 1))
fi

# Part 2: the two documents that carry the real number have to carry this
# tree's. Each is allowlisted above, so without this they could hold a version
# two bumps old and nothing would say so -- which is how the samples went
# stale in the first place.
plan_versions=$(grep -o -E 'API:[0-9]+\.[0-9]+' TEST_PLAN.md |
                sed 's/^API://' | sort -u)
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

# The row's second cell is the version a board must report. Matching the whole
# file would pass a row whose required value is two bumps old but whose prose
# mentions this one: that row names several versions on purpose.
compat_row=$(grep -n '^|[ ]*\*\*Ubertooth firmware (API)\*\*[ ]*|' COMPATIBILITY.md |
             head -1)
if [ -z "$compat_row" ]; then
    {
        echo "error: COMPATIBILITY.md has no '**Ubertooth firmware (API)**' row."
        echo ""
        echo "       That row states the version a board must report, and this"
        echo "       check reads its required value, so a check that stopped"
        echo "       finding the row would pass anything."
    } >&2
    problems=$((problems + 1))
else
    required=$(printf '%s' "$compat_row" | awk -F'|' '{ print $3 }')
    escaped=$(printf '%s' "$version" | sed 's/\./\\./g')
    if ! printf '%s' "$required" |
         grep -q -E "(^|[^0-9.])$escaped([^0-9.]|\$)"; then
        {
            echo "error: COMPATIBILITY.md's firmware row requires an API version"
            echo "       this tree does not carry."
            echo ""
            echo "         this tree:         $version ($header)"
            echo "         COMPATIBILITY.md: $required"
            echo ""
            echo "       A bump moves that row's required value with it."
        } >&2
        problems=$((problems + 1))
    fi
fi

# Part 3: the CHANGELOG's top section says what the version is now, in words,
# so the samples rule above cannot reach it. Only the top section is read:
# everything below it is the history of earlier releases, where an old version
# is the right thing to say.
#
# Only the one sentence a bump is asked to write is read, not every mention of
# a version. The top section talks about other versions on purpose -- btbr's
# fixed 1.06 that never moved, 2020-12-R1 firmware being refused -- so a rule
# that fired on any number near the words "API version" would go red on prose
# that is correct, with nothing to do about it but reword the prose.
changelog=CHANGELOG.md
# Not $top, which is the repository root above.
section=$work/section
stream=$work/section-stream
if [ ! -f "$changelog" ]; then
    {
        echo "error: $changelog is missing."
        echo ""
        echo "       Its top section states the firmware API version in words,"
        echo "       which is the one place a bump has to say the number out"
        echo "       loud, so a check that could not find the file would pass"
        echo "       any version at all."
    } >&2
    problems=$((problems + 1))
else
    # The top section: its heading and every line down to the next release
    # heading. Line numbers are carried along so a problem can be pointed at.
    #
    # Headings inside a fenced code block are text, not headings: a sample
    # showing how to write an entry would otherwise become the top section,
    # and one inside the top section would end it early and hide the rest.
    # Only backtick fences are tracked, which is what this file uses.
    awk '
        /^[ \t]*```/ { fence = !fence }
        !fence && /^## / { sections++; if (sections > 1) exit }
        sections == 1 { print FNR ": " $0 }
    ' "$changelog" > "$section"
    if [ ! -s "$section" ]; then
        {
            echo "error: $changelog has no '## ' release heading, so it has no"
            echo "       top section to read the version from."
        } >&2
        problems=$((problems + 1))
    else
        # The section as one line, so a sentence that wraps still reads as a
        # sentence: this file wraps at about 78 columns, and the number can
        # land on either side of the break. Runs of whitespace collapse to one
        # space for the same reason, and the line numbers come off.
        sed 's/^[0-9]*: //' "$section" | tr '\n\t' '  ' | tr -s ' ' > "$stream"
        # Every version the section states as the current one. grep -o rather
        # than a sed substitution, which would keep only the last match on the
        # line and so miss a stale number sitting beside a current one -- the
        # hole part 2 above has a case for. The number may be wrapped in
        # emphasis or backticks.
        stated=$(grep -o -E 'firmware API version is now [*`]*[0-9]+\.[0-9]+' \
                     "$stream" |
                 grep -o -E '[0-9]+\.[0-9]+$' | sort -u)
        if [ -z "$stated" ]; then
            {
                echo "error: $changelog's top section does not say what the"
                echo "       firmware API version is now."
                echo ""
                echo "         this tree: $version ($header)"
                echo ""
                echo "       .kiro/steering/branch-and-review.md asks a bump to"
                echo "       write that line, in the form 'the firmware API"
                echo "       version is now $version'. Without it a reader has"
                echo "       no statement of the version to go stale -- and"
                echo "       this check would pass a tree that had dropped it."
                echo ""
                echo "       Only that wording is read. A version the section"
                echo "       mentions for another reason -- a release this tree"
                echo "       did not build, an old value that never moved -- is"
                echo "       left alone, so the sentence has to be there in so"
                echo "       many words."
            } >&2
            problems=$((problems + 1))
        elif [ "$stated" != "$version" ]; then
            {
                echo "error: $changelog's top section states an API version this"
                echo "       tree does not carry."
                echo ""
                echo "         this tree:      $version ($header)"
                echo "         the CHANGELOG:  $(printf '%s' "$stated" | tr '\n' ' ')"
                echo ""
                echo "       A bump moves that line with it. Sections below the"
                echo "       top one are not read: an earlier release's note"
                echo "       keeps the version it shipped."
                grep -E 'firmware API version is now' "$section" |
                    sed "s|^|         $changelog:|"
            } >&2
            problems=$((problems + 1))
        fi
    fi
fi

if [ "$problems" -ne 0 ]; then
    exit 1
fi

echo "check-doc-api-version: $count documentation files scanned, no pinned version; TEST_PLAN.md, COMPATIBILITY.md and CHANGELOG.md are at $version."
exit 0
