#!/usr/bin/env bash
#
# install.sh - one-command from-source setup for the Ubertooth host tools
#              (and optionally firmware) on Debian/Ubuntu.
#
# Builds and installs: system dependencies, libbtbb, the Ubertooth host tools
# (with -DREQUIRE_ALL=ON so a missing optional dep fails loudly), udev rules,
# and the Python runtime deps for specan_ui. Firmware build/flash is opt-in.
#
# This is the from-source "just works" path. It is NOT a replacement for distro
# packaging (apt/dnf/brew). Idempotent: safe to re-run.
#
# Usage:
#   ./install.sh [options]
#
# Options:
#   --with-firmware   also build the firmware images (needs arm-none-eabi-gcc)
#   --flash           also flash bluetooth_rxtx to an attached Ubertooth
#                     (implies --with-firmware; STATE-CHANGING)
#   --flash-device <index|serial|all>
#                     flash a specific Ubertooth (implies --flash); use "all"
#                     to flash every attached device. Recommended when more
#                     than one device is attached.
#   --venv <path>     create/use a Python venv at <path> for the Python tools
#                     and point the build at its interpreter
#   --libbtbb-ref <r> git ref/tag of libbtbb to build (default: master)
#   --no-sudo         print the commands that need root instead of running them
#                     (dry-run for the privileged steps)
#   -h, --help        show this help
#
# Copyright 2026. Released under the GPLv2. Refer to COPYING.

set -euo pipefail

WITH_FIRMWARE=0
FLASH=0
FLASH_DEVICE=""
VENV=""
LIBBTBB_REF="master"
NO_SUDO=0

die() { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# Run a privileged command, or print it under --no-sudo.
as_root() {
	if [ "$NO_SUDO" -eq 1 ]; then
		echo "    [would run as root] $*"
	else
		sudo "$@"
	fi
}

while [ $# -gt 0 ]; do
	case "$1" in
		--with-firmware) WITH_FIRMWARE=1 ;;
		--flash) FLASH=1; WITH_FIRMWARE=1 ;;
		--flash-device) shift; FLASH_DEVICE="${1:-}"; FLASH=1; WITH_FIRMWARE=1; [ -n "$FLASH_DEVICE" ] || die "--flash-device needs an index or serial" ;;
		--venv) shift; VENV="${1:-}"; [ -n "$VENV" ] || die "--venv needs a path" ;;
		--libbtbb-ref) shift; LIBBTBB_REF="${1:-}"; [ -n "$LIBBTBB_REF" ] || die "--libbtbb-ref needs a value" ;;
		--no-sudo) NO_SUDO=1 ;;
		-h|--help) sed -n '3,/^# Copyright/p' "$0" | sed '/^# Copyright/d; s/^#\{1,\} \{0,1\}//; s/^#$//'; exit 0 ;;
		*) die "unknown option: $1 (use --help)" ;;
	esac
	shift
done

# --- platform check -------------------------------------------------------
command -v apt-get >/dev/null 2>&1 || \
	die "this script currently supports Debian/Ubuntu (apt-get) only; see REQUIREMENTS.md for other platforms"

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
WORK="${TMPDIR:-/tmp}/ubertooth-install.$$"
mkdir -p "$WORK"

# --- 1. system dependencies ----------------------------------------------
info "Installing system dependencies (apt)"
PKGS=(cmake build-essential pkg-config libusb-1.0-0-dev libbluetooth-dev
      libfftw3-dev python3 python3-venv python3-setuptools git)
if [ "$WITH_FIRMWARE" -eq 1 ]; then
	PKGS+=(gcc-arm-none-eabi dfu-util)
fi
as_root apt-get update -qq
as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "${PKGS[@]}"

# --- 2. Python interpreter / venv ----------------------------------------
PYTHON="python3"
if [ -n "$VENV" ]; then
	info "Creating/using Python venv at $VENV"
	[ -d "$VENV" ] || python3 -m venv "$VENV"
	# shellcheck disable=SC1091
	PYTHON="$VENV/bin/python"
	"$PYTHON" -m pip install --quiet --upgrade pip setuptools
fi
info "Installing Python runtime deps for specan_ui"
PIP_DEPS=(numpy qtpy pyside6)
if [ -n "$VENV" ]; then
	"$PYTHON" -m pip install --quiet "${PIP_DEPS[@]}" || \
		info "WARNING: could not pip-install $* GUI deps into the venv"
else
	# System Python on modern Debian is externally managed (PEP 668); prefer the
	# distro packages, fall back to pip --break-system-packages, and never abort.
	if ! "$PYTHON" -m pip install --quiet "${PIP_DEPS[@]}" 2>/dev/null; then
		"$PYTHON" -m pip install --quiet --break-system-packages "${PIP_DEPS[@]}" 2>/dev/null || \
			info "WARNING: could not pip-install GUI deps; install python3-numpy python3-qtpy python3-pyside6 (apt) or use --venv"
	fi
fi

# --- 3. libbtbb -----------------------------------------------------------
info "Building libbtbb ($LIBBTBB_REF)"
git clone --quiet https://github.com/greatscottgadgets/libbtbb.git "$WORK/libbtbb"
git -C "$WORK/libbtbb" checkout --quiet "$LIBBTBB_REF"
cmake -S "$WORK/libbtbb" -B "$WORK/libbtbb/build" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_PYTHON=OFF >/dev/null
cmake --build "$WORK/libbtbb/build" -j"$(nproc)" >/dev/null
as_root cmake --install "$WORK/libbtbb/build" >/dev/null
as_root ldconfig

# --- 4. host tools --------------------------------------------------------
info "Building Ubertooth host tools (REQUIRE_ALL=ON)"
CMAKE_ARGS=(-DREQUIRE_ALL=ON)
[ -n "$VENV" ] && CMAKE_ARGS+=("-DPython3_EXECUTABLE=$PYTHON")
cmake -S "$REPO_ROOT/host" -B "$REPO_ROOT/host/build" "${CMAKE_ARGS[@]}"
cmake --build "$REPO_ROOT/host/build" -j"$(nproc)"
as_root cmake --install "$REPO_ROOT/host/build"
as_root ldconfig

# --- 5. udev rules (handled by the host install on Linux) ----------------
info "udev rules installed with the host tools (unplug/replug the Ubertooth)"

# --- 6. firmware (opt-in) -------------------------------------------------
if [ "$WITH_FIRMWARE" -eq 1 ]; then
	info "Building firmware (bluetooth_rxtx)"
	make -C "$REPO_ROOT/firmware/bluetooth_rxtx"
	if [ "$FLASH" -eq 1 ]; then
		DFU="$REPO_ROOT/host/build/ubertooth-tools/src/ubertooth-dfu"
		UTIL="$REPO_ROOT/host/build/ubertooth-tools/src/ubertooth-util"
		IMG="$REPO_ROOT/firmware/bluetooth_rxtx/bluetooth_rxtx.dfu"
		if [ "$FLASH_DEVICE" = "all" ]; then
			# Enumerate every attached Ubertooth by serial and flash each, so
			# order/index changes between flashes don't matter.
			info "Flashing all attached Ubertooths"
			SERIALS=$("$UTIL" -s 2>&1 | sed -n 's/.*Serial No: \([0-9a-f]*\).*/\1/p')
			[ -n "$SERIALS" ] || die "no Ubertooth serials found to flash"
			for s in $SERIALS; do
				info "  flashing $s"
				"$DFU" -U "$s" -d "$IMG" -r
			done
		elif [ -n "$FLASH_DEVICE" ]; then
			info "Flashing bluetooth_rxtx to Ubertooth '$FLASH_DEVICE'"
			"$DFU" -U "$FLASH_DEVICE" -d "$IMG" -r
		else
			# No device specified: enumerate. Flash the sole device (by serial,
			# so we report exactly what we hit); refuse if several are attached
			# rather than silently picking one.
			SERIALS=$("$UTIL" -s 2>&1 | sed -n 's/.*Serial No: \([0-9a-f]*\).*/\1/p')
			N=$(printf '%s\n' "$SERIALS" | grep -c .)
			if [ "$N" -eq 0 ]; then
				die "no Ubertooth found to flash"
			elif [ "$N" -eq 1 ]; then
				info "Flashing bluetooth_rxtx to Ubertooth $SERIALS"
				"$DFU" -U "$SERIALS" -d "$IMG" -r
			else
				die "multiple Ubertooths attached; choose one with --flash-device <serial>, or --flash-device all:
$SERIALS"
			fi
		fi
	fi
fi

rm -rf "$WORK"
info "Done. Try: ubertooth-util -v   (and: ubertooth-specan-ui)"
