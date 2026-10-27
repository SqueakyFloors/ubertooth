#!/bin/sh
# Check that tools/check-dfu-package.sh accepts and refuses the packaged
# images it is meant to.
#
#     tools/check-dfu-package-selftest.sh
#
# The check it exercises is the only thing in CI that reads a built .dfu the
# way `ubertooth-dfu` reads it, so it has to be right in both directions: a
# packaging step that wrote a file the tool would refuse must go red, and an
# ordinary one must go green. The dfu-package CI job shows the green
# direction on the real tool and the real images; nothing but this shows the
# red one, because a release that broke packaging is exactly when nobody can
# ask CI to try it.
#
# So each case below packages with a stand-in for `ubertooth-dfu -s`: a
# program that appends a DFU suffix with the fields it is told to, including
# the wrong ones. It is built from the host's own dfu_image_crc32(), so the
# CRC over a deliberately broken file is still a real CRC and the case fails
# for the reason it is named after.
#
# No ARM toolchain, no board and no real tool: this runs anywhere a C
# compiler does.
#
# Prints each case with its exit status and both output streams, so a failing
# run says what happened without a rerun. Exits 0 when every case holds, 1
# otherwise.

set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 1
top=$(cd "$here/.." && pwd) || exit 1
check="$here/check-dfu-package.sh"

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

# The suffix the real packaging step writes, from
# host/ubertooth-tools/src/dfu.h and the DFU 1.1 specification. Spelt out here
# rather than read from those files: a self-test taking the layout from the
# same place as the code under test could not tell a wrong field from a right
# one.
VENDOR=0x1d50
PRODUCT=0x6003
BCD_DFU=0x0100

# Build the stand-in packaging tool: it appends a DFU suffix naming the vendor,
# product and DFU version it is given, with the signature and CRC either right
# or deliberately wrong, and writes <stem>.dfu beside its input, as the real
# `ubertooth-dfu -s` does.
src="$top/host/ubertooth-tools/src"
header="$top/host/libubertooth/src"
cat > "$work/fake-sign.c" <<'EOF'
/* A stand-in for `ubertooth-dfu -s`, for the self-test of
 * tools/check-dfu-package.sh: append a DFU suffix to the image named and
 * write <stem>.dfu beside it.
 *
 *     fake-sign <image.bin> <vendor> <product> <bcdDFU> <signature> <crc>
 *
 * <signature> is "ufd" for the real one or anything else for a wrong one;
 * <crc> is "crc" for the real one, "bad" for a wrong one, and "empty" writes
 * a file holding the suffix and no firmware at all. The CRC comes from the
 * host's own dfu_image_crc32(), so a file meant to fail on its product ID
 * fails on that and not on a CRC nobody computed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dfu_recovery.h"

/* The DFU 1.1 suffix: 16 little-endian bytes at the end of the file, holding
 * bcdDevice, idProduct, idVendor, bcdDFU, ucDfuSig[3], bLength and dwCRC in
 * that order. Written out here rather than taken from the host's dfu.h, so
 * that a wrong offset there would make a case fail rather than pass. */
#define SUFFIX_LENGTH 16
#define OFF_BCDDEVICE 0
#define OFF_IDPRODUCT 2
#define OFF_IDVENDOR 4
#define OFF_BCDDFU 6
#define OFF_UCDFUSIG 8
#define OFF_BLENGTH 11
#define OFF_DWCRC 12

static void write_le16(unsigned char *bytes, unsigned value)
{
	bytes[0] = (unsigned char)(value & 0xff);
	bytes[1] = (unsigned char)((value >> 8) & 0xff);
}

int main(int argc, char **argv)
{
	unsigned char *buffer, *suffix;
	char *output_name, *dot;
	unsigned long vendor, product, bcd_dfu;
	size_t payload_length = 0, buffer_length;
	long file_length;
	uint32_t crc;
	FILE *image;

	if (argc != 7) {
		fprintf(stderr, "usage: fake-sign <image.bin> <vendor> <product>"
		                " <bcdDFU> <signature> <crc>\n");
		return 2;
	}
	vendor = strtoul(argv[2], NULL, 0);
	product = strtoul(argv[3], NULL, 0);
	bcd_dfu = strtoul(argv[4], NULL, 0);

	image = fopen(argv[1], "rb");
	if (image == NULL) {
		perror(argv[1]);
		return 2;
	}
	if (fseek(image, 0, SEEK_END) != 0 || (file_length = ftell(image)) < 0) {
		perror(argv[1]);
		fclose(image);
		return 2;
	}
	rewind(image);
	buffer = malloc((size_t)file_length + SUFFIX_LENGTH);
	if (buffer == NULL) {
		fprintf(stderr, "fake-sign: out of memory\n");
		fclose(image);
		return 2;
	}
	if (strcmp(argv[6], "empty") != 0) {
		payload_length = fread(buffer, 1, (size_t)file_length, image);
		if (payload_length != (size_t)file_length) {
			fprintf(stderr, "fake-sign: short read\n");
			fclose(image);
			free(buffer);
			return 2;
		}
	}
	fclose(image);

	buffer_length = payload_length + SUFFIX_LENGTH;
	suffix = buffer + payload_length;
	write_le16(suffix + OFF_BCDDEVICE, 0);
	write_le16(suffix + OFF_IDPRODUCT, (unsigned)product);
	write_le16(suffix + OFF_IDVENDOR, (unsigned)vendor);
	write_le16(suffix + OFF_BCDDFU, (unsigned)bcd_dfu);
	if (strcmp(argv[5], "ufd") == 0) {
		suffix[OFF_UCDFUSIG + 0] = 0x55;
		suffix[OFF_UCDFUSIG + 1] = 0x46;
		suffix[OFF_UCDFUSIG + 2] = 0x44;
	} else {
		suffix[OFF_UCDFUSIG + 0] = 'n';
		suffix[OFF_UCDFUSIG + 1] = 'o';
		suffix[OFF_UCDFUSIG + 2] = 't';
	}
	suffix[OFF_BLENGTH] = SUFFIX_LENGTH;
	crc = dfu_image_crc32(buffer, buffer_length - 4);
	if (strcmp(argv[6], "bad") == 0)
		crc ^= 0xffffffff;
	suffix[OFF_DWCRC + 0] = (unsigned char)(crc & 0xff);
	suffix[OFF_DWCRC + 1] = (unsigned char)((crc >> 8) & 0xff);
	suffix[OFF_DWCRC + 2] = (unsigned char)((crc >> 16) & 0xff);
	suffix[OFF_DWCRC + 3] = (unsigned char)((crc >> 24) & 0xff);

	output_name = malloc(strlen(argv[1]) + 5);
	if (output_name == NULL) {
		fprintf(stderr, "fake-sign: out of memory\n");
		free(buffer);
		return 2;
	}
	strcpy(output_name, argv[1]);
	dot = strrchr(output_name, '.');
	if (dot != NULL)
		strcpy(dot, ".dfu");
	else
		strcat(output_name, ".dfu");
	image = fopen(output_name, "wb");
	if (image == NULL) {
		perror(output_name);
		free(output_name);
		free(buffer);
		return 2;
	}
	if (fwrite(buffer, 1, buffer_length, image) != buffer_length ||
	    fclose(image) != 0) {
		perror(output_name);
		free(output_name);
		free(buffer);
		return 2;
	}
	free(output_name);
	free(buffer);
	return 0;
}
EOF
if ! output=$("${CC:-cc}" -o "$work/fake-sign" "$work/fake-sign.c" \
              "$src/dfu_recovery.c" "$src/dfu_transition.c" \
              -I"$src" -I"$header" 2>&1); then
    printf '%s\n' "error: the stand-in packaging tool would not compile:" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

# Write a packaging tool: a script taking `-s <image>` as the real tool does
# and running the stand-in with the fields named here.
#
#     packager <name> <vendor> <product> <bcdDFU> <signature> <crc>
packager() {
    name=$1
    shift
    {
        printf '%s\n' "#!/bin/sh"
        printf '%s\n' "exec \"$work/fake-sign\" \"\$2\" $*"
    } > "$work/$name"
    chmod +x "$work/$name" || exit 1
    printf '%s\n' "$work/$name"
}

good=$(packager good $VENDOR $PRODUCT $BCD_DFU ufd crc)
wrong_product=$(packager wrong-product $VENDOR 0x9999 $BCD_DFU ufd crc)
wrong_vendor=$(packager wrong-vendor 0x1111 $PRODUCT $BCD_DFU ufd crc)
wrong_version=$(packager wrong-version $VENDOR $PRODUCT 0x0300 ufd crc)
wrong_signature=$(packager wrong-signature $VENDOR $PRODUCT $BCD_DFU no crc)
wrong_crc=$(packager wrong-crc $VENDOR $PRODUCT $BCD_DFU ufd bad)
no_firmware=$(packager no-firmware $VENDOR $PRODUCT $BCD_DFU ufd empty)

# A packaging tool that fails, and one that succeeds without writing
# anything: both leave the user with no file to flash.
printf '%s\n' "#!/bin/sh" \
    "echo 'cannot write the signed file' >&2" "exit 1" > "$work/failing"
chmod +x "$work/failing" || exit 1
printf '%s\n' "#!/bin/sh" "exit 0" > "$work/silent"
chmod +x "$work/silent" || exit 1

# A packaging tool that writes a valid DFU file whose firmware is not the
# firmware it was given, with the length unchanged. The suffix and its CRC are
# its own, so the tool's own check passes and only a comparison with the input
# can see it.
printf '%s\n' "#!/bin/sh" \
    "cp \"\$2\" \"\$2.kept\"" \
    "printf 'rewritten' | dd of=\"\$2\" bs=1 conv=notrunc 2> /dev/null" \
    "\"$work/fake-sign\" \"\$2\" $VENDOR $PRODUCT $BCD_DFU ufd crc || exit 1" \
    "mv \"\$2.kept\" \"\$2\"" > "$work/rewriting"
chmod +x "$work/rewriting" || exit 1

# A packaging tool that pads the image before signing it. The file it writes
# is a DFU file this device would accept, and it holds sixteen bytes of
# firmware that were never built.
printf '%s\n' "#!/bin/sh" \
    "cp \"\$2\" \"\$2.kept\"" \
    "printf 'padpadpadpadpadp' >> \"\$2\"" \
    "\"$work/fake-sign\" \"\$2\" $VENDOR $PRODUCT $BCD_DFU ufd crc || exit 1" \
    "mv \"\$2.kept\" \"\$2\"" > "$work/padding"
chmod +x "$work/padding" || exit 1

# An image to package: filler, as a real one has code throughout. The bytes
# are not read by anything here -- the suffix is what is checked -- but the
# payload has to be long enough to be a plausible image and the same every
# time, so the CRC a case prints is reproducible.
write_image() {
    path=$1

    mkdir -p "$(dirname "$path")" || exit 1
    i=0
    while [ "$i" -lt 64 ]; do
        printf 'ubertooth firmware filler ubertooth firmware filler\n'
        i=$((i + 1))
    done > "$path"
}

# Run the check, keeping its status and both streams. No set -e: most cases
# are about a check that is meant to fail.
run() {
    case_name=$1
    shift
    printf '\n%s\n' "case: $case_name"
    printf '%s\n' "  check-dfu-package.sh $*"
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
# What an ordinary packaging step produces
# -------------------------------------------------------------------------

image="$work/bluetooth_rxtx.bin"
write_image "$image"

run "an image packaged as the tool packages it" --dfu-tool "$good" "$image"
want_status 0
want_empty_stderr
want_stdout "suffix 16 bytes, DFU 0100, vendor 0x1d50, product 0x6003"

# The image is left as it was, and no .dfu appears beside it: packaging writes
# its output next to its input, and a check must not leave a file behind in a
# directory it was only asked to read.
[ -f "$work/bluetooth_rxtx.dfu" ] &&
    fail "left a .dfu beside the image it checked"

# -------------------------------------------------------------------------
# Packaged files the tool would refuse
# -------------------------------------------------------------------------

run "a suffix naming another product" --dfu-tool "$wrong_product" "$image"
want_status 1
want_empty_stdout
want_stderr "would refuse this image's .dfu"
want_stderr "product 0x9999, not 0x6003"

run "a suffix naming another vendor" --dfu-tool "$wrong_vendor" "$image"
want_status 1
want_empty_stdout
want_stderr "vendor 0x1111, not 0x1d50"

run "a DFU version the host does not know" --dfu-tool "$wrong_version" "$image"
want_status 1
want_empty_stdout
want_stderr "Unknown DFU version"

run "a suffix with the wrong signature" --dfu-tool "$wrong_signature" "$image"
want_status 1
want_empty_stdout
want_stderr "not a DFU file"

run "a corrupted suffix CRC" --dfu-tool "$wrong_crc" "$image"
want_status 1
want_empty_stdout
want_stderr "CRC mismatch"

run "a suffix with no firmware behind it" --dfu-tool "$no_firmware" "$image"
want_status 1
want_empty_stdout
want_stderr "nothing to write"

# -------------------------------------------------------------------------
# A packaging step that produced nothing, or something else
# -------------------------------------------------------------------------

run "a packaging step that failed" --dfu-tool "$work/failing" "$image"
want_status 1
want_empty_stdout
want_stderr "the packaging step failed"
want_stderr "cannot write the signed file"

run "a packaging step that wrote nothing" --dfu-tool "$work/silent" "$image"
want_status 1
want_empty_stdout
want_stderr "wrote no bluetooth_rxtx.dfu"

run "a packaging step that rewrote the firmware" \
    --dfu-tool "$work/rewriting" "$image"
want_status 1
want_empty_stdout
want_stderr "holds other firmware than the image"

run "a packaging step that padded the image" \
    --dfu-tool "$work/padding" "$image"
want_status 1
want_empty_stdout
want_stderr "a .dfu is the image plus"

# -------------------------------------------------------------------------
# Images that are not there, and the command line
# -------------------------------------------------------------------------

run "an image that does not exist" --dfu-tool "$good" "$work/absent.bin"
want_status 1
want_empty_stdout
want_stderr "no such image to package"

run "a directory given as an image" --dfu-tool "$good" "$work"
want_status 1
want_empty_stdout
want_stderr "no such image to package"

run "a packaging tool that is not there" --dfu-tool "$work/absent" "$image"
want_status 1
want_empty_stdout
want_stderr "cannot be run, so nothing can be packaged"

# Run with nothing at all: the error line first and then the usage text, both
# on standard error (.kiro/steering/cli-output.md).
run "no arguments"
want_status 1
want_empty_stdout
want_stderr "error: an image or --from <dir> is needed"
want_stderr "usage:"

run "-h" -h
want_status 0
want_empty_stderr
want_stdout "usage:"

run "an unknown option" --nonsense
want_status 1
want_empty_stdout
want_stderr "unknown option"

run "--dfu-tool with no tool" --dfu-tool
want_status 1
want_stderr "takes the ubertooth-dfu to package with"

# A repeated option is refused rather than quietly reconciled: two tools or
# two directories in one command line is a mistake, and acting on one of them
# hides it.
run "--dfu-tool twice" --dfu-tool "$good" --dfu-tool "$wrong_product" "$image"
want_status 1
want_empty_stdout
want_stderr "--dfu-tool twice"

run "--from twice" --dfu-tool "$good" --from "$work" --from "$work"
want_status 1
want_empty_stdout
want_stderr "--from twice"

# An option after the images would be read as a file name, and the error would
# name a missing image rather than the option that was ignored.
run "an option after the images" --dfu-tool "$good" "$image" --from "$work"
want_status 1
want_empty_stdout
want_stderr "comes after the images"

run "--from with no directory" --from
want_status 1
want_stderr "takes one directory"

run "--from a path that is not a directory" --from "$work/absent"
want_status 1
want_stderr "is not a directory"

run "--from and an image together" --dfu-tool "$good" --from "$work" "$image"
want_status 1
want_stderr "has nothing to mean"

# -------------------------------------------------------------------------
# --from: the mode CI runs, over a directory of built images
# -------------------------------------------------------------------------

built="$work/built"
write_image "$built/firmware-linux/bluetooth_rxtx/bluetooth_rxtx.bin"
write_image "$built/firmware-linux/btbr/btbr.bin"

# Standard output here carries nothing but `path: suffix ...` lines, so a
# caller can read it without filtering.
run "a directory of good images" --dfu-tool "$good" --from "$built"
want_status 0
want_empty_stderr
want_stdout "bluetooth_rxtx.bin: suffix 16 bytes"
want_stdout "btbr.bin: suffix 16 bytes"
want_stdout_only ': suffix 16 bytes, DFU 0100, vendor 0x1d50, product 0x6003, CRC 0x[0-9a-f]{8}$'

# One shipped image missing is a failure, not a pass over the rest: an image
# that stopped being built is exactly what nobody would notice.
mv "$built/firmware-linux/btbr/btbr.bin" "$work/kept-btbr.bin"
run "a shipped image that was not built" --dfu-tool "$good" --from "$built"
want_status 1
want_stderr "no btbr.bin was built under"
want_stdout "bluetooth_rxtx.bin: suffix 16 bytes"

# Every runner's artifact is packaged, not just the first: a packaging step
# that goes wrong on one platform only has to be caught. The second runner's
# btbr is the one that is checked with a broken packager, which no per-image
# option can express, so the whole run uses it and the point is that both
# images are reported.
cp "$work/kept-btbr.bin" "$built/firmware-linux/btbr/btbr.bin"
write_image "$built/firmware-mac/bluetooth_rxtx/bluetooth_rxtx.bin"
write_image "$built/firmware-mac/btbr/btbr.bin"
run "every runner's images are packaged" --dfu-tool "$good" --from "$built"
want_status 0
want_stdout "firmware-linux/btbr/btbr.bin: suffix 16 bytes"
want_stdout "firmware-mac/btbr/btbr.bin: suffix 16 bytes"

run "a broken packaging step over a directory" \
    --dfu-tool "$wrong_product" --from "$built"
want_status 1
want_empty_stdout
want_stderr "firmware-linux/bluetooth_rxtx/bluetooth_rxtx.bin"
want_stderr "firmware-mac/btbr/btbr.bin"

printf '\n'
if [ "$failures" -eq 0 ]; then
    printf '%s\n' "All cases hold: $(basename "$check") accepts a .dfu"
    printf '%s\n' "ubertooth-dfu would flash and refuses one it would not."
    exit 0
fi
printf '%s\n' "$failures case(s) failed" >&2
exit 1
