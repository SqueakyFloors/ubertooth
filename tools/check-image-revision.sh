#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Check that a built firmware image carries the revision `ubertooth-dfu -r`
# reads out of it.
#
#     tools/check-image-revision.sh <image>...
#     tools/check-image-revision.sh --from <dir>
#     tools/check-image-revision.sh --check-list [<tree>]
#     tools/check-image-revision.sh --list-shipped
#
# `-r` checks, after the reset, that the board came back running the revision
# that was just written, and it learns which revision to expect from the FILE:
# it reads the firmware info block every image from this tree carries at
# UBERTOOTH_FW_INFO_OFFSET. An image whose block it cannot read is refused
# before anything is erased, so the documented way to flash it stops working.
# That happened to the btbr image, and it was found at the bench, because
# nothing in CI read a built image.
#
# The read is not reimplemented here. This compiles the host's own
# dfu_read_image_info() out of host/ubertooth-tools/src and runs it on each
# image, so the check cannot read a revision the tool would not read, or miss
# one it would. It needs a C compiler and nothing else: no libusb, no board,
# no network.
#
# firmware/common.mk already fails a build whose .bin has no UBTFINFO magic at
# the offset, which it compares with dd. That is eight bytes. Everything else
# the tool's own parse insists on -- the format version it knows, an image name
# and a revision that are non-empty and printable -- is only checked by running
# that parse, which is what this does. It also checks the image name in the
# block against the file it came out of: the block bakes in FW_IMAGE_NAME, and
# an image claiming to be another one would flash and verify without complaint.
#
# The images checked are the ones users flash with `ubertooth-dfu`, named in
# SHIPPED_IMAGES below. --check-list is what keeps that list honest: it refuses
# a tree with a firmware directory that none of the lists below classifies, so
# a new image has to be called shipped or bench-only before CI goes green.
#
# That list is the only one in the tree, and --list-shipped is how the other
# check over built images (tools/check-dfu-package.sh) reads it rather than
# keeping a second copy that could come to disagree. It checks the list first,
# so a caller cannot be handed a list from a tree the classification refuses.
#
# The revision is all this checks. `ubertooth-dfu` validates the DFU suffix
# first (dfu_image_check(): signature, CRC, vendor and product), and a .bin
# carries no suffix at all, so an image passing here is not yet a file the tool
# would accept as given; it is an image whose revision the tool can read, which
# is the property that regressed. The suffix on the packaged file is
# tools/check-dfu-package.sh's question, and CI asks both.
#
# An image may be a .bin or a .dfu. A .dfu is the .bin with a 16-byte suffix
# appended, and the block is read from a fixed offset near the start, so
# appending to a .bin cannot change what is read: an image accepted as a .bin
# is accepted as the .dfu built from it.
#
# Exits 0 when every image named carries a readable revision, 1 otherwise.

set -u

self=$(basename "$0")

# The firmware images users flash with `ubertooth-dfu`, and whose revision -r
# verifies. Each name is both the directory under firmware/ and the stem of the
# built file, so bluetooth_rxtx builds bluetooth_rxtx.bin.
#
# bl_update is one of them: a user writes it to install a bootloader
# (ubertooth-dfu(1), UPDATING THE BOOTLOADER). It is the one that -r does not
# verify afterwards, because it has no USB of its own and returns the board to
# DFU rather than coming back as an application; its revision still has to be
# the tree's, which is what this check is for.
SHIPPED_IMAGES="bluetooth_rxtx btbr bl_update"

# Images built at the bench to test a board, never flashed to run it. They
# carry the info block too (firmware/common.mk links fw_info.c into every
# image), so these would pass; they are listed to say that leaving them out is
# a decision, not an oversight.
BENCH_ONLY_IMAGES="assembly_test blinky cc2400_test clock_test usb_test"

# What a file's info block may call itself, given the file's stem, as a
# space-separated list; empty when the stem says nothing about the name, and
# the block's name is then not checked.
#
# Normally the two are the same: each image is named after the directory it is
# built in. The receive-only build of bluetooth_rxtx is the exception. It is
# that target with its transmit half left out and names itself bluetooth_rx
# (IMAGE_NAME under `ifdef DISABLE_TX` in firmware/bluetooth_rxtx/Makefile), so
# that a board running it can be told from one running the full image, while
# the file it builds is still bluetooth_rxtx.* and the release ships that file
# as bluetooth_rx_only.*. So both stems may carry that name.
#
# For the bluetooth_rxtx stem that is a loosening, not a tightening: a full
# build whose block said bluetooth_rx would pass here. One target builds both
# images under the same file name, so the stem cannot tell them apart; what
# does is test_fw_info, which pins each build's name to its own make flags.
image_names_for_stem() {
    case "$1" in
    bluetooth_rxtx)
        echo "bluetooth_rxtx bluetooth_rx"
        ;;
    bluetooth_rx_only)
        echo "bluetooth_rx"
        ;;
    *)
        case " $SHIPPED_IMAGES $BENCH_ONLY_IMAGES " in
        *" $1 "*) echo "$1" ;;
        esac
        ;;
    esac
}

# Directories under firmware/ that build no Ubertooth image: shared sources,
# the host tests, the bootloader, which carries no info block and goes on over
# ISP or JTAG rather than through `ubertooth-dfu`, and tc13badge, which is code
# for the Renesas microcontroller on the ToorCon 13 badge -- a different
# compiler, its own Makefile, and no block to read.
NOT_AN_IMAGE="bootloader common tc13badge tests"

here=$(cd "$(dirname "$0")" && pwd) || exit 1
top=$(cd "$here/.." && pwd) || exit 1

work=
trap 'rm -rf "$work"' EXIT HUP INT TERM

usage() {
    cat <<EOF
usage: $self <image>...
       $self --from <dir>
       $self --check-list [<tree>]
       $self --list-shipped

Check that each shipped firmware image carries the revision
ubertooth-dfu -r reads out of it.

  <image>...     check the named image files (.bin or .dfu)
  --from <dir>   find every shipped image under <dir> and check it
  --check-list [<tree>]
                 check that the lists of images classify every directory
                 under <tree>/firmware, or this tree's own
  --list-shipped print the images users flash, one a line, after checking
                 the lists against this tree
  -h, --help     print this text
EOF
}

# One error line, then continuation lines lined up under it.
fail() {
    printf 'error: %s\n' "$1" >&2
    shift
    for line in "$@"; do
        printf '       %s\n' "$line" >&2
    done
}

# Build the host's reader into a program that prints the image name, revision
# and API version it reads out of the image named on its command line. Sets
# $reader and $work.
reader=
build_reader() {
    src="$top/host/ubertooth-tools/src"
    header="$top/host/libubertooth/src"
    for needed in "$src/dfu_recovery.c" "$src/dfu_recovery.h" \
                  "$src/dfu_transition.c" "$header/ubertooth_interface.h"; do
        if [ ! -f "$needed" ]; then
            fail "$needed is not there, so the check cannot read an image" \
                 "the way ubertooth-dfu reads it"
            return 1
        fi
    done

    if [ -z "$work" ]; then
        work=$(mktemp -d) || return 1
    fi
    cat > "$work/read-info.c" <<'EOF'
/* The firmware info block `ubertooth-dfu` would read out of the image named on
 * the command line: the image name, the revision and the API version, on
 * standard output. On a block it cannot read, the tool's own reason for
 * refusing the file, on standard error. */
#include <stdio.h>

#include "dfu_recovery.h"
#include "ubertooth_interface.h"

int main(int argc, char **argv)
{
	struct ubertooth_fw_info info;
	FILE *image;
	int result;

	if (argc != 2) {
		fprintf(stderr, "usage: read-info <image>\n");
		return 2;
	}
	image = fopen(argv[1], "rb");
	if (image == NULL) {
		perror(argv[1]);
		return 2;
	}
	result = dfu_read_image_info(image, &info);
	fclose(image);
	if (result != UBERTOOTH_FW_INFO_OK) {
		fprintf(stderr, "%s\n", ubertooth_fw_info_result_string(result));
		return 1;
	}
	printf("%s %s %x.%02x\n", info.image, info.revision,
	       (info.api_version >> 8) & 0xFF, info.api_version & 0xFF);
	return 0;
}
EOF
    reader="$work/read-info"
    # dfu_transition.c comes along because dfu_recovery.c calls into it; the
    # host's own unit tests link the same pair.
    if ! output=$("${CC:-cc}" -o "$reader" "$work/read-info.c" \
                  "$src/dfu_recovery.c" "$src/dfu_transition.c" \
                  -I"$src" -I"$header" 2>&1); then
        fail "the host's image reader would not compile:"
        printf '%s\n' "$output" >&2
        return 1
    fi
    return 0
}

# Check one image file. Prints what it found either way.
check_image() {
    path=$1

    if [ ! -f "$path" ]; then
        fail "$path: there is no such image to check"
        return 1
    fi
    if ! found=$("$reader" "$path" 2> "$work/why"); then
        fail "$path: ubertooth-dfu could not read the firmware info block" \
             "out of this image, so it would refuse to flash it at all," \
             "and -r would have no revision to verify. The tool's reason:" \
             "$(cat "$work/why")."
        return 1
    fi

    # The block names the image it was built for. A file whose block claims
    # another image would flash and verify without complaint, and the name is
    # baked in per image (FW_IMAGE_NAME in firmware/common.mk), so a build
    # that shared one object between images would get this wrong.
    #
    # Only for a file whose stem says what the name should be, which is every
    # file --from finds, every file straight out of firmware/, and the one name
    # the release renames a file to. `ubertooth-dfu` imposes no such rule, so a
    # published asset renamed to anything else must not fail for that.
    claimed=${found%% *}
    stem=$(basename "$path")
    stem=${stem%.*}
    allowed=$(image_names_for_stem "$stem")
    if [ -n "$allowed" ]; then
        matched=0
        for name in $allowed; do
            if [ "$claimed" = "$name" ]; then
                matched=1
                break
            fi
        done
        if [ "$matched" -eq 0 ]; then
            # The names on one line, so a reader sees what was found against
            # what a file of this name may carry without reading a wrap.
            quoted=""
            for name in $allowed; do
                if [ -z "$quoted" ]; then
                    quoted="\"$name\""
                else
                    quoted="$quoted or \"$name\""
                fi
            done
            fail "$path: the info block in this image says it is" \
                 "\"$claimed\", not $quoted." \
                 "ubertooth-dfu would report the wrong image and -r would" \
                 "verify against the wrong build."
            return 1
        fi
    fi
    printf '%s: %s\n' "$path" "${found#* }"
    return 0
}

# Refuse a tree with a firmware directory that none of the lists classifies:
# an image nobody has called shipped or bench-only is an image nothing checks.
#
# Under --check-list the list it ends up with is what the caller asked for, so
# it goes to standard output. Run as --from's first step it is only status, and
# standard output there carries nothing but `path: revision` lines.
# This shell has no local variables, so each of the functions below keeps its
# own name for what went wrong. One shared `status` would mean whichever ran
# last decided the exit code, and a failure found earlier would be printed and
# then thrown away.
list_is_status=no
check_list() {
    tree=${1:-$top}
    known=" $SHIPPED_IMAGES $BENCH_ONLY_IMAGES $NOT_AN_IMAGE "
    list_status=0

    if [ ! -d "$tree/firmware" ]; then
        fail "$tree has no firmware directory to read the list from"
        return 1
    fi
    present=$(cd "$tree/firmware" && for entry in */; do
                  [ -d "$entry" ] && printf '%s\n' "${entry%/}"
              done | sort)
    if [ -z "$present" ]; then
        fail "$tree/firmware has no subdirectories, so either the firmware" \
             "tree has moved or this scan has stopped working"
        return 1
    fi
    for image in $present; do
        case $known in
        *" $image "*) ;;
        *)
            fail "firmware/$image is not in any of the lists in $self, so" \
                 "nothing decided whether users flash that image. Add it to" \
                 "SHIPPED_IMAGES if they do, to BENCH_ONLY_IMAGES if it is" \
                 "only built at the bench, or to NOT_AN_IMAGE if it builds" \
                 "no image of its own."
            list_status=1
            ;;
        esac
    done
    for image in $SHIPPED_IMAGES $BENCH_ONLY_IMAGES $NOT_AN_IMAGE; do
        if [ ! -d "$tree/firmware/$image" ]; then
            fail "$self lists $image, but there is no firmware/$image" \
                 "directory under $tree"
            list_status=1
        fi
    done
    if [ "$list_status" -eq 0 ]; then
        if [ "$list_is_status" = yes ]; then
            printf 'shipped images: %s\n' "$SHIPPED_IMAGES" >&2
        else
            printf 'shipped images: %s\n' "$SHIPPED_IMAGES"
        fi
    fi
    return "$list_status"
}

# Print every shipped image found under a directory, one a line. The layout
# below that directory is not fixed: CI downloads one artifact per runner, and
# each holds the images in their own subdirectories.
find_images() {
    directory=$1
    find_status=0

    for image in $SHIPPED_IMAGES; do
        found=$(find "$directory" -type f -name "$image.bin" -o \
                -type f -name "$image.dfu" | sort)
        if [ -z "$found" ]; then
            fail "no $image image was built under $directory, so the one" \
                 "check that reads what ubertooth-dfu reads has nothing" \
                 "to read"
            find_status=1
            continue
        fi
        printf '%s\n' "$found"
    done
    return "$find_status"
}

case ${1:-} in
-h|--help)
    usage
    exit 0
    ;;
'')
    # Run with nothing at all: the error line and then the usage text, both on
    # standard error, as .kiro/steering/cli-output.md asks.
    fail "an image, --from <dir> or --check-list is needed"
    usage >&2
    exit 1
    ;;
--check-list)
    if [ $# -gt 2 ]; then
        fail "--check-list takes at most one tree"
        exit 1
    fi
    check_list "${2:-}"
    exit $?
    ;;
--list-shipped)
    if [ $# -gt 1 ]; then
        fail "--list-shipped takes nothing else"
        exit 1
    fi
    # The names are what the caller asked for, so they go to standard output,
    # one a line; the classification check's own line is status and goes to
    # standard error. Nothing is printed at all when that check fails: a list
    # from a tree holding an image nobody has classified is not a list to act
    # on.
    list_is_status=yes
    check_list || exit 1
    for image in $SHIPPED_IMAGES; do
        printf '%s\n' "$image"
    done
    exit 0
    ;;
--from)
    if [ $# -ne 2 ]; then
        fail "--from takes one directory"
        exit 1
    fi
    if [ ! -d "$2" ]; then
        fail "$2 is not a directory"
        exit 1
    fi
    ;;
-*)
    fail "unknown option: $1"
    exit 1
    ;;
esac

build_reader || exit 1

status=0
if [ "$1" = "--from" ]; then
    # The list the images are looked up by has to be the right list first: an
    # image missing from it would be looked for nowhere and missed quietly.
    list_is_status=yes
    check_list || status=1
    find_images "$2" > "$work/images" || status=1
    while IFS= read -r image; do
        [ -n "$image" ] || continue
        check_image "$image" || status=1
    done < "$work/images"
else
    for image in "$@"; do
        check_image "$image" || status=1
    done
fi
exit "$status"
