#!/bin/sh
# Fail when an issue or pull request number appears in source, tests or build
# files.
#
# The rule is in .kiro/steering/branch-and-review.md, "Issue references": this
# fork and upstream number issues independently, so a bare number in the tree
# does not say which tracker it belongs to. The reason for a line of code
# belongs in the comment itself; the link belongs in the commit message, the
# pull request description or CHANGELOG.md, where a full URL is used.
#
# Run it from anywhere in a checkout:
#
#     tools/check-issue-references.sh
#
# It reports every match as file:line: text on standard error and exits 1, or
# says on standard output that the tree is clean and exits 0. CI runs this same
# script, so what fails there fails here.
#
# What it does not catch, so the next reader knows: a reference written in
# words ("issue 246", "GH-246", "# 246"), and a file that is not tracked yet,
# which git ls-files cannot see. It is a net for the common spelling, not a
# proof.

set -u

top=$(git rev-parse --show-toplevel) || exit 1
if [ -z "$top" ]; then
    echo "error: not inside a git checkout" >&2
    exit 1
fi
cd "$top" || exit 1

# In scope: the files the rule names. An inclusion list rather than an
# exclusion one, so a new kind of documentation cannot quietly join the check
# and a new source file cannot quietly leave it. The cost of that choice is
# that a kind of source file nobody thought of goes unscanned, so the list
# covers the spellings a C, C++, Python, shell, CMake, make or packaging file
# can arrive under, and the *.in templates such files are generated from, not
# only the ones the tree happens to use today.
#
# A pathspec with no wildcard in it is a path, not a name: 'Makefile' matches a
# Makefile at the root and nowhere else. Hence the '*/' forms below, which is
# how the per-directory CMakeLists.txt and Makefile files are reached.
#
# Assembly (*.S, *.s) is left out, because an immediate operand is spelt the
# same way as a reference and every line of one would need excusing. Inline
# assembly inside a C or C++ file is handled by the allowlist below.
#
# Vendored trees are left out because the rule says to leave them as they are.
files=$(git ls-files -- \
            '*.c' '*.h' '*.cc' '*.cpp' '*.cxx' '*.hh' '*.hpp' \
            '*.py' '*.sh' '*.rb' '*.bat' \
            'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' \
            'Makefile' '*/Makefile' '*.mk' '*.ld' '*.in' \
            '.github/workflows/*.yml' '.github/workflows/*.yaml' \
            'Dockerfile' 'Jenkinsfile' |
        grep -v -E '^(host/cmake/modules/|firmware/common/lpcusb/)')

# A floor, so a pathspec that stops matching cannot turn this check into a
# scan of nothing that passes. The CI job plants a reference in a build file
# as well, which catches the same mistake from the other side.
count=$(printf '%s\n' "$files" | grep -c .)
if [ "$count" -lt 250 ]; then
    echo "error: only $count files in scope, which cannot be right:" >&2
    echo "       a pathspec in this script has stopped matching" >&2
    exit 1
fi

# The allowlist. Each entry is one extended regular expression matched against
# the whole "path:line:text" of a hit, so it names both the file and the shape
# of the line: an issue number added elsewhere in an allowlisted file is still
# caught. Lines beginning with # are the reasons. Keep it narrow.
#
# This script is in scope itself, as any shell script is, and an entry has to
# be able to quote the shape it excuses. So the last entry excuses the entries:
# a line here that begins with the anchor. Anything else in this file is
# caught like anywhere else.
allowlist=$(mktemp) || exit 1
matches=$(mktemp) || exit 1
reported=$(mktemp) || exit 1
trap 'rm -f "$allowlist" "$matches" "$reported"' EXIT HUP INT TERM
grep -v '^#' > "$allowlist" <<'EOF'
# ARM immediates in the reset-vector assembly, matched on the shape of an
# operand rather than on the quoted line, so prose smuggled into a string in
# that file is still caught.
^firmware/bootloader/bootloader\.cpp:[0-9]+:.*, #[0-9]+
# The libusb descriptor message, which this test asserts on word for word.
^host/ubertooth-tools/tests/test_device_select\.c:[0-9]+:.*dev #1!
# The allowlist entries above, as they appear in this file.
^tools/check-issue-references\.sh:[0-9]+:\^
EOF

# Both spellings the rule reserves for commit messages, pull request
# descriptions and CHANGELOG.md: the bare number, and the full URL that is the
# correct form *there*. A URL in a source comment is the same reference wearing
# a hat, and nothing in the tree uses one, so there is no cost to refusing it.
reference='#[0-9]+|github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/(issues|pull)/[0-9]+'

# grep's own errors are kept: a tracked file missing from the worktree should
# be heard about, not scanned silently as nothing.
#
# grep's status cannot be read through xargs, which reports a command that
# exited 1 as a failure of its own (123 with GNU xargs, 1 with the BSD one),
# so a scan that found nothing would read as a scan that went wrong. The
# wrapper below turns grep's two ordinary answers -- 0 matched, 1 did not --
# into success, and lets anything above that through as the failure it is.
# A batch with no files at all is success too: without that, GNU xargs runs
# the command once on empty input and a grep with no file arguments would
# read the pipe it was fed from.
printf '%s\n' "$files" | tr '\n' '\0' |
    xargs -0 sh -c '
        [ "$#" -gt 0 ] || exit 0
        grep -n -H -E -- "$0" "$@"
        status=$?
        [ "$status" -le 1 ] && exit 0
        exit "$status"
    ' "$reference" > "$matches"
status=$?
if [ "$status" -ne 0 ]; then
    echo "error: the scan itself failed (status $status)" >&2
    exit 1
fi

grep -v -E -f "$allowlist" "$matches" > "$reported"

if [ ! -s "$reported" ]; then
    echo "check-issue-references: $count files scanned, no issue references."
    exit 0
fi

{
    echo "error: issue or pull request numbers in source, tests or build files:"
    sed 's/^\([^:]*:[0-9]*\):/  \1: /' "$reported"
    cat <<'EOF'

.kiro/steering/branch-and-review.md does not allow them there: this fork and
upstream number issues independently, so a bare number does not say which
tracker it belongs to.

Say in the comment why the code is the way it is, and put the link in the
commit message, the pull request description or CHANGELOG.md, as a full URL or
owner/repo#N.

If a match is not a reference, add it to the allowlist in
tools/check-issue-references.sh, with the reason.
EOF
} >&2
exit 1
