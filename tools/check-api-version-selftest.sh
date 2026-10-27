#!/bin/sh
# Check that tools/check-api-version.sh accepts and refuses the branches it is
# meant to.
#
#     tools/check-api-version-selftest.sh
#
# The check it exercises guards a rule that cost three nights of hand-checking
# and still let two pull requests claim a version main had taken, so it has to
# be right in both directions: refusing a good branch is as bad as passing a
# bad one, because a check that cries wolf gets ignored. Each case below builds
# a throwaway repository, puts a branch in one of the shapes the rule names,
# and asserts the status and the message.
#
# The header the cases read from is a copy of the real
# host/libubertooth/src/ubertooth_interface.h, so the value is parsed out of
# the file format the tree actually uses. The copy's version line is rewritten
# per case, and a failure to rewrite it stops the run rather than leaving every
# case comparing the same number.
#
# Prints each case with its exit status and both output streams, so a failing
# run says what happened without a rerun. Exits 0 when every case holds, 1
# otherwise.

set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 1
check="$here/check-api-version.sh"
top=$(git -C "$here" rev-parse --show-toplevel) || exit 1
header_path=host/libubertooth/src/ubertooth_interface.h
real_header="$top/$header_path"

if [ ! -x "$check" ]; then
    printf '%s\n' "error: $check is missing or not executable" >&2
    exit 1
fi
if [ ! -f "$real_header" ]; then
    printf '%s\n' "error: $real_header is not there, so the cases below would" >&2
    printf '%s\n' "       read a header this tree does not use" >&2
    exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

failures=0
case_name=
repos=0
repo=

fail() {
    printf '%s\n' "FAIL: $case_name: $*" >&2
    failures=$((failures + 1))
}

# Put one value in the copied header. The grep is the guard: if the real
# header's define line ever changes shape, this says so instead of leaving the
# cases below comparing whatever was there before.
set_version() {
    sed "s|^#define UBERTOOTH_API_VERSION .*|#define UBERTOOTH_API_VERSION $2|" \
        "$1/$header_path" > "$1/header.new"
    mv "$1/header.new" "$1/$header_path"
    grep -Fq "#define UBERTOOTH_API_VERSION $2" "$1/$header_path" || {
        printf '%s\n' "error: could not set the version to '$2' in the copy of" >&2
        printf '%s\n' "       $header_path: its define line has changed shape" >&2
        exit 1
    }
}

commit() {
    git -C "$1" add -A
    git -C "$1" commit -q -m "$2"
}

# A line appended to a file, enough to make a commit touch it. What the line
# says does not matter: the check reads paths, not content.
edit() {
    printf '/* changed */\n' >> "$1/$2"
}

# A throwaway repository holding the paths the check cares about: main with the
# given version, and a branch "work" cut from it, checked out.
new_repo() {
    repos=$((repos + 1))
    repo="$work/repo$repos"
    mkdir -p "$repo"
    git init -q "$repo"
    # Named here rather than left to init.defaultBranch, which an older git
    # does not read.
    git -C "$repo" symbolic-ref HEAD refs/heads/main
    git -C "$repo" config user.name "api version selftest"
    git -C "$repo" config user.email "selftest@invalid"
    mkdir -p "$repo/host/libubertooth/src" "$repo/firmware/bluetooth_rxtx" \
        "$repo/firmware/common" "$repo/firmware/btbr" "$repo/firmware/tests"
    cp "$real_header" "$repo/$header_path"
    set_version "$repo" "$1"
    for f in firmware/bluetooth_rxtx/bluetooth_rxtx.c firmware/common/usb.c \
        firmware/btbr/btbr.c firmware/tests/test_firmware.c \
        host/libubertooth/src/ubertooth.c; do
        printf '/* %s */\n' "$f" > "$repo/$f"
    done
    commit "$repo" "the tree as main has it"
    git -C "$repo" checkout -q -b work
}

# Run the check with its working directory inside a repository, keeping its
# status and both streams. No set -e: most cases are about a check that is
# meant to fail.
run() {
    case_name=$1
    in_repo=$2
    shift 2
    printf '\n%s\n' "case: $case_name"
    printf '%s\n' "  tools/check-api-version.sh $*"
    (cd "$in_repo" && "$check" "$@") > "$work/out" 2> "$work/err"
    status=$?
    printf '%s\n' "  exit status $status"
    sed 's/^/  stderr: /' "$work/err"
    sed 's/^/  stdout: /' "$work/out"
}

want_status() {
    [ "$status" -eq "$1" ] || fail "exit status $status, expected $1"
}

want_stdout() {
    grep -qF -- "$1" "$work/out" || fail "standard output does not mention '$1'"
}

want_stderr() {
    grep -qF -- "$1" "$work/err" || fail "standard error does not mention '$1'"
}

want_empty_stdout() {
    [ ! -s "$work/out" ] || fail "wrote to standard output and should not have"
}

want_empty_stderr() {
    [ ! -s "$work/err" ] || fail "wrote to standard error and should not have"
}

# A branch that changes the firmware interface and leaves the version where
# main already has it: the collision this check exists for.
new_repo 0x0131
edit "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
commit "$repo" "change the firmware, keep the version"
run "firmware changed, version is main's" "$repo" main
want_status 1
want_empty_stdout
want_stderr "has to be above the one main carries"
want_stderr "at HEAD:   1.31 (0x0131)"
want_stderr "on main:   1.31 (0x0131)"
want_stderr "next free: 0x0132"
want_stderr "firmware/bluetooth_rxtx/bluetooth_rxtx.c"

# The same branch with the version bumped.
set_version "$repo" 0x0132
commit "$repo" "take the next version"
run "firmware changed, version bumped" "$repo" main
want_status 0
want_empty_stderr
want_stdout "takes 1.32 (0x0132), above main's 1.31 (0x0131)"

# The case comparing with the merge base misses, and the one that let two
# pull requests through: the branch took the next value when main was on
# 0x0130, then main merged a branch that took the same one. Nothing about the
# branch changed, and it is now wrong.
new_repo 0x0130
edit "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
set_version "$repo" 0x0131
commit "$repo" "change the firmware and take 0x0131"
git -C "$repo" checkout -q main
edit "$repo" firmware/common/usb.c
set_version "$repo" 0x0131
commit "$repo" "another branch got there first"
git -C "$repo" checkout -q work
run "main moved to the value this branch claims" "$repo" main
want_status 1
want_empty_stdout
want_stderr "at HEAD:   1.31 (0x0131)"
want_stderr "on main:   1.31 (0x0131)"
want_stderr "next free: 0x0132"

# Rebasing it onto that main and taking the next value is what fixes it.
set_version "$repo" 0x0132
commit "$repo" "take the next value instead"
run "the same branch, moved up one" "$repo" main
want_status 0
want_stdout "takes 1.32 (0x0132), above main's 1.31 (0x0131)"

# A version below main's is refused as plainly as an equal one.
new_repo 0x0131
edit "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
set_version "$repo" 0x0130
commit "$repo" "change the firmware and go backwards"
run "firmware changed, version lowered" "$repo" main
want_status 1
want_stderr "at HEAD:   1.30 (0x0130)"
want_stderr "next free: 0x0132"

# A host-only branch that leaves the version alone.
new_repo 0x0131
edit "$repo" host/libubertooth/src/ubertooth.c
commit "$repo" "a host-only change"
run "host only, version untouched" "$repo" main
want_status 0
want_empty_stderr
want_stdout "no firmware interface file changed"
want_stdout "1.31 (0x0131), as the merge base had it"

# A host-only branch that bumps it anyway. Boards that are fine would be
# refused until they were reflashed, with no new firmware to reflash them to.
new_repo 0x0131
edit "$repo" host/libubertooth/src/ubertooth.c
set_version "$repo" 0x0132
commit "$repo" "a host-only change that bumps the version"
run "host only, version bumped" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"
want_stderr "at HEAD:       1.32 (0x0132)"
want_stderr "at merge base: 1.31 (0x0131)"
want_stderr "Put it back to 0x0131"

# The purest form of the same thing, and the one way this check could be made
# to pass by accident: the version lives in an interface file, so a branch
# whose only change is the bump would certify itself as the firmware change
# that justifies it.
new_repo 0x0131
set_version "$repo" 0x0132
commit "$repo" "bump the version and nothing else"
run "nothing but the bump" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"
want_stderr "Put it back to 0x0131"

# A host-only branch is not asked to chase main, only to leave the value as it
# found it: here main has moved on and the branch has not.
new_repo 0x0130
edit "$repo" host/libubertooth/src/ubertooth.c
commit "$repo" "a host-only change"
git -C "$repo" checkout -q main
edit "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
set_version "$repo" 0x0131
commit "$repo" "main moves on"
git -C "$repo" checkout -q work
run "host only, main moved on without it" "$repo" main
want_status 0
want_stdout "1.30 (0x0130), as the merge base had it"

# The mistake the BCD increment invites: the value after 0x0119 is 0x0120, and
# 0x011a is not a version at all.
new_repo 0x0129
edit "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
set_version "$repo" 0x012a
commit "$repo" "change the firmware and take a value that is not BCD"
run "a nibble above 9" "$repo" main
want_status 1
want_empty_stdout
want_stderr "0x012a, which is not"
want_stderr "binary-coded decimal"

# Which is why the suggested value carries the BCD rather than hex: after 1.39
# comes 1.40.
new_repo 0x0139
edit "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
commit "$repo" "change the firmware, keep the version"
run "the next value rolls over" "$repo" main
want_status 1
want_stderr "next free: 0x0140"

# And at the end of a major version, 1.99 is followed by 2.00.
new_repo 0x0199
edit "$repo" firmware/common/usb.c
commit "$repo" "change shared firmware code, keep the version"
run "the next value rolls into the next major version" "$repo" main
want_status 1
want_stderr "next free: 0x0200"

# Firmware the bluetooth_rxtx image does not contain: the host-compiled
# firmware tests, and the separate btbr image, whose own bcdDevice this
# version says nothing about. Neither is asked for a bump.
new_repo 0x0131
edit "$repo" firmware/tests/test_firmware.c
edit "$repo" firmware/btbr/btbr.c
commit "$repo" "firmware tests and the other image"
run "firmware tests and the btbr image are not the interface" "$repo" main
want_status 0
want_stdout "no firmware interface file changed"

# The interface header on its own counts as a firmware change: the request
# numbers and the shared structs live there.
new_repo 0x0131
set_version "$repo" 0x0132
printf '#define UBERTOOTH_SOMETHING_NEW 99\n' >> "$repo/$header_path"
commit "$repo" "add a request to the interface"
run "a new request in the interface header, version bumped" "$repo" main
want_status 0
want_stdout "changes the firmware interface"
want_stdout "takes 1.32 (0x0132)"

# The same request added without taking a new version.
new_repo 0x0131
printf '#define UBERTOOTH_SOMETHING_NEW 99\n' >> "$repo/$header_path"
commit "$repo" "add a request and keep the version"
run "a new request in the interface header, version kept" "$repo" main
want_status 1
want_stderr "has to be above the one main carries"
want_stderr "$header_path"

# A comment after the value is not part of it.
new_repo 0x0131
edit "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
set_version "$repo" "0x0132 /* next */"
commit "$repo" "change the firmware and annotate the version"
run "a comment after the value" "$repo" main
want_status 0
want_stdout "takes 1.32 (0x0132)"

# Every way of reading no value has to fail. A check that had stopped finding
# the define would otherwise pass every branch, which is the failure mode that
# makes a check worse than none.
new_repo 0x0131
grep -v UBERTOOTH_API_VERSION "$repo/$header_path" > "$repo/header.new"
mv "$repo/header.new" "$repo/$header_path"
commit "$repo" "lose the define"
run "the define is gone" "$repo" main
want_status 1
want_empty_stdout
want_stderr "defines UBERTOOTH_API_VERSION 0 times, not once"

new_repo 0x0131
printf '#define UBERTOOTH_API_VERSION 0x0132\n' >> "$repo/$header_path"
commit "$repo" "define it twice"
run "the define appears twice" "$repo" main
want_status 1
want_stderr "defines UBERTOOTH_API_VERSION 2 times, not once"

new_repo 0x0131
set_version "$repo" 0x13
commit "$repo" "a two-digit value"
run "the value is not four hex digits" "$repo" main
want_status 1
want_stderr "is '0x13', which is not"

new_repo 0x0131
git -C "$repo" rm -q "$header_path"
commit "$repo" "remove the header"
run "the header is gone" "$repo" main
want_status 1
want_stderr "does not exist at HEAD"

# No common commit means there is nothing to compare, which is a failure
# rather than a pass: in CI it is a shallow clone, where every answer would be
# made up.
new_repo 0x0131
edit "$repo" host/libubertooth/src/ubertooth.c
commit "$repo" "a host-only change"
lonely=$(git -C "$repo" commit-tree 'main^{tree}' -m lonely < /dev/null)
run "no common commit" "$repo" "$lonely"
want_status 1
want_empty_stdout
want_stderr "share no commit"

# The command line itself, by the rules in .kiro/steering/cli-output.md.
new_repo 0x0131
run "no main to compare against" "$repo"
want_status 1
want_empty_stdout
want_stderr "error: no main to compare against"

run "a ref that is not a commit" "$repo" no-such-ref
want_status 1
want_stderr "error: 'no-such-ref' is not a commit in this checkout"

run "two refs" "$repo" main other
want_status 1
want_stderr "error: one main-ref at most, and 'other' is a second"

run "an option the check does not know" "$repo" --plausible
want_status 1
want_empty_stdout
want_stderr "error: unknown option '--plausible'"

run "the help option" "$repo" -h
want_status 0
want_empty_stderr
want_stdout "usage: tools/check-api-version.sh"

printf '\n'
if [ "$failures" -ne 0 ]; then
    printf '%s\n' "error: $failures of the API version check's cases did not hold" >&2
    exit 1
fi
printf '%s\n' "check-api-version.sh refuses a claimed or stray version and passes the rest."
