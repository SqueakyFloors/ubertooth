#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Keep LIBBTBB_REQUIRED_FUNCTIONS in host/cmake/modules/FindBTBB.cmake equal to
# the libbtbb functions the host sources call.
#
# That list is what the configure step probes the installed libbtbb for, so an
# installed libbtbb too old for this tree is refused by name instead of
# arriving as an undeclared-function error in the middle of libubertooth.
# libbtbb has no version macro, so the declarations the code needs are the only
# thing there is to ask about -- and the list only keeps answering for the
# whole tree while it matches the calls. This is what keeps it matching:
#
#     tools/check-btbb-symbols.sh
#
# It prints what it compared on standard output and exits 0, or reports the
# differences on standard error and exits 1. The scripts CI job runs it, so
# what fails there fails here.
#
# Scope is the two directories whose code is shipped, host/libubertooth/src and
# host/ubertooth-tools/src. The tests are left out: one of them defines a stub
# of a libbtbb function, which no scan can tell from a call, and the configure
# check exists for the tools a user builds. So the tests are uncovered both
# ways: a libbtbb function only a test calls is not required of an installed
# libbtbb, and the test build is where its absence would turn up.
#
# What it does not catch, so the next reader knows: a call reached through a
# function pointer or assembled from a macro, and a libbtbb function whose
# signature changed without its name changing. It compares names.

set -u

top=$(git rev-parse --show-toplevel) || exit 1
if [ -z "$top" ]; then
    echo "error: not inside a git checkout" >&2
    exit 1
fi
cd "$top" || exit 1

module=host/cmake/modules/FindBTBB.cmake
listed=$(mktemp) || exit 1
called=$(mktemp) || exit 1
trap 'rm -f "$listed" "$called"' EXIT HUP INT TERM

# The list, read between the set() that opens it and the line that closes it.
sed -n '/^set(LIBBTBB_REQUIRED_FUNCTIONS$/,/^)$/p' "$module" |
    sed -n 's/^  \([a-z_][a-z0-9_]*\)$/\1/p' | sort -u > "$listed"
if [ ! -s "$listed" ]; then
    echo "error: no function list found in $module:" >&2
    echo "       the set(LIBBTBB_REQUIRED_FUNCTIONS ...) block this script" >&2
    echo "       reads has been renamed or reformatted" >&2
    exit 1
fi

sources=$(git ls-files -- 'host/libubertooth/src/*.c' 'host/libubertooth/src/*.h' \
                          'host/ubertooth-tools/src/*.c' 'host/ubertooth-tools/src/*.h')
# A floor, so a pathspec that stops matching cannot turn this into a comparison
# against nothing.
count=$(printf '%s\n' "$sources" | grep -c .)
if [ "$count" -lt 20 ]; then
    echo "error: only $count source files in scope, which cannot be right:" >&2
    echo "       a pathspec in this script has stopped matching" >&2
    exit 1
fi

# Comments are stripped before the scan, because this project's comments name
# the libbtbb functions they talk about and a comment is free to name one the
# code does not call -- the function a call replaced, most likely. Counting
# those as calls would demand them of every installed libbtbb.
#
# Strings are not stripped, so a "//" inside one ends the line early and a
# "/*" inside one swallows the lines up to the next "*/". A URL in a string
# literal already does the first, and the cost is a missing candidate, which
# is reported as a listed function nothing calls: loud, and never a build that
# breaks. Nothing would be gained by lexing C to avoid it.
# in_block is cleared at the start of each file, so one unterminated comment
# cannot carry into the next.
# shellcheck disable=SC2016  # $0 and FNR below belong to awk, not to the shell
printf '%s\n' "$sources" | tr '\n' '\0' |
    xargs -0 awk '
    FNR == 1 { in_block = 0 }
    {
        line = $0
        code = ""
        while (length(line)) {
            if (in_block) {
                end = index(line, "*/")
                if (end == 0) { line = ""; break }
                line = substr(line, end + 2)
                in_block = 0
                continue
            }
            block = index(line, "/*")
            rest = index(line, "//")
            if (rest > 0 && (block == 0 || rest < block)) {
                code = code substr(line, 1, rest - 1)
                line = ""
                break
            }
            if (block > 0) {
                code = code substr(line, 1, block - 1)
                line = substr(line, block + 2)
                in_block = 1
                continue
            }
            code = code line
            line = ""
        }
        print code
    }' |
    grep -o -E '\b(btbb|lell)_[a-zA-Z0-9_]*[[:space:]]*\(' |
    sed 's/[[:space:]]*($//' | sort -u > "$called"

# No floor on the number of calls found, unlike the file count above: a scan
# that stopped matching reports all of the listed functions as called by
# nothing, which is as loud as a floor would be.
unlisted=$(comm -13 "$listed" "$called")
uncalled=$(comm -23 "$listed" "$called")

if [ -z "$unlisted" ] && [ -z "$uncalled" ]; then
    echo "check-btbb-symbols: $count files scanned, $(grep -c . "$listed") functions, list matches."
    exit 0
fi

{
    if [ -n "$unlisted" ]; then
        echo "error: the host sources call libbtbb functions $module does not list:"
        printf '%s\n' "$unlisted" | sed 's/^/  /'
    fi
    if [ -n "$uncalled" ]; then
        echo "error: $module lists libbtbb functions no host source calls:"
        printf '%s\n' "$uncalled" | sed 's/^/  /'
    fi
    cat <<EOF

That list is what the configure step asks the installed libbtbb for. A
function missing from it is one an old libbtbb can lack without the configure
step noticing, and the build then fails with "call to undeclared function"
in the middle of libubertooth. One the list names but nothing calls would
refuse a libbtbb this tree does not need.

Add or remove the names above, keeping the list sorted and one name per line.
EOF
} >&2
exit 1
