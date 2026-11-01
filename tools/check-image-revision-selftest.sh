#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Check that tools/check-image-revision.sh accepts and refuses the images it
# is meant to.
#
#     tools/check-image-revision-selftest.sh
#
# The check it exercises is the only thing in CI that reads a built image the
# way `ubertooth-dfu` reads it, so it has to be right in both directions: an
# image whose firmware info block the tool cannot read must go red, and an
# ordinary image must go green. Each case below writes a throwaway image --
# filler bytes either side of a 64-byte block at the offset, as a real one has
# code either side of it -- and asserts the status and the message.
#
# No ARM toolchain and no board: the images here are written byte by byte, so
# this runs anywhere a C compiler does. What it cannot show is that the real
# firmware still carries a readable block; the image-revision CI job does that,
# on the images the firmware job built.
#
# Prints each case with its exit status and both output streams, so a failing
# run says what happened without a rerun. Exits 0 when every case holds, 1
# otherwise.

set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 1
check="$here/check-image-revision.sh"

if [ ! -x "$check" ]; then
    printf '%s\n' "error: $check is missing or not executable" >&2
    exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

failures=0
case_name=

fail() {
    printf '%s\n' "FAIL: $case_name: $*" >&2
    failures=$((failures + 1))
}

# The block's layout, from host/libubertooth/src/ubertooth_interface.h. Spelt
# out here rather than read from the header: a self-test that took the layout
# from the same place as the code under test could not tell a wrong offset
# from a right one.
INFO_OFFSET=256
INFO_MAGIC=UBTFINFO
IMAGE_LEN=16
REVISION_LEN=32

# Write $1 NUL bytes.
nuls() {
    i=0
    while [ "$i" -lt "$1" ]; do
        printf '\000'
        i=$((i + 1))
    done
}

# Write the number in $1 as two little-endian bytes, which is how the block
# holds its 16-bit fields. The escapes are built as text and handed to %b, so
# the format string itself stays a literal.
le16() {
    printf '%b' "$(printf '\\%03o\\%03o' "$(($1 % 256))" "$(($1 / 256))")"
}

# Write the text in $1 into a field of $2 bytes, padded with NULs. A text
# exactly as long as the field is written with no terminator, which is what a
# full-length revision looks like in a real image.
field() {
    text=$1
    size=$2

    if [ "${#text}" -gt "$size" ]; then
        printf '%s\n' "error: this self-test tried to write '$text' into a" \
            "$size-byte field, which would run into the next one" >&2
        exit 1
    fi
    printf '%s' "$text"
    nuls $((size - ${#text}))
}

# Write an image file. Arguments: the path, the image name for the block, the
# revision, and optionally a format version other than 1 and a magic other
# than the real one.
#
# The filler is 0x5a rather than NUL so that a block read from the wrong place
# is unprintable, as it would be in a real image: NUL filler would read as an
# empty string and hide a wrong offset.
write_image() {
    path=$1
    image_name=$2
    revision=$3
    format=${4:-1}
    magic=${5:-$INFO_MAGIC}

    mkdir -p "$(dirname "$path")" || exit 1
    {
        i=0
        while [ "$i" -lt "$INFO_OFFSET" ]; do
            printf '\132'
            i=$((i + 1))
        done
        printf '%s' "$magic"
        # The format version and the API version, then four reserved bytes.
        le16 "$format"
        le16 $((0x0133))
        nuls 4
        field "$image_name" "$IMAGE_LEN"
        field "$revision" "$REVISION_LEN"
        printf '\132\132\132\132'
    } > "$path"
}

# Run the check, keeping its status and both streams. No set -e: most cases
# are about a check that is meant to fail.
run() {
    case_name=$1
    shift
    printf '\n%s\n' "case: $case_name"
    printf '%s\n' "  check-image-revision.sh $*"
    "$check" "$@" > "$work/out" 2> "$work/err"
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

# Every line of standard output matches the extended regular expression given,
# so nothing but the result reached a caller reading it.
want_stdout_only() {
    if grep -qvE -- "$1" "$work/out"; then
        fail "standard output has a line that is not a result:"
        grep -vE -- "$1" "$work/out" | sed 's/^/    /' >&2
    fi
}

# -------------------------------------------------------------------------
# What a good build carries
# -------------------------------------------------------------------------

image="$work/bluetooth_rxtx.bin"
write_image "$image" bluetooth_rxtx git-deadbeef
run "an image carrying a readable info block" "$image"
want_status 0
want_empty_stderr
want_stdout "git-deadbeef"

# A .dfu is the .bin plus a 16-byte suffix, so the same payload has to pass
# with one appended: that is what makes checking the .bin enough. The bytes
# here are filler on purpose. Whether a suffix is a valid one is
# dfu_image_check()'s question, which the tool asks before it reads the block
# and this check does not ask at all.
cp "$image" "$work/bluetooth_rxtx.dfu"
printf '\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017\020' \
    >> "$work/bluetooth_rxtx.dfu"
run "the same payload as a .dfu" "$work/bluetooth_rxtx.dfu"
want_status 0
want_empty_stderr
want_stdout "git-deadbeef"

# The revision a release build carries is not a git hash.
write_image "$image" bluetooth_rxtx 2026-02-R1
run "a release revision" "$image"
want_status 0
want_stdout "2026-02-R1"

# A revision filling the field with no room for a terminator: the host reads
# it as a string of exactly that length, so it is good.
full_revision=git-0123456789abcdef0123456789ab   # 32 bytes, the whole field
write_image "$image" bluetooth_rxtx "$full_revision"
run "a revision filling the field" "$image"
want_status 0
want_stdout "$full_revision"

# -------------------------------------------------------------------------
# What the tool refuses, and so this must refuse
# -------------------------------------------------------------------------

# No block at all, which is what a build that stopped linking fw_info.c
# produces, and what the btbr image shipped with.
write_image "$image" bluetooth_rxtx git-deadbeef 1 "........"
run "an image with no info block" "$image"
want_status 1
want_empty_stdout
want_stderr "could not read the firmware info block"
want_stderr "no firmware info block"

# A block from a newer format than this host knows: the tool refuses rather
# than reading fields it cannot place.
write_image "$image" bluetooth_rxtx git-deadbeef 2
run "a block in a format the host does not know" "$image"
want_status 1
want_empty_stdout
want_stderr "could not read the firmware info block"

# An empty revision passes the magic check a shell can do and fails the
# tool's: this is the case firmware/common.mk's dd cannot see.
write_image "$image" bluetooth_rxtx ""
run "an empty revision" "$image"
want_status 1
want_empty_stdout
want_stderr "could not read the firmware info block"

# An unprintable byte in the revision, likewise.
write_image "$image" bluetooth_rxtx "git-dead"
printf '\132' | dd of="$image" bs=1 seek=$((INFO_OFFSET + 32)) conv=notrunc \
    2> /dev/null
printf '\001' | dd of="$image" bs=1 seek=$((INFO_OFFSET + 33)) conv=notrunc \
    2> /dev/null
run "a revision with an unprintable byte" "$image"
want_status 1
want_empty_stdout
want_stderr "could not read the firmware info block"

# An empty image name: the block has to say which image it belongs to.
write_image "$image" "" git-deadbeef
run "an empty image name" "$image"
want_status 1
want_empty_stdout
want_stderr "could not read the firmware info block"

# A file too short to hold a block at the offset.
printf '%s' "not an image" > "$work/short.bin"
run "a file shorter than the offset" "$work/short.bin"
want_status 1
want_empty_stdout
want_stderr "could not read the firmware info block"

# A readable block that names another image. The name is baked in per image,
# so this is what a build sharing one object between images would produce,
# and it would flash and verify against the wrong build without complaint.
write_image "$work/btbr.bin" bluetooth_rxtx git-deadbeef
run "a block naming another image" "$work/btbr.bin"
want_status 1
want_empty_stdout
want_stderr "the info block in this image says it is"
want_stderr '"bluetooth_rxtx", not "btbr"'

# The same block under a name that is not a build's. `ubertooth-dfu` has no
# rule about the filename, and the release procedure renames the image it
# publishes, so a renamed file is checked for its revision and nothing more.
cp "$work/btbr.bin" "$work/ubertooth-one-bin-firmware.dfu"
run "a renamed image" "$work/ubertooth-one-bin-firmware.dfu"
want_status 0
want_empty_stderr
want_stdout "git-deadbeef"

# -------------------------------------------------------------------------
# Images that are not there, and the command line
# -------------------------------------------------------------------------

run "an image that does not exist" "$work/absent.bin"
want_status 1
want_empty_stdout
want_stderr "no such image to check"

run "a directory given as an image" "$work"
want_status 1
want_empty_stdout
want_stderr "no such image to check"

# Run with nothing at all: the error line first and then the usage text, both
# on standard error (.kiro/steering/cli-output.md).
run "no arguments"
want_status 1
want_empty_stdout
want_stderr "error: an image, --from <dir> or --check-list is needed"
want_stderr "usage:"

run "-h" -h
want_status 0
want_empty_stderr
want_stdout "usage:"

run "an unknown option" --nonsense
want_status 1
want_empty_stdout
want_stderr "unknown option"

# -------------------------------------------------------------------------
# --from: the mode CI runs, over a directory of built images
# -------------------------------------------------------------------------

built="$work/built"
write_image "$built/firmware-linux/bluetooth_rxtx/bluetooth_rxtx.bin" \
    bluetooth_rxtx git-cafe1234
write_image "$built/firmware-linux/btbr/btbr.bin" btbr git-cafe1234

# Standard output here carries nothing but `path: revision api` lines, so a
# caller can read it without filtering; which images were looked for is
# status, and goes to standard error.
run "a directory of good images" --from "$built"
want_status 0
want_stdout "bluetooth_rxtx.bin: git-cafe1234"
want_stdout "btbr.bin: git-cafe1234"
want_stderr "shipped images: bluetooth_rxtx btbr"
want_stdout_only ': git-cafe1234 1\.33$'

# One shipped image missing is a failure, not a pass over the rest: an image
# that stopped being built is exactly what nobody would notice.
mv "$built/firmware-linux/btbr/btbr.bin" "$work/kept-btbr.bin"
run "a shipped image that was not built" --from "$built"
want_status 1
want_stderr "no btbr image was built under"
want_stdout_only ': git-cafe1234 1\.33$'  # the one good image, nothing else

# One bad image among good ones.
write_image "$built/firmware-linux/btbr/btbr.bin" btbr ""
run "one bad image among good ones" --from "$built"
want_status 1
want_stdout "bluetooth_rxtx.bin: git-cafe1234"
want_stderr "could not read the firmware info block"

# Every runner's artifact is checked, not just the first: a toolchain that
# dropped the block on one platform only has to be caught.
cp "$work/kept-btbr.bin" "$built/firmware-linux/btbr/btbr.bin"
write_image "$built/firmware-mac/bluetooth_rxtx/bluetooth_rxtx.bin" \
    bluetooth_rxtx git-cafe1234
write_image "$built/firmware-mac/btbr/btbr.bin" btbr git-cafe1234 1 "........"
run "a second runner's image is bad" --from "$built"
want_status 1
want_stdout "firmware-linux/btbr/btbr.bin: git-cafe1234"
want_stderr "firmware-mac/btbr/btbr.bin"

run "--from with no directory" --from
want_status 1
want_stderr "takes one directory"

run "--from a path that is not a directory" --from "$work/absent"
want_status 1
want_stderr "is not a directory"

# --from checks the list first and then the images. A tree whose list is wrong
# has to fail even when every image it finds reads fine: the images that were
# looked for came from that list, so a bad list means something was not looked
# for at all. This runs a copy of the check from a scratch tree with an
# unclassified firmware directory, since the real tree's own list is good.
#
# The copy keeps the layout the check expects: tools/ beside firmware/, and
# host/ for the reader it compiles, which the real tree can lend.
scratch="$work/scratch"
mkdir -p "$scratch/tools"
cp "$check" "$scratch/tools/"
ln -s "$here/../host" "$scratch/host"
for directory in bluetooth_rxtx btbr assembly_test blinky cc2400_test \
                 clock_test usb_test bootloader common tc13badge tests \
                 newimage; do
    mkdir -p "$scratch/firmware/$directory"
done
write_image "$scratch/art/firmware-linux/bluetooth_rxtx/bluetooth_rxtx.bin" \
    bluetooth_rxtx git-cafe1234
write_image "$scratch/art/firmware-linux/btbr/btbr.bin" btbr git-cafe1234

case_name="--from over a tree whose list is wrong"
printf '\n%s\n' "case: $case_name"
printf '%s\n' "  (scratch tree) check-image-revision.sh --from art"
"$scratch/tools/check-image-revision.sh" --from "$scratch/art" \
    > "$work/out" 2> "$work/err"
status=$?
printf '%s\n' "  exit status $status"
sed 's/^/  stderr: /' "$work/err"
sed 's/^/  stdout: /' "$work/out"
want_status 1
want_stderr "firmware/newimage is not in any of the lists"
# The images themselves are fine, so this is the one case where the only
# reason to fail comes from the list check.
want_stdout "bluetooth_rxtx.bin: git-cafe1234"

# --list-shipped from the same tree hands out no list at all: a caller acting
# on one would be checking a set of images nobody has decided about.
case_name="--list-shipped over a tree whose list is wrong"
printf '\n%s\n' "case: $case_name"
printf '%s\n' "  (scratch tree) check-image-revision.sh --list-shipped"
"$scratch/tools/check-image-revision.sh" --list-shipped \
    > "$work/out" 2> "$work/err"
status=$?
printf '%s\n' "  exit status $status"
sed 's/^/  stderr: /' "$work/err"
sed 's/^/  stdout: /' "$work/out"
want_status 1
want_empty_stdout
want_stderr "firmware/newimage is not in any of the lists"

# -------------------------------------------------------------------------
# --check-list: the lists against the firmware tree
# -------------------------------------------------------------------------

# The tree this script sits in. This is the live guard: a firmware directory
# added to the tree has to be classified, or nothing decided whether the image
# it builds is one users flash.
run "this tree's own firmware directories" --check-list
want_status 0
want_empty_stderr
want_stdout "bluetooth_rxtx btbr"

# A tree holding exactly the directories the lists name.
make_tree() {
    tree="$work/$1"
    shift
    for image in "$@"; do
        mkdir -p "$tree/firmware/$image"
    done
    printf '%s\n' "$tree"
}

listed="bluetooth_rxtx btbr assembly_test blinky cc2400_test clock_test"
listed="$listed usb_test bootloader common tc13badge tests"

# shellcheck disable=SC2086 # the list is the argument list, one per directory
make_tree classified $listed > /dev/null
run "a tree whose directories are all classified" --check-list \
    "$work/classified"
want_status 0
want_empty_stderr

# A new firmware directory nobody has classified: it may build an image users
# flash, and until someone says so nothing checks it.
# shellcheck disable=SC2086 # as above
make_tree extra $listed newimage > /dev/null
run "a tree with a directory no list names" --check-list "$work/extra"
want_status 1
want_stderr "firmware/newimage is not in any of the lists"

# A directory the lists name that is not there: the list has gone stale, and
# --from would look for an image nothing builds.
make_tree moved bluetooth_rxtx > /dev/null
run "a listed directory that is not there" --check-list "$work/moved"
want_status 1
want_stderr "lists btbr, but there is no"

# A firmware directory with nothing in it: either the tree has moved or the
# scan has stopped working. Both are failures.
mkdir -p "$work/bare/firmware"
run "a firmware directory with no subdirectories" --check-list "$work/bare"
want_status 1
want_stderr "has no subdirectories"

run "--check-list with a tree that has no firmware" --check-list "$work"
want_status 1
want_stderr "has no firmware directory"

# -------------------------------------------------------------------------
# --list-shipped: the list, for the other check over built images
# -------------------------------------------------------------------------

# tools/check-dfu-package.sh reads the list from here rather than keeping a
# second copy, so standard output carries the names and nothing else, one a
# line.
run "the list of shipped images" --list-shipped
want_status 0
want_stdout "bluetooth_rxtx"
want_stdout "btbr"
want_stdout_only '^[a-z0-9_]+$'
want_stderr "shipped images: bluetooth_rxtx btbr"

run "--list-shipped with something else" --list-shipped "$work"
want_status 1
want_empty_stdout
want_stderr "takes nothing else"

printf '\n'
if [ "$failures" -eq 0 ]; then
    printf '%s\n' "All cases hold: $(basename "$check") accepts an image whose"
    printf '%s\n' "info block ubertooth-dfu can read and refuses one it cannot."
    exit 0
fi
printf '%s\n' "$failures case(s) failed" >&2
exit 1
