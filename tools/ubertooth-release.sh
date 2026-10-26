#!/bin/bash

# "Run from the base of the Ubertooth repository, on the release branch (probably master)
# usage: tools/ubertooth-release.sh <release version name>


if test $# -lt 1 ; then
	version=$(git rev-parse --abbrev-ref HEAD)
else
	version=${1}
fi

# FIXME you'll need to update these:
top="$(pwd)/../release"

releasename="ubertooth-${version}"
targetdir="${top}/${releasename}"

mkdir -p "${targetdir}"
git archive --format=tar  HEAD | (cd "${targetdir}" && tar xf -)

# The firmware version string now comes from a generated version.h (the -D
# GIT_REVISION compile flag was removed). The archive has no .git, so write a
# version.h carrying the release version into each firmware image directory;
# the build uses a shipped version.h as-is when git is unavailable.
for imgdir in bluetooth_rxtx btbr assembly_test bootloader; do
	d="${targetdir}/firmware/${imgdir}"
	test -d "${d}" || continue
	cat > "${d}/version.h" <<EOF
/* Generated for release ${version} by tools/ubertooth-release.sh. */
#ifndef UBERTOOTH_VERSION_H
#define UBERTOOTH_VERSION_H
#define GIT_REVISION "${version}"
#define COMPILE_BY "release"
#define COMPILE_HOST "$(hostname -s)"
#define TIMESTAMP "$(date)"
#endif
EOF
done

sed -e "s/set(RELEASE.*/set(RELEASE \"${version}\")/" -i "${targetdir}/host/libubertooth/src/CMakeLists.txt"

############################
# Documentation
############################
cd "${targetdir}/web" || exit 1
make

############################
# Software
############################
export BOARD=UBERTOOTH_ONE
mkdir "${targetdir}/ubertooth-one-firmware-bin"
cd "${targetdir}/firmware" || exit 1
make clean
make bluetooth_rxtx
cp bluetooth_rxtx/bluetooth_rxtx.hex "${targetdir}/ubertooth-one-firmware-bin/"
cp bluetooth_rxtx/bluetooth_rxtx.bin "${targetdir}/ubertooth-one-firmware-bin/"
cp bluetooth_rxtx/bluetooth_rxtx.dfu "${targetdir}/ubertooth-one-firmware-bin/"
make bootloader
cp bootloader/bootloader.hex "${targetdir}/ubertooth-one-firmware-bin/"
cp bootloader/bootloader.bin "${targetdir}/ubertooth-one-firmware-bin/"
make assembly_test
cp assembly_test/assembly_test.hex "${targetdir}/ubertooth-one-firmware-bin/"
cp assembly_test/assembly_test.bin "${targetdir}/ubertooth-one-firmware-bin/"
make clean

# Receive Only firmware
DISABLE_TX=1 make bluetooth_rxtx
cp bluetooth_rxtx/bluetooth_rxtx.hex "${targetdir}/ubertooth-one-firmware-bin/bluetooth_rx_only.hex"
cp bluetooth_rxtx/bluetooth_rxtx.bin "${targetdir}/ubertooth-one-firmware-bin/bluetooth_rx_only.bin"
cp bluetooth_rxtx/bluetooth_rxtx.dfu "${targetdir}/ubertooth-one-firmware-bin/bluetooth_rx_only.dfu"
make clean

############################
# Clean up
############################
cp "${targetdir}/tools/RELEASENOTES" "${targetdir}/"
rm -rf "${targetdir}/tools"
rm -rf "${targetdir}/hardware"
rm "${targetdir}/.gitignore"
rm "${targetdir}/.travis.yml"

############################
# Archive
############################
cd "${top}" || exit 1
tar -cJf "${releasename}.tar.xz" "${releasename}/"

sha256sum "${releasename}.tar.xz"
sha512sum "${releasename}.tar.xz"
