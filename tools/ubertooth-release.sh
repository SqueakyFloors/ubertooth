#!/bin/bash

# "Run from the base of the Ubertooth repository, on the release branch (probably master)
# usage: tools/ubertooth-release.sh <release version name>

# Stop at the first failure. Without this a firmware image that fails to build
# only prints an error: the copies of its files fail too, and the script still
# packs a tarball and reports success.
set -e
set -o pipefail


if test $# -lt 1 ; then
	version=$(git rev-parse --abbrev-ref HEAD)
else
	version=${1}
fi

# FIXME you'll need to update these:
top="$(pwd)/../release"

# A published image must not carry the name of the user or the machine that
# built it. firmware/common.mk otherwise takes those from `whoami` and
# `hostname -s`, so pin both to a neutral stamp. Variables given on the make
# command line override the makefile's own values and reach every sub-make.
version_by="ubertooth"
version_host="release"
stamp=(VERSION_BY="${version_by}" VERSION_HOST="${version_host}")

releasename="ubertooth-${version}"
targetdir="${top}/${releasename}"

mkdir -p "${targetdir}"
git archive --format=tar  HEAD | (cd "${targetdir}" && tar xf -)

# The firmware version string now comes from a generated version.h (the -D
# GIT_REVISION compile flag was removed). The archive has no .git, so write a
# version.h carrying the release version into each firmware image directory;
# the build uses a shipped version.h as-is when git is unavailable.
#
# `make clean` deletes version.h along with the rest of the build output, so
# this runs after every clean below rather than once here. Without that the
# images report "git-unknown" instead of the release.
write_version_headers() {
	for imgdir in bluetooth_rxtx btbr assembly_test bootloader; do
		d="${targetdir}/firmware/${imgdir}"
		test -d "${d}" || continue
		cat > "${d}/version.h" <<EOF
/* Generated for release ${version} by tools/ubertooth-release.sh. */
#ifndef UBERTOOTH_VERSION_H
#define UBERTOOTH_VERSION_H
#define GIT_REVISION "${version}"
#define FW_IMAGE_NAME "${imgdir}"
#define COMPILE_BY "${version_by}"
#define COMPILE_HOST "${version_host}"
#define TIMESTAMP "$(date)"
#endif
EOF
	done
}

# The version a build with no git history reports -- which is what this
# archive is -- comes from LATEST_RELEASE in host/cmake/set_release.cmake, so
# set it to the release. BSD and GNU sed disagree about `-i`, so the edit goes
# through a temporary file. The grep stops a silent no-op if the line moves
# again.
relfile="${targetdir}/host/cmake/set_release.cmake"
sed -e "s/^set(LATEST_RELEASE .*/set(LATEST_RELEASE \"${version}\")/" \
	"${relfile}" > "${relfile}.tmp"
mv "${relfile}.tmp" "${relfile}"
grep -q "^set(LATEST_RELEASE \"${version}\")$" "${relfile}"

############################
# Software
############################
# The documentation needs no step here: it lives in docs/ and is built by Read
# the Docs. The nanoc web site this used to compile left the tree years ago.
export BOARD=UBERTOOTH_ONE
# -p so a re-run after a failure does not stop here.
mkdir -p "${targetdir}/ubertooth-one-firmware-bin"
cd "${targetdir}/firmware" || exit 1
make clean
write_version_headers
make "${stamp[@]}" bluetooth_rxtx
cp bluetooth_rxtx/bluetooth_rxtx.hex "${targetdir}/ubertooth-one-firmware-bin/"
cp bluetooth_rxtx/bluetooth_rxtx.bin "${targetdir}/ubertooth-one-firmware-bin/"
cp bluetooth_rxtx/bluetooth_rxtx.dfu "${targetdir}/ubertooth-one-firmware-bin/"
# btbr, the Classic Bluetooth image ubertooth-btbr talks to. It is a second
# shipped image, so a release carries it alongside bluetooth_rxtx.
make "${stamp[@]}" btbr
cp btbr/btbr.hex "${targetdir}/ubertooth-one-firmware-bin/"
cp btbr/btbr.bin "${targetdir}/ubertooth-one-firmware-bin/"
cp btbr/btbr.dfu "${targetdir}/ubertooth-one-firmware-bin/"
make "${stamp[@]}" bootloader
cp bootloader/bootloader.hex "${targetdir}/ubertooth-one-firmware-bin/"
cp bootloader/bootloader.bin "${targetdir}/ubertooth-one-firmware-bin/"
make "${stamp[@]}" assembly_test
cp assembly_test/assembly_test.hex "${targetdir}/ubertooth-one-firmware-bin/"
cp assembly_test/assembly_test.bin "${targetdir}/ubertooth-one-firmware-bin/"
make clean

# Receive Only firmware
write_version_headers
DISABLE_TX=1 make "${stamp[@]}" bluetooth_rxtx
cp bluetooth_rxtx/bluetooth_rxtx.hex "${targetdir}/ubertooth-one-firmware-bin/bluetooth_rx_only.hex"
cp bluetooth_rxtx/bluetooth_rxtx.bin "${targetdir}/ubertooth-one-firmware-bin/bluetooth_rx_only.bin"
cp bluetooth_rxtx/bluetooth_rxtx.dfu "${targetdir}/ubertooth-one-firmware-bin/bluetooth_rx_only.dfu"
make clean
# Once more after the last clean: these headers are not for a build here, they
# are what ships in the archive, so someone who builds the firmware from the
# release gets the release version rather than "git-unknown".
write_version_headers

############################
# Clean up
############################
cp "${targetdir}/tools/RELEASENOTES" "${targetdir}/"
rm -rf "${targetdir}/tools"
rm -rf "${targetdir}/hardware"
# -f because not every tag carries all of these.
rm -f "${targetdir}/.gitignore"
rm -f "${targetdir}/.travis.yml"

############################
# Archive
############################
cd "${top}" || exit 1
tar -cJf "${releasename}.tar.xz" "${releasename}/"

# sha256sum and sha512sum are GNU; macOS ships shasum instead.
if command -v sha256sum > /dev/null ; then
	sha256sum "${releasename}.tar.xz"
	sha512sum "${releasename}.tar.xz"
else
	shasum -a 256 "${releasename}.tar.xz"
	shasum -a 512 "${releasename}.tar.xz"
fi
