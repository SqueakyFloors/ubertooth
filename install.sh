#!/usr/bin/env bash
#
# install.sh - one-command from-source setup for the Ubertooth host tools
#              (and optionally firmware) on Debian/Ubuntu and on macOS with
#              Homebrew.
#
# Builds and installs: system dependencies, libbtbb, the Ubertooth host tools,
# and the Python runtime deps for specan_ui. On Debian/Ubuntu it also installs
# udev rules and builds with -DREQUIRE_ALL=ON so a missing optional dep fails
# loudly. macOS needs no udev rules and has no BlueZ, so ubertooth-scan and
# ubertooth-follow are not built there. Firmware build/flash is opt-in.
#
# When flashing is requested, the flash target is resolved and the firmware is
# built and flashed FIRST (fail fast), before the longer host/libbtbb build.
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
#   --prefix <path>   install prefix (default: /usr/local on Debian/Ubuntu,
#                     the Homebrew prefix on macOS)
#   --skip-deps       do not install system packages; assume they are present
#   --no-sudo         print the commands that need root instead of running them
#                     (dry-run for the privileged steps)
#   -h, --help        show this help
#
# On macOS the install goes into the Homebrew prefix by default, which is owned
# by your user, so no sudo is needed. Homebrew asks for your password only when
# it installs the gcc-arm-embedded toolchain package (--with-firmware).
#
# Copyright 2026. Released under the GPLv2. Refer to COPYING.

set -euo pipefail

WITH_FIRMWARE=0
FLASH=0
FLASH_DEVICE=""
VENV=""
LIBBTBB_REF="master"
NO_SUDO=0
PREFIX=""
SKIP_DEPS=0

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

# List attached Ubertooth serials (one per line), in index order. Works without
# sudo. With one device `ubertooth-util -s` prints "Serial No: <serial>"; with
# several it prints "<index>: <serial>" per device. A failed read is ignored, so
# the exit code is too.
list_serials() {
	"$1" -s 2>&1 | sed -n \
		-e 's/.*Serial No: \([0-9a-f]\{32\}\).*/\1/p' \
		-e 's/^[0-9][0-9]*: \([0-9a-f]\{32\}\)$/\1/p' || true
}

# Install the given Homebrew formulae that are not installed yet.
brew_install() {
	local formula missing=()
	for formula in "$@"; do
		brew list --formula --versions "$formula" >/dev/null 2>&1 || missing+=("$formula")
	done
	if [ "${#missing[@]}" -gt 0 ]; then
		brew install "${missing[@]}"
	fi
}

# Run an install step into PREFIX: directly when PREFIX is writable on macOS
# (the Homebrew prefix or a scratch --prefix), otherwise as root.
as_installer() {
	if [ "$PLATFORM" = macos ] && [ -w "$PREFIX" ]; then
		"$@"
	else
		as_root "$@"
	fi
}

# Build a concise "multiple devices" message listing each device as
# "<index>: <serial>" (the serial list is already in index order), so the user
# can choose by index or serial.
multi_device_msg() {
	local serials="$1" out i=0
	out="multiple Ubertooths attached; pick one with --flash-device <index|serial> (or 'all'):"
	while IFS= read -r s; do
		[ -n "$s" ] && { out="$out"$'\n'"    $i: $s"; i=$((i+1)); }
	done <<< "$serials"
	printf '%s' "$out"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--with-firmware) WITH_FIRMWARE=1 ;;
		--flash) FLASH=1; WITH_FIRMWARE=1 ;;
		--flash-device) shift; FLASH_DEVICE="${1:-}"; FLASH=1; WITH_FIRMWARE=1; [ -n "$FLASH_DEVICE" ] || die "--flash-device needs an index, serial, or 'all'" ;;
		--venv) shift; VENV="${1:-}"; [ -n "$VENV" ] || die "--venv needs a path" ;;
		--libbtbb-ref) shift; LIBBTBB_REF="${1:-}"; [ -n "$LIBBTBB_REF" ] || die "--libbtbb-ref needs a value" ;;
		--prefix) shift; PREFIX="${1:-}"; [ -n "$PREFIX" ] || die "--prefix needs a path" ;;
		--skip-deps) SKIP_DEPS=1 ;;
		--no-sudo) NO_SUDO=1 ;;
		-h|--help) sed -n '3,/^# Copyright/p' "$0" | sed '/^# Copyright/d; s/^#\{1,\} \{0,1\}//; s/^#$//'; exit 0 ;;
		*) die "unknown option: $1 (use --help)" ;;
	esac
	shift
done

# --- platform check -------------------------------------------------------
case "$(uname -s)" in
	Linux)
		command -v apt-get >/dev/null 2>&1 || \
			die "on Linux this script supports Debian/Ubuntu (apt-get) only; see REQUIREMENTS.md for other platforms"
		PLATFORM=debian
		PREFIX="${PREFIX:-/usr/local}"
		JOBS="$(nproc)"
		;;
	Darwin)
		command -v brew >/dev/null 2>&1 || \
			die "on macOS this script needs Homebrew (https://brew.sh); see REQUIREMENTS.md"
		PLATFORM=macos
		BREW_PREFIX="$(brew --prefix)"
		PREFIX="${PREFIX:-$BREW_PREFIX}"
		JOBS="$(sysctl -n hw.ncpu)"
		;;
	*)
		die "unsupported platform $(uname -s); this script supports Debian/Ubuntu and macOS (Homebrew); see REQUIREMENTS.md"
		;;
esac
command -v python3 >/dev/null 2>&1 || die "python3 not found; install Python 3.8 or newer"
ORIG_PATH="$PATH"
mkdir -p "$PREFIX" 2>/dev/null || true
# Tools installed by this run (and an existing install in PREFIX) come first.
export PATH="$PREFIX/bin:$PATH"

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
WORK="${TMPDIR:-/tmp}/ubertooth-install.$$"
mkdir -p "$WORK"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# Build the firmware and flash the selected device(s). Uses whichever
# ubertooth-dfu / ubertooth-util is on PATH (an existing install).
flash_firmware() {
	local dfu util img serials n sel_serial
	dfu="$(command -v ubertooth-dfu || true)"
	util="$(command -v ubertooth-util || true)"
	img="$REPO_ROOT/firmware/bluetooth_rxtx/bluetooth_rxtx.dfu"
	[ -n "$dfu" ] && [ -n "$util" ] || die "ubertooth-dfu/ubertooth-util not found on PATH; build the host tools first"

	info "Building firmware (bluetooth_rxtx)"
	PATH="$(dirname "$dfu"):$PATH" make -C "$REPO_ROOT/firmware/bluetooth_rxtx"

	if [ "$FLASH" -ne 1 ]; then
		return 0
	fi

	if [ "$FLASH_DEVICE" = "all" ]; then
		info "Flashing all attached Ubertooths"
		serials="$(list_serials "$util")"
		[ -n "$serials" ] || die "no Ubertooth serials found to flash"
		for s in $serials; do
			info "  flashing $s"
			"$dfu" -U "$s" -d "$img" -r
		done
	elif [ -n "$FLASH_DEVICE" ]; then
		# Show the serial even when the user selected by index.
		if printf '%s' "$FLASH_DEVICE" | grep -qE '^[0-9]+$'; then
			serials="$(list_serials "$util")"
			sel_serial="$(printf '%s\n' "$serials" | sed -n "$((FLASH_DEVICE+1))p")"
			if [ -n "$sel_serial" ]; then
				info "Flashing bluetooth_rxtx to Ubertooth index $FLASH_DEVICE (serial $sel_serial)"
			else
				info "Flashing bluetooth_rxtx to Ubertooth index $FLASH_DEVICE"
			fi
		else
			info "Flashing bluetooth_rxtx to Ubertooth serial $FLASH_DEVICE"
		fi
		"$dfu" -U "$FLASH_DEVICE" -d "$img" -r
	else
		serials="$(list_serials "$util")"
		n="$(printf '%s\n' "$serials" | grep -c . || true)"
		if [ "$n" -eq 0 ]; then
			die "no Ubertooth found to flash"
		elif [ "$n" -eq 1 ]; then
			info "Flashing bluetooth_rxtx to Ubertooth $serials"
			"$dfu" -U "$serials" -d "$img" -r
		else
			die "$(multi_device_msg "$serials")"
		fi
	fi
}

# --- 0. flash pre-flight (fail fast, BEFORE any build work) ---------------
# Resolve/validate the flash target up front so an ambiguous situation
# (multiple devices, no selector) aborts immediately instead of after a full
# build. Only possible if ubertooth-util is already installed.
if [ "$FLASH" -eq 1 ] && command -v ubertooth-util >/dev/null 2>&1; then
	if [ "$FLASH_DEVICE" != "all" ] && [ -z "$FLASH_DEVICE" ]; then
		_serials="$(list_serials "$(command -v ubertooth-util)")"
		_n="$(printf '%s\n' "$_serials" | grep -c . || true)"
		[ "$_n" -le 1 ] || die "$(multi_device_msg "$_serials")"
	fi
fi

# --- 1. firmware toolchain deps (only what flashing needs, installed early) -
if [ "$WITH_FIRMWARE" -eq 1 ] && [ "$SKIP_DEPS" -eq 0 ]; then
	if [ "$PLATFORM" = debian ]; then
		info "Installing firmware toolchain (apt)"
		as_root apt-get update -qq
		as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y gcc-arm-none-eabi dfu-util make
	else
		info "Installing firmware toolchain (Homebrew)"
		brew_install dfu-util
		brew list --cask --versions gcc-arm-embedded >/dev/null 2>&1 || \
			brew install --cask gcc-arm-embedded
	fi
fi

# --- 2. firmware build + flash FIRST (fail fast on a bad device) ----------
# Only if the host tools (ubertooth-dfu) already exist; on a fresh machine they
# are built below first, then we flash.
FLASHED=0
if [ "$WITH_FIRMWARE" -eq 1 ] && command -v ubertooth-dfu >/dev/null 2>&1; then
	flash_firmware
	FLASHED=1
fi

# --- 3. system dependencies (host build) ----------------------------------
if [ "$SKIP_DEPS" -eq 1 ]; then
	info "Skipping system dependencies (--skip-deps)"
elif [ "$PLATFORM" = debian ]; then
	info "Installing system dependencies (apt)"
	as_root apt-get update -qq
	as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y \
		cmake build-essential pkg-config libusb-1.0-0-dev libbluetooth-dev \
		libfftw3-dev python3 python3-venv python3-usb1 git
else
	info "Installing system dependencies (Homebrew)"
	brew_install cmake pkg-config libusb fftw
fi

# --- 4. Python interpreter / venv ----------------------------------------
PYTHON="python3"
if [ -n "$VENV" ]; then
	info "Creating/using Python venv at $VENV"
	[ -d "$VENV" ] || python3 -m venv "$VENV"
	# shellcheck disable=SC1091
	PYTHON="$VENV/bin/python"
	"$PYTHON" -m pip install --quiet --upgrade pip
fi
info "Installing Python runtime deps for specan_ui and ubtbr"
PIP_DEPS=(numpy qtpy pyside6)
# Debian/Ubuntu gets the ubtbr USB binding from apt (python3-usb1).
[ "$PLATFORM" = macos ] && PIP_DEPS+=(libusb1==3.4.0)
if [ "$PLATFORM" = debian ]; then
	GUI_DEPS_HINT="install python3-numpy python3-qtpy python3-pyside6 (apt) or use --venv"
else
	GUI_DEPS_HINT="use --venv, or pip-install ${PIP_DEPS[*]} for $(command -v python3)"
fi
if [ -n "$VENV" ]; then
	"$PYTHON" -m pip install --quiet "${PIP_DEPS[@]}" libusb1==3.4.0 || \
		info "WARNING: could not pip-install Python runtime deps into the venv"
else
	if ! "$PYTHON" -m pip install --quiet "${PIP_DEPS[@]}" 2>/dev/null; then
		"$PYTHON" -m pip install --quiet --break-system-packages "${PIP_DEPS[@]}" 2>/dev/null || \
			info "WARNING: could not pip-install GUI deps; $GUI_DEPS_HINT"
	fi
fi

# --- 5. libbtbb -----------------------------------------------------------
info "Building libbtbb ($LIBBTBB_REF)"
git clone --quiet https://github.com/greatscottgadgets/libbtbb.git "$WORK/libbtbb"
git -C "$WORK/libbtbb" checkout --quiet "$LIBBTBB_REF"
cmake -S "$WORK/libbtbb" -B "$WORK/libbtbb/build" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_PYTHON=OFF \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$WORK/libbtbb/build" -j"$JOBS" >/dev/null
as_installer cmake --install "$WORK/libbtbb/build" >/dev/null
[ "$PLATFORM" = debian ] && as_root ldconfig

# --- 6. host tools --------------------------------------------------------
# The installed tools find libubertooth and libbtbb under PREFIX through their
# run path, so a non-default prefix works without ldconfig or DYLD settings.
CMAKE_ARGS=(
	"-DCMAKE_INSTALL_PREFIX=$PREFIX"
	"-DCMAKE_PREFIX_PATH=$PREFIX"
	"-DCMAKE_INSTALL_RPATH=$PREFIX/lib"
)
if [ "$PLATFORM" = debian ]; then
	info "Building Ubertooth host tools (REQUIRE_ALL=ON)"
	CMAKE_ARGS+=(-DREQUIRE_ALL=ON)
else
	# No BlueZ on macOS, so REQUIRE_ALL is not used; Homebrew's libusb headers
	# are not on the compiler's default search path.
	info "Building Ubertooth host tools"
	CMAKE_ARGS+=("-DCMAKE_C_FLAGS=-I$BREW_PREFIX/include")
fi
[ -n "$VENV" ] && CMAKE_ARGS+=("-DPython3_EXECUTABLE=$PYTHON")
# Forget a libbtbb found by an earlier configure so this run's install is used.
cmake -S "$REPO_ROOT/host" -B "$REPO_ROOT/host/build" -U 'LIBBTBB_*' "${CMAKE_ARGS[@]}"
cmake --build "$REPO_ROOT/host/build" -j"$JOBS"
as_installer cmake --install "$REPO_ROOT/host/build"
if [ "$PLATFORM" = debian ]; then
	as_root ldconfig
	info "udev rules installed with the host tools (unplug/replug the Ubertooth)"
fi

# --- 7. firmware flash (deferred case: fresh install built tools just now) -
if [ "$WITH_FIRMWARE" -eq 1 ] && [ "$FLASHED" -eq 0 ]; then
	flash_firmware
fi

info "Done. Try: ubertooth-util -v   (and: ubertooth-specan-ui)"
case ":$ORIG_PATH:" in
	*":$PREFIX/bin:"*) ;;
	*) info "Add $PREFIX/bin to your PATH to run the installed tools" ;;
esac
