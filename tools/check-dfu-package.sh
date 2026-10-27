#!/bin/sh
# Check that the .dfu built from a firmware image is a file ubertooth-dfu
# would accept.
#
#     tools/check-dfu-package.sh [--dfu-tool <tool>] <image.bin>...
#     tools/check-dfu-package.sh [--dfu-tool <tool>] --from <dir>
#
# The .dfu is the file every install guide tells users to flash, and nothing
# used to build one outside a developer's own tree. The firmware build ends in
# a packaging step that shells out to `ubertooth-dfu -s`
# (firmware/common.mk: `%.dfu: %.bin`), so a CI job with no host tool built
# cannot run it, and the images CI produced were .bin files nobody packaged.
#
# `ubertooth-dfu -d` validates the DFU suffix before it reads anything else
# out of the file: the suffix length, the DFU version, the UFD signature, a
# payload that is not empty, the CRC over the whole file, and the USB vendor
# and product the suffix names. A change that broke the packaging step or the
# suffix layout would leave every .bin check green and show up the first time
# someone flashed a board.
#
# What this cannot see on its own is a change to the two USB IDs in
# host/ubertooth-tools/src/dfu.h: the packaging and the check below both read
# them from there, so they would agree on a new value. The self-test is what
# catches that, because its stand-in packager spells the numbers out.
#
# So this packages each image with the real tool and runs the packaged file
# through the tool's own check. Neither half is reimplemented here: the
# packaging is whatever `ubertooth-dfu -s` writes, and the check compiles the
# host's own dfu_image_check() out of host/ubertooth-tools/src and asks it,
# with the vendor and product the tool asks for. It also compares the
# packaged payload with the image it came from, so a packaging step that
# rewrote the firmware and recomputed the CRC does not pass.
#
# The shipped images are the ones users flash with `ubertooth-dfu`. That list
# lives in tools/check-image-revision.sh, which this asks for it
# (--list-shipped), so the two checks cannot come to disagree about which
# images matter.
#
# tools/check-dfu-package-selftest.sh is what shows this holds in both
# directions: it runs the whole script against packaging tools that write a
# broken suffix on purpose. The CI job is the other half -- the real tool, on
# the images the firmware job built.
#
# Needs a C compiler and a built `ubertooth-dfu`. No board, and no network.
#
# Exits 0 when every image packages into a file the tool would accept, 1
# otherwise.

set -u

self=$(basename "$0")

here=$(cd "$(dirname "$0")" && pwd) || exit 1
top=$(cd "$here/.." && pwd) || exit 1
revision_check="$here/check-image-revision.sh"

# The 16-byte DFU suffix the packaging step appends (DFU_SUFFIX_LENGTH in
# host/ubertooth-tools/src/dfu.h). Spelt out rather than read from the header:
# the point below is that the packaged file is the image plus a suffix of
# exactly this length, and taking the figure from the code under test could
# not tell a right answer from a wrong one.
SUFFIX_LENGTH=16

# The tool packaging is done with, and the name the usage text names as the
# default: a separate constant, so `--dfu-tool X -h` still says what the
# default is rather than reporting X as one.
DEFAULT_DFU_TOOL=ubertooth-dfu
dfu_tool=$DEFAULT_DFU_TOOL
work=
trap 'rm -rf "$work"' EXIT HUP INT TERM

usage() {
    cat <<EOF
usage: $self [--dfu-tool <tool>] <image.bin>...
       $self [--dfu-tool <tool>] --from <dir>

Package each firmware image with ubertooth-dfu and check that the
packaged .dfu is a file ubertooth-dfu would accept.

  <image.bin>...   package and check the named images
  --from <dir>     find every shipped image under <dir> and check it
  --dfu-tool <t>   the ubertooth-dfu to package with (default: $DEFAULT_DFU_TOOL)
  -h, --help       print this text
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

# Build the host's own suffix check into a program that prints what the suffix
# of the image named on its command line holds. Sets $checker and $work.
checker=
build_checker() {
    src="$top/host/ubertooth-tools/src"
    header="$top/host/libubertooth/src"
    for needed in "$src/dfu_recovery.c" "$src/dfu_recovery.h" "$src/dfu.h" \
                  "$src/dfu_transition.c" "$header/ubertooth_interface.h"; do
        if [ ! -f "$needed" ]; then
            fail "$needed is not there, so the check cannot read a suffix" \
                 "the way ubertooth-dfu reads it"
            return 1
        fi
    done

    if [ -z "$work" ]; then
        work=$(mktemp -d) || return 1
    fi
    cat > "$work/check-suffix.c" <<'EOF'
/* What `ubertooth-dfu` makes of the DFU suffix on the image named on the
 * command line. On a file it would accept, the suffix fields on standard
 * output; on one it would refuse, the tool's own reason, on standard error. */
#include <stdio.h>

#include "dfu.h"
#include "dfu_recovery.h"

int main(int argc, char **argv)
{
	struct dfu_image_report report;
	char message[160];
	FILE *image;
	int result;

	if (argc != 2) {
		fprintf(stderr, "usage: check-suffix <image>\n");
		return 2;
	}
	image = fopen(argv[1], "rb");
	if (image == NULL) {
		perror(argv[1]);
		return 2;
	}
	result = dfu_image_check(image, U1_DFU_VENDORID, U1_DFU_PRODUCTID,
	                         &report);
	fclose(image);
	if (result != DFU_IMAGE_OK) {
		dfu_image_describe_failure(&report, message, sizeof(message));
		fprintf(stderr, "%s\n", message);
		return 1;
	}
	printf("suffix %u bytes, DFU %04x, vendor 0x%04x, product 0x%04x, "
	       "CRC 0x%08x\n", report.suffix_length, report.bcd_dfu,
	       report.id_vendor, report.id_product, report.crc_found);
	return 0;
}
EOF
    checker="$work/check-suffix"
    # dfu_transition.c comes along because dfu_recovery.c calls into it; the
    # host's own unit tests link the same pair.
    if ! output=$("${CC:-cc}" -o "$checker" "$work/check-suffix.c" \
                  "$src/dfu_recovery.c" "$src/dfu_transition.c" \
                  -I"$src" -I"$header" 2>&1); then
        fail "the host's DFU suffix check would not compile:"
        printf '%s\n' "$output" >&2
        return 1
    fi
    return 0
}

# The number of bytes in the file named, or nothing when it cannot be read.
file_size() {
    wc -c < "$1" 2>/dev/null | tr -d ' '
}

# Package one image and check the packaged file. Prints what it found either
# way.
#
# The image is copied into the work directory first, because packaging writes
# its output beside its input: a CI job must not have to write into the
# directory it downloaded artifacts to, and a developer running this by hand
# must not end up with a .dfu they did not ask for.
check_image() {
    path=$1

    if [ ! -f "$path" ]; then
        fail "$path: there is no such image to package"
        return 1
    fi
    stem=$(basename "$path")
    stem=${stem%.*}
    rm -rf "$work/package"
    mkdir -p "$work/package" || return 1
    copy="$work/package/$stem.bin"
    packaged="$work/package/$stem.dfu"
    if ! cp "$path" "$copy"; then
        fail "$path: could not be copied to package it"
        return 1
    fi

    # The same invocation firmware/common.mk's dfu rule makes.
    if ! "$dfu_tool" -s "$copy" > "$work/package-out" 2>&1; then
        fail "$path: the packaging step failed, so there is no file to" \
             "flash. \`$dfu_tool -s\` said:"
        sed 's/^/       /' "$work/package-out" >&2
        return 1
    fi
    if [ ! -f "$packaged" ]; then
        fail "$path: the packaging step wrote no $stem.dfu beside the" \
             "image, so the file users are told to flash is not the file" \
             "the build produces"
        return 1
    fi

    if ! found=$("$checker" "$packaged" 2> "$work/why"); then
        fail "$path: ubertooth-dfu would refuse this image's .dfu, so the" \
             "documented way to flash the image does not work." \
             "The tool's reason: $(cat "$work/why")."
        return 1
    fi

    # The payload has to come through packaging unchanged. The suffix CRC the
    # check above verified covers the whole file, so a step that rewrote the
    # firmware and then signed its own work passes that: this is what
    # notices.
    image_size=$(file_size "$copy")
    packaged_size=$(file_size "$packaged")
    if [ -z "$image_size" ] || [ -z "$packaged_size" ]; then
        fail "$path: could not measure the image or the packaged file"
        return 1
    fi
    if [ "$packaged_size" -ne $((image_size + SUFFIX_LENGTH)) ]; then
        fail "$path: packaging turned $image_size bytes into $packaged_size," \
             "not $((image_size + SUFFIX_LENGTH)): a .dfu is the image plus" \
             "a $SUFFIX_LENGTH-byte suffix, and nothing else"
        return 1
    fi
    dd if="$packaged" of="$work/payload" bs="$image_size" count=1 \
        2> /dev/null
    if ! cmp -s "$copy" "$work/payload"; then
        fail "$path: the packaged file holds other firmware than the image" \
             "it was built from, so flashing the .dfu would not write" \
             "this build"
        return 1
    fi
    printf '%s: %s\n' "$path" "$found"
    return 0
}

# The images users flash, from the one list there is. The revision check owns
# it, and refuses a tree with a firmware directory that list does not
# classify, so an image nobody has decided about fails here too.
shipped_images() {
    if [ ! -x "$revision_check" ]; then
        fail "$revision_check is missing or not executable, so there is no" \
             "list of the images users flash to check"
        return 1
    fi
    if ! shipped=$("$revision_check" --list-shipped 2> "$work/list-why"); then
        fail "the list of shipped images could not be read from" \
             "$(basename "$revision_check"):"
        sed 's/^/       /' "$work/list-why" >&2
        return 1
    fi
    printf '%s\n' "$shipped"
    return 0
}

# Print every shipped image found under a directory, one a line. The layout
# below that directory is not fixed: CI downloads one artifact per runner, and
# each holds the images in their own subdirectories.
find_images() {
    directory=$1
    find_status=0

    # Through a file rather than a pipeline: a list that could not be read at
    # all has to fail here, and a `for` over a failed command substitution
    # would quietly find nothing to do.
    shipped_images > "$work/shipped" || return 1
    while IFS= read -r image; do
        [ -n "$image" ] || continue
        found=$(find "$directory" -type f -name "$image.bin" | sort)
        if [ -z "$found" ]; then
            fail "no $image.bin was built under $directory, so there is" \
                 "nothing to package and the one check that reads a built" \
                 ".dfu has nothing to read"
            find_status=1
            continue
        fi
        printf '%s\n' "$found"
    done < "$work/shipped"
    return "$find_status"
}

from_directory=
while [ $# -gt 0 ]; do
    case $1 in
    -h|--help)
        usage
        exit 0
        ;;
    --dfu-tool)
        if [ $# -lt 2 ]; then
            fail "--dfu-tool takes the ubertooth-dfu to package with"
            exit 1
        fi
        # A repeated option is refused rather than quietly reconciled, as
        # every tool in this tree does: two tools named in one command line
        # is a mistake, and acting on one of them hides it.
        if [ "$dfu_tool" != "$DEFAULT_DFU_TOOL" ]; then
            fail "--dfu-tool twice ($dfu_tool, then $2): name one tool"
            exit 1
        fi
        dfu_tool=$2
        shift 2
        ;;
    --from)
        if [ $# -lt 2 ]; then
            fail "--from takes one directory"
            exit 1
        fi
        if [ -n "$from_directory" ]; then
            fail "--from twice ($from_directory, then $2): name one directory"
            exit 1
        fi
        if [ ! -d "$2" ]; then
            fail "$2 is not a directory"
            exit 1
        fi
        from_directory=$2
        shift 2
        ;;
    -*)
        fail "unknown option: $1"
        exit 1
        ;;
    *)
        break
        ;;
    esac
done

# The loop above stops at the first image, so anything after it is a file
# name. An option there would have been read as one, and the error would name
# a missing image rather than the option that was ignored.
for argument in "$@"; do
    case $argument in
    -?*)
        fail "$argument comes after the images, where it reads as a file" \
             "name: the options go first"
        exit 1
        ;;
    esac
done

if [ -n "$from_directory" ] && [ $# -gt 0 ]; then
    fail "--from reads the images from a directory, so an image named as" \
         "well ($1) has nothing to mean. Use one or the other."
    exit 1
fi
if [ -z "$from_directory" ] && [ $# -eq 0 ]; then
    # The error line and then the usage text, both on standard error, as
    # .kiro/steering/cli-output.md asks.
    fail "an image or --from <dir> is needed"
    usage >&2
    exit 1
fi

# A tool that is not there is the one failure worth naming before anything is
# built or copied: every image would fail the same way with the same message.
if ! command -v "$dfu_tool" > /dev/null 2>&1; then
    fail "$dfu_tool cannot be run, so nothing can be packaged. Build the" \
         "host tools and pass --dfu-tool <path to ubertooth-dfu>."
    exit 1
fi

build_checker || exit 1

status=0
if [ -n "$from_directory" ]; then
    find_images "$from_directory" > "$work/images" || status=1
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
