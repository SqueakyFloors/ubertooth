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
#   --libbtbb-url <u> git URL to clone libbtbb from
#   --libbtbb-ref <r> git commit, tag or branch of libbtbb to build. These two
#                     default to a pinned commit of this project's fork (both
#                     defaults are printed below); an older libbtbb may need
#                     CMake options this script does not pass.
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
# libbtbb comes from this project's fork, pinned to a commit so a push there
# cannot change what this script builds. The pin is bumped in
# .github/workflows/build.yml, which checks that this copy agrees with it.
# --libbtbb-url / --libbtbb-ref override these for one run.
LIBBTBB_URL="https://github.com/SqueakyFloors/libbtbb.git"
LIBBTBB_REF="2fd0d391d99e9b0620146f63f439c1ec74f79861"
NO_SUDO=0
PREFIX=""
SKIP_DEPS=0

die() { echo "error: $*" >&2; exit 1; }
# Progress, not a result: the script writes nothing to standard output,
# so "./install.sh > log" still shows what it is doing.
info() { echo "==> $*" >&2; }
# The Python tools need Python 3.8 or newer (host/python/cmake/UbertoothPythonFloor.cmake).
require_python() {
	local version
	"$1" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null && return 0
	# Python before 3.4 prints -V on stderr.
	version="$("$1" -V 2>&1)" || version="not runnable"
	die "Python 3.8 or newer is required for the Python tools (found $1: $version)${2:+; $2}"
}

# Run a privileged command, or print it under --no-sudo.
as_root() {
	if [ "$NO_SUDO" -eq 1 ]; then
		echo "    [would run as root] $*" >&2 # visible even when output is silenced
	else
		sudo "$@"
	fi
}

# Print one "<index> <serial>" line per attached Ubertooth, in index order, with
# "?" as the serial of a device that could not be read. The serials come from
# `ubertooth-util -s` in any of its output forms ("<index>: <serial>" for
# several devices, "Device <index>: Serial No: <serial>" from older tools, or a
# single "Serial No: <serial>"); the device count comes from `ubertooth-util -N`,
# so a board whose serial is missing is reported instead of dropped. Exit codes
# are ignored on purpose.
list_devices() {
	local parsed count i serial
	parsed="$("$1" -s 2>&1 | sed -n \
		-e 's/^\([0-9][0-9]*\): \([0-9a-f]\{32\}\)$/\1 \2/p' -e t \
		-e 's/.*Device \([0-9][0-9]*\): Serial No: \([0-9a-f]\{32\}\).*/\1 \2/p' -e t \
		-e 's/^Serial No: \([0-9a-f]\{32\}\).*/0 \1/p' || true)"
	count="$("$1" -N 2>/dev/null || true)"
	case "$count" in
		''|*[!0-9]*) count="$(printf '%s\n' "$parsed" | grep -c . || true)" ;;
	esac
	i=0
	while [ "$i" -lt "$count" ]; do
		serial="$(printf '%s\n' "$parsed" | awk -v i="$i" '$1 == i { print $2; exit }')"
		echo "$i ${serial:-?}"
		i=$((i+1))
	done
}

# Build a concise "multiple devices" message listing each device as
# "<index>: <serial>", so the user can choose by index or serial.
multi_device_msg() {
	local devices="$1" out index serial
	out="multiple Ubertooths attached; pick one with --flash-device <index|serial> (or 'all'):"
	while read -r index serial; do
		[ -n "$index" ] || continue
		[ "$serial" = "?" ] && serial="<could not be read>"
		out="$out"$'\n'"    $index: $serial"
	done <<< "$devices"
	printf '%s' "$out"
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
	# Probe lib/ when it exists: Intel Homebrew owns /usr/local/lib but not
	# /usr/local itself.
	local probe="$PREFIX/lib"
	[ -d "$probe" ] || probe="$PREFIX"
	if [ "$PLATFORM" = macos ] && [ -w "$probe" ]; then
		"$@"
	else
		as_root "$@"
	fi
}

while [ $# -gt 0 ]; do
	case "$1" in
		--with-firmware) WITH_FIRMWARE=1 ;;
		--flash) FLASH=1; WITH_FIRMWARE=1 ;;
		--flash-device) shift; FLASH_DEVICE="${1:-}"; FLASH=1; WITH_FIRMWARE=1; [ -n "$FLASH_DEVICE" ] || die "--flash-device needs an index, serial, or 'all'" ;;
		--venv) shift; VENV="${1:-}"; [ -n "$VENV" ] || die "--venv needs a path" ;;
		--libbtbb-url) shift; LIBBTBB_URL="${1:-}"; [ -n "$LIBBTBB_URL" ] || die "--libbtbb-url needs a git URL" ;;
		--libbtbb-ref) shift; LIBBTBB_REF="${1:-}"; [ -n "$LIBBTBB_REF" ] || die "--libbtbb-ref needs a value" ;;
		--prefix) shift; PREFIX="${1:-}"; [ -n "$PREFIX" ] || die "--prefix needs a path" ;;
		--skip-deps) SKIP_DEPS=1 ;;
		--no-sudo) NO_SUDO=1 ;;
		-h|--help)
			sed -n '3,/^# Copyright/p' "$0" | sed '/^# Copyright/d; s/^#\{1,\} \{0,1\}//; s/^#$//'
			# Printed rather than written in the comment above, so the pin
			# itself lives in exactly one place in this script.
			printf 'libbtbb defaults:\n  --libbtbb-url %s\n  --libbtbb-ref %s\n' \
				"$LIBBTBB_URL" "$LIBBTBB_REF"
			exit 0 ;;
		*) die "unknown option: $1 (use --help)" ;;
	esac
	shift
done

# --- platform check -------------------------------------------------------
case "$(uname -s)" in
	Linux)
		command -v apt-get >/dev/null 2>&1 || \
			die "on Linux this script supports Debian/Ubuntu (apt-get) only; see REQUIREMENTS.md for other platforms"
		# python3 comes from apt below if it is missing; check it here if not,
		# so an old one stops the script before anything is installed or built.
		if command -v python3 >/dev/null 2>&1; then
			require_python python3
		fi
		PLATFORM=debian
		PREFIX="${PREFIX:-/usr/local}"
		JOBS="$(nproc)"
		;;
	Darwin)
		command -v brew >/dev/null 2>&1 || \
			die "on macOS this script needs Homebrew (https://brew.sh); see REQUIREMENTS.md"
		# Homebrew does not install Python here; apt does on Debian/Ubuntu.
		command -v python3 >/dev/null 2>&1 || \
			die "python3 not found; install Python 3.8 or newer (for example: brew install python)"
		require_python python3 "install a newer one (for example: brew install python)"
		PLATFORM=macos
		BREW_PREFIX="$(brew --prefix)"
		PREFIX="${PREFIX:-$BREW_PREFIX}"
		JOBS="$(sysctl -n hw.ncpu)"
		;;
	*)
		die "unsupported platform $(uname -s); this script supports Debian/Ubuntu and macOS (Homebrew); see REQUIREMENTS.md"
		;;
esac
ORIG_PATH="$PATH"
# Resolve python3 from the user's PATH, before PREFIX/bin is put first.
PYTHON3="$(command -v python3 || true)"
# Run paths and PATH need an absolute prefix. Create it (as root if needed) and
# resolve symlinks so it can be compared with the Homebrew prefix.
case "$PREFIX" in /*) ;; *) PREFIX="$PWD/$PREFIX" ;; esac
mkdir -p "$PREFIX" 2>/dev/null || as_installer mkdir -p "$PREFIX"
[ -d "$PREFIX" ] && PREFIX="$(cd "$PREFIX" && pwd -P)"
if [ "$PLATFORM" = macos ] && [ "$PREFIX" = "$(cd "$BREW_PREFIX" && pwd -P)" ]; then
	# Homebrew core has its own ubertooth and libbtbb; installing over them in
	# the Homebrew prefix would break both.
	for formula in ubertooth libbtbb; do
		if brew list --formula --versions "$formula" >/dev/null 2>&1; then
			die "Homebrew's $formula is installed in $BREW_PREFIX; run 'brew uninstall $formula' first, or install elsewhere with --prefix <path>"
		fi
	done
fi
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
	local dfu util img devices n sel_serial index serial
	dfu="$(command -v ubertooth-dfu || true)"
	util="$(command -v ubertooth-util || true)"
	img="$REPO_ROOT/firmware/bluetooth_rxtx/bluetooth_rxtx.dfu"
	if [ -z "$dfu" ] || [ -z "$util" ]; then
		die "ubertooth-dfu/ubertooth-util not found on PATH; build the host tools first"
	fi

	info "Building firmware (bluetooth_rxtx)"
	PATH="$(dirname "$dfu"):$PATH" make -C "$REPO_ROOT/firmware/bluetooth_rxtx"

	if [ "$FLASH" -ne 1 ]; then
		return 0
	fi

	devices="$(list_devices "$util")"
	n="$(printf '%s\n' "$devices" | grep -c . || true)"
	if [ "$FLASH_DEVICE" = "all" ]; then
		[ "$n" -gt 0 ] || die "no Ubertooth found to flash"
		# Never skip a board silently: refuse if any device cannot be read.
		if printf '%s\n' "$devices" | grep -q ' ?$'; then
			die "$(multi_device_msg "$devices")"$'\n'"a device could not be read; flash each readable one with --flash-device <index|serial>"
		fi
		info "Flashing all $n attached Ubertooths"
		while read -r index serial; do
			info "  flashing $index: $serial"
			"$dfu" -U "$serial" -d "$img" -r </dev/null
		done <<< "$devices"
	elif [ -n "$FLASH_DEVICE" ]; then
		# Show the serial even when the user selected by index.
		if printf '%s' "$FLASH_DEVICE" | grep -qE '^[0-9]+$'; then
			sel_serial="$(printf '%s\n' "$devices" | awk -v i="$FLASH_DEVICE" '$1 == i { print $2 }')"
			if [ -n "$sel_serial" ] && [ "$sel_serial" != "?" ]; then
				info "Flashing bluetooth_rxtx to Ubertooth index $FLASH_DEVICE (serial $sel_serial)"
			else
				info "Flashing bluetooth_rxtx to Ubertooth index $FLASH_DEVICE"
			fi
		else
			info "Flashing bluetooth_rxtx to Ubertooth serial $FLASH_DEVICE"
		fi
		"$dfu" -U "$FLASH_DEVICE" -d "$img" -r
	else
		if [ "$n" -eq 0 ]; then
			die "no Ubertooth found to flash"
		elif [ "$n" -gt 1 ]; then
			die "$(multi_device_msg "$devices")"
		fi
		read -r index serial <<< "$devices"
		[ "$serial" != "?" ] || die "the attached Ubertooth could not be read; check it with ubertooth-util -s"
		info "Flashing bluetooth_rxtx to Ubertooth $serial"
		"$dfu" -U "$serial" -d "$img" -r
	fi
}

# --- 0. flash pre-flight (fail fast, BEFORE any build work) ---------------
# Resolve/validate the flash target up front so an ambiguous situation
# (multiple devices, no selector) aborts immediately instead of after a full
# build. Only possible if ubertooth-util is already installed.
if [ "$FLASH" -eq 1 ] && command -v ubertooth-util >/dev/null 2>&1; then
	if [ "$FLASH_DEVICE" != "all" ] && [ -z "$FLASH_DEVICE" ]; then
		_devices="$(list_devices "$(command -v ubertooth-util)")"
		_n="$(printf '%s\n' "$_devices" | grep -c . || true)"
		[ "$_n" -le 1 ] || die "$(multi_device_msg "$_devices")"
	fi
fi

# --- 1. firmware toolchain deps (only what flashing needs, installed early) -
if [ "$WITH_FIRMWARE" -eq 1 ] && [ "$SKIP_DEPS" -eq 0 ]; then
	if [ "$PLATFORM" = debian ]; then
		info "Installing firmware toolchain (apt)"
		as_root apt-get update -qq
		as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi dfu-util make
	else
		info "Installing firmware toolchain (Homebrew)"
		brew_install dfu-util
		# Any arm-none-eabi-gcc with a C library will do (Homebrew's formula has
		# none); installing the cask over an existing one would conflict.
		if ! printf '#include <string.h>\n' | \
				arm-none-eabi-gcc -x c -c - -o /dev/null >/dev/null 2>&1; then
			command -v arm-none-eabi-gcc >/dev/null 2>&1 && \
				die "arm-none-eabi-gcc cannot compile firmware (no C library?); install the gcc-arm-embedded cask instead"
			brew install --cask gcc-arm-embedded
		fi
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
# On Debian/Ubuntu python3 may only exist after the apt step above.
PYTHON="${PYTHON3:-$(command -v python3 || echo python3)}"
require_python "$PYTHON"
if [ -n "$VENV" ]; then
	info "Creating/using Python venv at $VENV"
	[ -d "$VENV" ] || "$PYTHON" -m venv "$VENV"
	# shellcheck disable=SC1091
	PYTHON="$VENV/bin/python"
	"$PYTHON" -m pip install --quiet --upgrade pip
fi
info "Installing Python runtime deps for specan_ui and ubtbr"
PIP_DEPS=(numpy qtpy pyside6)
PY_DEPS_WARNING=""
# Debian/Ubuntu gets the ubtbr USB binding from apt (python3-usb1).
[ "$PLATFORM" = macos ] && PIP_DEPS+=(libusb1==3.4.0)
if [ "$PLATFORM" = debian ]; then
	GUI_DEPS_HINT="install python3-numpy python3-qtpy python3-pyqt5 (apt) or use --venv"
else
	GUI_DEPS_HINT="re-run with --venv <path> (recommended on macOS)"
fi
if [ -n "$VENV" ]; then
	VENV_DEPS=("${PIP_DEPS[@]}")
	[ "$PLATFORM" = debian ] && VENV_DEPS+=(libusb1==3.4.0)
	if ! "$PYTHON" -m pip install --quiet "${VENV_DEPS[@]}"; then
		PY_DEPS_WARNING="could not pip-install Python runtime deps into the venv $VENV"
		info "WARNING: $PY_DEPS_WARNING"
	fi
elif ! "$PYTHON" -m pip install --quiet "${PIP_DEPS[@]}" 2>/dev/null; then
	# Homebrew's Python is externally managed and upgrades replace it, so do not
	# force packages into it on macOS; a venv is the supported route there.
	if [ "$PLATFORM" = debian ]; then
		"$PYTHON" -m pip install --quiet --break-system-packages "${PIP_DEPS[@]}" 2>/dev/null || \
			PY_DEPS_WARNING="could not pip-install GUI deps; $GUI_DEPS_HINT"
	else
		PY_DEPS_WARNING="could not pip-install GUI deps; $GUI_DEPS_HINT"
	fi
	[ -z "$PY_DEPS_WARNING" ] || info "WARNING: $PY_DEPS_WARNING"
fi

# --- 5. libbtbb -----------------------------------------------------------
info "Building libbtbb ($LIBBTBB_REF from $LIBBTBB_URL)"
git clone --quiet "$LIBBTBB_URL" "$WORK/libbtbb"
git -C "$WORK/libbtbb" checkout --quiet "$LIBBTBB_REF"
# ENABLE_PYTHON=OFF skips libbtbb's pcaptools: the Ubertooth tools link only
# the C library, and the Python tools would pip-install a package into PREFIX
# that nothing here uses.
cmake -S "$WORK/libbtbb" -B "$WORK/libbtbb/build" -DENABLE_PYTHON=OFF \
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
	CMAKE_ARGS+=("-DCMAKE_C_FLAGS=${CFLAGS:+$CFLAGS }-I$BREW_PREFIX/include")
fi
# Build the Python tools for the interpreter that got the runtime deps.
CMAKE_ARGS+=("-DPython3_EXECUTABLE=$(command -v "$PYTHON")")
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

if [ -n "$PY_DEPS_WARNING" ]; then
	info "Done, but the Python tools (ubertooth-specan-ui, ubtbr) will not run: $PY_DEPS_WARNING"
	info "Try: ubertooth-util -v"
else
	info "Done. Try: ubertooth-util -v   (and: ubertooth-specan-ui)"
fi
case ":$ORIG_PATH:" in
	*":$PREFIX/bin:"*) ;;
	*) info "Add $PREFIX/bin to your PATH to run the installed tools" ;;
esac
