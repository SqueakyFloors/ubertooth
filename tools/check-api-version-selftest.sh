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

# A line of something that is not a comment appended to a file: a change the
# check is meant to see. It need not be valid C, Makefile or linker script,
# because the filter the check applies strips comments and blank lines and
# reads whatever is left as content.
edit() {
    printf 'CHANGED_%s = 1\n' "$((changes = changes + 1))" >> "$1/$2"
}
changes=0

# A comment appended to a file: a change the check is meant to ignore,
# whichever of the forms the tree uses.
edit_comment() {
    printf '/* a comment on one line */\n' >> "$1/$2"
}

edit_block_comment() {
    printf '/*\n * a comment over several lines\n */\n' >> "$1/$2"
}

# The form the header normalisation used to miss: a block whose interior line
# begins with neither / nor *.
edit_prose_comment() {
    printf '/*\nplain prose with no leading star\n*/\n' >> "$1/$2"
}

# A throwaway repository holding the paths the check cares about: main with the
# given version, and a branch "work" cut from it, checked out.
new_repo() {
    repos=$((repos + 1))
    repo="$work/repo$repos"
    mkdir -p "$repo"
    # An empty template, because the default one copies a directory of sample
    # hooks into every one of these repositories and this script makes dozens.
    git init -q --template= "$repo"
    # Named here rather than left to init.defaultBranch, which an older git
    # does not read.
    git -C "$repo" symbolic-ref HEAD refs/heads/main
    git -C "$repo" config user.name "api version selftest"
    git -C "$repo" config user.email "selftest@invalid"
    mkdir -p "$repo/host/libubertooth/src" "$repo/firmware/bluetooth_rxtx" \
        "$repo/firmware/common/lpcusb/target" "$repo/firmware/btbr" \
        "$repo/firmware/tests"
    cp "$real_header" "$repo/$header_path"
    set_version "$repo" "$1"
    # The seeded line is code rather than a comment, so a file the check
    # watches starts out with content it can see.
    for f in firmware/bluetooth_rxtx/bluetooth_rxtx.c firmware/bluetooth_rxtx/README \
        firmware/bluetooth_rxtx/flash \
        firmware/bluetooth_rxtx/Makefile firmware/common/usb.c \
        firmware/common/lpcusb/target/usbhw_lpc.c \
        firmware/common/LPC17xx_Linker_Script_with_bootloader.ld \
        firmware/common/sections.ld \
        firmware/common/LPC17xx_Linker_Script_bootloader.ld \
        firmware/common.mk firmware/Makefile \
        firmware/btbr/btbr.c firmware/tests/test_firmware.c \
        host/libubertooth/src/ubertooth.c CHANGELOG.md COMPATIBILITY.md \
        TEST_PLAN.md; do
        printf 'SEEDED = %s\n' "$f" > "$repo/$f"
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

want_no_stdout() {
    grep -qF -- "$1" "$work/out" && fail "standard output mentions '$1' and should not"
    return 0
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

# A capital X is as valid a hex literal to the compiler, so refusing it here
# would be a red about nothing.
new_repo 0x0131
edit "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
set_version "$repo" 0X0132
commit "$repo" "change the firmware and write the value with a capital X"
run "a capital X in the literal" "$repo" main
want_status 0
want_stdout "takes 1.32 (0x0132)"

# A comment in the interface header is not the interface. Asking a typo fix
# for a bump would be a false red, and a bump costs every user a reflash plus
# CHANGELOG, COMPATIBILITY.md, TEST_PLAN.md and test-string churn.
new_repo 0x0131
printf '/* a clearer word for the same thing */\n' >> "$repo/$header_path"
commit "$repo" "reword a comment in the interface header"
run "a comment in the interface header is not a change to it" "$repo" main
want_status 0
want_stdout "no firmware interface file changed"

# The other direction, and the bypass a reviewer found: a comment is not the
# firmware change that would justify a bump, so a branch cannot authorise its
# own by rewording one.
new_repo 0x0131
printf '/* a clearer word for the same thing */\n' >> "$repo/$header_path"
set_version "$repo" 0x0132
commit "$repo" "reword a comment and bump the version"
run "a comment cannot authorise its own bump" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"
want_stderr "Put it back to 0x0131"

# The same bypass through a file that merely sits in a watched directory. A
# README cannot change what the host sees, so appending a line to one must not
# buy a version.
new_repo 0x0131
edit "$repo" firmware/bluetooth_rxtx/README
set_version "$repo" 0x0132
commit "$repo" "document the firmware and bump the version"
run "a README cannot authorise its own bump" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"

# The watched list says ".c", meaning a name ending in a dot and a c, and a
# file called "flash" is not one: it ends in an s and an h. The distinction
# matters because the list is read twice, once to compare two commits and once
# for --list-watched, and the two have to agree. They did not: handing the
# pattern to awk with -v turned every \. in it into a . that matches any
# character, which is how "flash" came to be a watched file to one reader and
# not to the other. A flashing helper beside the sources is a plausible file
# to add, and asking it for a version would be a red about nothing.
new_repo 0x0131
edit "$repo" firmware/bluetooth_rxtx/flash
set_version "$repo" 0x0132
commit "$repo" "change a helper script beside the sources and bump"
run "a dot in the watched list is a dot" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"

# The image's own Makefile is the list of the sources that go into it, so
# adding or removing one of them is a change to the image.
new_repo 0x0131
edit "$repo" firmware/bluetooth_rxtx/Makefile
commit "$repo" "change which sources the image is built from"
run "the image's Makefile is a firmware change" "$repo" main
want_status 1
want_stderr "has to be above the one main carries"
want_stderr "firmware/bluetooth_rxtx/Makefile"

# firmware/common.mk is a known gap rather than an oversight, so it is pinned
# here: the reason it is not watched is in tools/check-api-version.sh, and a
# case that fails if someone adds it without reading that reason is the point.
new_repo 0x0131
edit "$repo" firmware/common.mk
commit "$repo" "change the shared build file"
run "the shared build file is a known gap" "$repo" main
want_status 0
want_stdout "no firmware interface file changed"

# The linker script the image is laid out with decides where things land, so
# it counts, and so does the file it includes.
new_repo 0x0131
edit "$repo" firmware/common/LPC17xx_Linker_Script_with_bootloader.ld
commit "$repo" "change the memory layout"
run "the image's linker script is a firmware change" "$repo" main
want_status 1
want_stderr "has to be above the one main carries"
want_stderr "LPC17xx_Linker_Script_with_bootloader.ld"

new_repo 0x0131
edit "$repo" firmware/common/sections.ld
commit "$repo" "change the sections the linker script includes"
run "the included sections file is a firmware change" "$repo" main
want_status 1
want_stderr "has to be above the one main carries"

# The bootloader's own linker script is not: firmware/common holds four and
# this image uses two of them.
new_repo 0x0131
edit "$repo" firmware/common/LPC17xx_Linker_Script_bootloader.ld
commit "$repo" "change the bootloader's memory layout"
run "another image's linker script is not the interface" "$repo" main
want_status 0
want_stdout "no firmware interface file changed"

# The shared USB code under lpcusb is linked into the image, two directories
# down, so the watch has to reach it.
new_repo 0x0131
edit "$repo" firmware/common/lpcusb/target/usbhw_lpc.c
commit "$repo" "change the shared USB code"
run "the lpcusb sources are part of the image" "$repo" main
want_status 1
want_stderr "has to be above the one main carries"
want_stderr "lpcusb/target/usbhw_lpc.c"

# A comment appended to a firmware source is not a firmware change, in either
# direction. This is the bypass one directory below the README: a comment in a
# compiled file used to count as the change that justified its own bump.
new_repo 0x0131
edit_comment "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
commit "$repo" "comment the firmware"
run "a comment in a firmware source is not a change" "$repo" main
want_status 0
want_stdout "no firmware interface file changed"

new_repo 0x0131
edit_comment "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
set_version "$repo" 0x0132
commit "$repo" "comment the firmware and bump the version"
run "a comment in a firmware source cannot buy a version" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"

# The same for a block comment, including the shape the normalisation used to
# read as code: an interior line beginning with neither / nor *.
new_repo 0x0131
edit_block_comment "$repo" firmware/common/usb.c
edit_prose_comment "$repo" firmware/bluetooth_rxtx/bluetooth_rxtx.c
set_version "$repo" 0x0132
commit "$repo" "comment two firmware files and bump the version"
run "block and prose comments cannot buy a version" "$repo" main
want_status 1
want_stderr "changes no firmware interface file"

new_repo 0x0131
edit_prose_comment "$repo" "$header_path"
set_version "$repo" 0x0132
commit "$repo" "add prose to the interface header and bump the version"
run "prose in the interface header cannot buy a version" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"

# A request added inside a comment is not a request, so it asks for nothing.
new_repo 0x0131
printf '/*\n#define UBERTOOTH_SOMETHING_NEW 99\n*/\n' >> "$repo/$header_path"
commit "$repo" "add a request inside a comment"
run "a request wrapped in a comment is not a request" "$repo" main
want_status 0
want_stdout "no firmware interface file changed"

# Commenting an existing request out is a change to the interface, though: the
# filter drops the lines, so the header it compares has lost them.
new_repo 0x0131
git -C "$repo" checkout -q main
printf '#define UBERTOOTH_SOMETHING 98\n' >> "$repo/$header_path"
commit "$repo" "a request main has"
git -C "$repo" checkout -q -B work
sed 's|^#define UBERTOOTH_SOMETHING 98|/*\n#define UBERTOOTH_SOMETHING 98\n*/|' \
    "$repo/$header_path" > "$repo/h.new"
mv "$repo/h.new" "$repo/$header_path"
commit "$repo" "comment that request out"
run "commenting a request out is an interface change" "$repo" main
want_status 1
want_stderr "has to be above the one main carries"

# firmware/Makefile picks which images to build, not how any of them is
# compiled, so it is not the interface.
new_repo 0x0131
edit "$repo" firmware/Makefile
commit "$repo" "change which images are built"
run "the top firmware Makefile is not the interface" "$repo" main
want_status 0
want_stdout "no firmware interface file changed"

# A release takes the next major version with the minor digits at 00, whether
# or not the cycle changed the firmware (docs/source/release_procedure.rst).
# Without this the release's own pull request would be refused by a job it
# could not satisfy.
new_repo 0x0131
edit "$repo" host/libubertooth/src/ubertooth.c
edit "$repo" CHANGELOG.md
edit "$repo" COMPATIBILITY.md
edit "$repo" TEST_PLAN.md
set_version "$repo" 0x0200
commit "$repo" "cut a release"
run "a release takes the next major version" "$repo" main
want_status 0
want_empty_stderr
want_stdout "the next major version"

# And that carve-out stays narrow, because a stray 0x0200 on an ordinary
# branch would refuse every board until it was reflashed. Two majors up is not
# a release, it is a typo.
new_repo 0x0131
edit "$repo" CHANGELOG.md
edit "$repo" COMPATIBILITY.md
edit "$repo" TEST_PLAN.md
set_version "$repo" 0x0300
commit "$repo" "skip a major version"
run "two majors up is not a release bump" "$repo" main
want_status 1
want_stderr "changes no firmware interface file"

# Nor is a minor bump dressed up as one.
new_repo 0x0131
edit "$repo" CHANGELOG.md
edit "$repo" COMPATIBILITY.md
edit "$repo" TEST_PLAN.md
set_version "$repo" 0x0201
commit "$repo" "a major version with a minor digit"
run "a major version with a minor digit is not a release bump" "$repo" main
want_status 1
want_stderr "changes no firmware interface file"

# Nor is the right value on its own: a release moves the three documents that
# record the version with it, and a slip of the finger does not.
new_repo 0x0131
edit "$repo" host/libubertooth/src/ubertooth.c
set_version "$repo" 0x0200
commit "$repo" "a host-only branch that happens to type the next major"
run "the next major without the release documents" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"
want_stderr "CHANGELOG.md, COMPATIBILITY.md and"

new_repo 0x0131
edit "$repo" CHANGELOG.md
edit "$repo" COMPATIBILITY.md
set_version "$repo" 0x0200
commit "$repo" "a release that forgot TEST_PLAN.md"
run "a release bump with one document missing" "$repo" main
want_status 1
want_stderr "changes no firmware interface file"

# And the release shape must not be a door around the collision the whole
# check is about. Main has already released 0x0200; a second release-shaped
# branch, cut before that landed or re-cut after it, claims the same value.
# Everything else about it is a release: one major up from its merge base, the
# minor digits at 00, the three documents moved. Only the comparison with main
# refuses it, which is why that comparison is "above main" and not "not below
# main". The push-to-main leg would not catch this one either: there the merge
# base is main, so an equal value leaves through the unchanged exit.
new_repo 0x0131
git -C "$repo" checkout -q main
edit "$repo" CHANGELOG.md
edit "$repo" COMPATIBILITY.md
edit "$repo" TEST_PLAN.md
set_version "$repo" 0x0200
commit "$repo" "main releases 2.00"
git -C "$repo" checkout -q work
edit "$repo" CHANGELOG.md
edit "$repo" COMPATIBILITY.md
edit "$repo" TEST_PLAN.md
set_version "$repo" 0x0200
commit "$repo" "a second release claiming the same value"
run "a release cannot claim the version main released" "$repo" main
want_status 1
want_empty_stdout
want_stderr "changes no firmware interface file"
want_stderr "on main:       2.00 (0x0200)"

# A source in a subdirectory of the image's own directory is still one of the
# image's sources. The directory has none today, but the other firmware image
# in this tree keeps its sources that way, and matching only direct children
# would mean the Makefile edit that introduced a subdirectory was the last
# thing the check ever saw there.
new_repo 0x0131
git -C "$repo" checkout -q main
mkdir -p "$repo/firmware/bluetooth_rxtx/src"
printf 'SEEDED = nested\n' > "$repo/firmware/bluetooth_rxtx/src/helper.c"
commit "$repo" "move a source into a subdirectory"
git -C "$repo" checkout -q -B work
edit "$repo" firmware/bluetooth_rxtx/src/helper.c
commit "$repo" "change the nested source, keep the version"
run "a source in a subdirectory of the image counts" "$repo" main
want_status 1
want_stderr "has to be above the one main carries"
want_stderr "firmware/bluetooth_rxtx/src/helper.c"

# The answer must not depend on how this checkout happens to be configured to
# display a diff. color.ui = always colorises even when nothing is a terminal
# and diff.external replaces the format altogether, either of which would make
# a line-matching filter see no change and report the permissive answer.
for setting in "color.ui always" "diff.external /bin/echo" "diff.noprefix true"; do
    new_repo 0x0131
    # shellcheck disable=SC2086 # two words, a git config key and its value
    git -C "$repo" config $setting
    printf '#define UBERTOOTH_SOMETHING_NEW 99\n' >> "$repo/$header_path"
    commit "$repo" "add a request to the interface"
    run "an interface change is found with $setting set" "$repo" main
    want_status 1
    want_stderr "has to be above the one main carries"
done

# --list-watched prints what the check watches, so a reader does not have to
# trust that the pathspecs still match anything. It lists the compiled sources
# and the interface header, and not the documentation beside them.
new_repo 0x0131
run "the watched files can be listed" "$repo" --list-watched
want_status 0
want_empty_stderr
want_stdout firmware/bluetooth_rxtx/bluetooth_rxtx.c
want_stdout firmware/bluetooth_rxtx/Makefile
want_stdout firmware/common/usb.c
want_stdout "$header_path"
want_no_stdout firmware/bluetooth_rxtx/README
want_no_stdout firmware/bluetooth_rxtx/flash
want_no_stdout firmware/common.mk
want_no_stdout firmware/Makefile
want_no_stdout firmware/btbr/btbr.c
want_no_stdout firmware/tests/test_firmware.c

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

# A lone -- ends the options, so a ref whose name begins with a dash can still
# be named (.kiro/steering/cli-output.md).
run "a lone -- ends the options" "$repo" -- main
want_status 0
want_stdout "no firmware interface file changed"

# A combination that cannot mean one thing is refused rather than half done.
run "a ref with --list-watched" "$repo" --list-watched main
want_status 1
want_empty_stdout
want_stderr "--list-watched reads no commit"

run "--list-watched after a ref" "$repo" main --list-watched
want_status 1
want_empty_stdout
want_stderr "--list-watched reads no commit"

run "--list-watched twice" "$repo" --list-watched --list-watched
want_status 1
want_empty_stdout
want_stderr "error: --list-watched twice"

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
