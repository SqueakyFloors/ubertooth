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
# It prints every match as file:line: text and exits 1, or says the tree is
# clean and exits 0. CI runs this same script, so what fails there fails here.

set -u

cd "$(git rev-parse --show-toplevel)" || exit 1

# In scope: the files the rule names. An inclusion list rather than an
# exclusion one, so a new kind of documentation cannot quietly join the check
# and a new source file cannot quietly leave it. The cost of that choice is
# that a kind of source file nobody thought of is not scanned, so the list
# covers the spellings a C, C++, Python, shell, CMake, make or packaging file
# can arrive under, and the *.in templates those are generated from, not only
# the ones the tree happens to use today.
#
# Assembly (*.S, *.s) is left out, because an immediate operand is spelt the
# same way as a reference and every line of one would need excusing. Inline
# assembly inside a C or C++ file is handled by the allowlist below.
#
# Vendored trees are left out because the rule says to leave them as they are.
files=$(git ls-files -- \
            '*.c' '*.h' '*.cc' '*.cpp' '*.cxx' '*.hh' '*.hpp' \
            '*.py' '*.sh' '*.rb' '*.bat' \
            'CMakeLists.txt' '*.cmake' 'Makefile' '*.mk' '*.ld' '*.in' \
            '.github/workflows/*.yml' '.github/workflows/*.yaml' \
            'Dockerfile' 'Jenkinsfile' |
        grep -v -E '^(host/cmake/modules/|firmware/common/lpcusb/)')

if [ -z "$files" ]; then
    echo "check-issue-references: nothing in scope, which cannot be right" >&2
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
cat > "$allowlist" <<'EOF'
# ARM immediates in the reset-vector assembly, which is written as quoted asm
# lines inside the C++ file.
^firmware/bootloader/bootloader\.cpp:[0-9]+:[[:space:]]*"
# The libusb descriptor message, which this test asserts on word for word.
^host/ubertooth-tools/tests/test_device_select\.c:[0-9]+:.*dev #1!
# The allowlist entries above, as they appear in this file.
^tools/check-issue-references\.sh:[0-9]+:\^
EOF
grep -v '^#' "$allowlist" > "$allowlist.patterns" && mv "$allowlist.patterns" "$allowlist"

matches=$(mktemp) || exit 1
reported=$(mktemp) || exit 1

# Both spellings the rule reserves for commit messages, pull request
# descriptions and CHANGELOG.md: the bare number, and the full URL that is the
# correct form *there*. A URL in a source comment is the same reference wearing
# a hat, and nothing in the tree uses one, so there is no cost to refusing it.
reference='#[0-9]+|github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/(issues|pull)/[0-9]+'

printf '%s\n' "$files" | tr '\n' '\0' |
    xargs -0 grep -n -H -E -- "$reference" > "$matches" 2>/dev/null

grep -v -E -f "$allowlist" "$matches" > "$reported"

if [ ! -s "$reported" ]; then
    echo "check-issue-references: no issue numbers in source, tests or build files."
    exit 0
fi

sed 's/^\([^:]*:[0-9]*\):/\1: /' "$reported"
cat >&2 <<'EOF'

The lines above put an issue or pull request number in a source, test or build
file, which .kiro/steering/branch-and-review.md does not allow: this fork and
upstream number issues independently, so a bare number does not say which
tracker it belongs to.

Say in the comment why the code is the way it is, and put the link in the
commit message, the pull request description or CHANGELOG.md, as a full URL or
owner/repo#N.

If a match is not an issue reference, add it to the allowlist in
tools/check-issue-references.sh, with the reason.
EOF
exit 1
