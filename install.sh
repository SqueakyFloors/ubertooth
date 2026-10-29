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
#   ./install.sh --uninstall [--prefix <path>] [--keep-libbtbb|--remove-libbtbb]
#
# Options:
#   --uninstall       remove what an earlier run of this script installed into
#                     the prefix: the host tools, the libraries, the man pages
#                     and, on Debian/Ubuntu, the udev rule. The list comes from
#                     what that run recorded, so nothing else on the machine is
#                     touched: the apt or Homebrew packages, the Python
#                     packages and a venv made with --venv are all left alone.
#                     Safe to run twice.
#   --keep-libbtbb    with --uninstall: leave the libbtbb this script installed
#   --remove-libbtbb  with --uninstall: remove it as well, without asking.
#                     Anything else built against that libbtbb stops building
#                     until it is put back, so without either option a
#                     terminal is asked and a script keeps it.
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
# CFLAGS from the environment is passed to the host build, every run, so a
# re-run with it changed or cleared is built with what this run was given.
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
UNINSTALL=0
# "keep", "remove", or empty for "ask a terminal, keep otherwise".
LIBBTBB_ACTION=""
# Cleared when --libbtbb-url or --libbtbb-ref was given.
LIBBTBB_DEFAULTS=1
VENV=""
# libbtbb comes from this project's fork, pinned to a commit so a push there
# cannot change what this script builds. The pin is bumped in
# .github/workflows/build.yml, which checks that this copy agrees with it.
# --libbtbb-url / --libbtbb-ref override these for one run.
LIBBTBB_URL="https://github.com/SqueakyFloors/libbtbb.git"
LIBBTBB_REF="df77ac2623016787ae7daf5eabc6a0205a74e44a"
NO_SUDO=0
PREFIX=""
SKIP_DEPS=0
# Set by -h, answered after the whole command line has parsed (see below).
HELP=0

# A failure that does not end the run, one file of many: the "error: " line
# the output rules ask for, without info()'s progress prefix. Warnings carry
# the prefix those rules name as well.
err() { echo "error: $*" >&2; }
warn() { echo "warning: $*" >&2; }
die() { err "$*"; exit 1; }
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
# `ubertooth-util -s` in any of its output forms ("Serial No (<index>):
# <serial>" for several devices, "Device <index>: Serial No: <serial>" from
# older tools, or a single "Serial No: <serial>"); the device count comes from
# `ubertooth-util -N`, so a board whose serial is missing is reported instead
# of dropped. A board the listing could not read says "unavailable, <reason>"
# where its serial would be, which no expression here matches, so it keeps the
# "?". Exit codes are ignored on purpose.
list_devices() {
	local parsed count i serial
	parsed="$("$1" -s 2>&1 | sed -n \
		-e 's/^Serial No (\([0-9][0-9]*\)): \([0-9a-f]\{32\}\)$/\1 \2/p' -e t \
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

# Record the files an install step just put on the system, so --uninstall can
# take exactly those away again and nothing else. CMake writes
# install_manifest.txt into the build directory on every "cmake --install",
# one absolute path a line; the copy under the prefix is the one that survives,
# because libbtbb is built in a temporary directory. An optional description
# becomes a "# " comment on the first line, which the uninstall quotes back.
record_installed() { # <name> <build dir> [description]
	local name="$1" manifest="$2/install_manifest.txt"
	local dest="$RECORD_DIR/$name-files" src="$WORK/$name-files"
	if [ ! -f "$manifest" ]; then
		warn "$manifest is missing, so --uninstall will not know about the $name files"
		return 0
	fi
	# What an earlier run into this prefix installed is kept alongside what
	# this one did: a manifest names only the files its own build installed,
	# so a re-run configured with one tool fewer would otherwise leave that
	# tool behind with nothing recording it. A path whose file has since
	# gone costs the uninstall nothing; it counts as already gone.
	#
	# awk rather than cat, because CMake leaves no newline after the last
	# path of a manifest and a plain cat would join it to the next line.
	{
		[ -z "${3:-}" ] || printf '# %s\n' "$3"
		# LC_ALL=C, so that two paths differing only in a character
		# the locale's collation ignores are both kept.
		{ awk 1 "$manifest"; [ ! -f "$dest" ] || awk '!/^#/' "$dest"; } |
			LC_ALL=C sort -u
	} > "$src"
	as_installer mkdir -p "$RECORD_DIR"
	as_installer cp "$src" "$dest"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--with-firmware) WITH_FIRMWARE=1 ;;
		--flash) FLASH=1; WITH_FIRMWARE=1 ;;
		--flash-device) shift; FLASH_DEVICE="${1:-}"; FLASH=1; WITH_FIRMWARE=1; [ -n "$FLASH_DEVICE" ] || die "option '--flash-device' requires a value" ;;
		--uninstall) UNINSTALL=1 ;;
		--keep-libbtbb)
			[ "$LIBBTBB_ACTION" != remove ] || die "--keep-libbtbb cannot be combined with --remove-libbtbb"
			LIBBTBB_ACTION=keep ;;
		--remove-libbtbb)
			[ "$LIBBTBB_ACTION" != keep ] || die "--remove-libbtbb cannot be combined with --keep-libbtbb"
			LIBBTBB_ACTION=remove ;;
		--venv) shift; VENV="${1:-}"; [ -n "$VENV" ] || die "option '--venv' requires a value" ;;
		--libbtbb-url) shift; LIBBTBB_URL="${1:-}"; LIBBTBB_DEFAULTS=0; [ -n "$LIBBTBB_URL" ] || die "option '--libbtbb-url' requires a value" ;;
		--libbtbb-ref) shift; LIBBTBB_REF="${1:-}"; LIBBTBB_DEFAULTS=0; [ -n "$LIBBTBB_REF" ] || die "option '--libbtbb-ref' requires a value" ;;
		--prefix) shift; PREFIX="${1:-}"; [ -n "$PREFIX" ] || die "option '--prefix' requires a value" ;;
		--skip-deps) SKIP_DEPS=1 ;;
		--no-sudo) NO_SUDO=1 ;;
		-h|--help) HELP=1 ;;
		--)
			# A lone "--" ends the options and leaves nothing behind it, so
			# the run goes ahead with the defaults, as a command line with no
			# options does. This script takes no operands, so a word after
			# "--" is one it cannot use.
			#
			# "break" rather than falling out of the loop: "set -e" is on, and
			# the "shift" at the end of the body returns 1 once the argument
			# list is empty, which would end the run with no message at all.
			shift
			[ $# -eq 0 ] || die "unexpected argument '$1'"
			break ;;
		# The wording the tools share for a command line the option parser
		# will not take (.kiro/steering/cli-output.md). A word that does not
		# start with "-" is not an option the script is missing, so it is
		# reported as the stray word it is, and so is a lone "-", which is an
		# operand the tools spell the same way.
		-) die "unexpected argument '-'" ;;
		-*) die "unrecognized option '$1'" ;;
		*) die "unexpected argument '$1'" ;;
	esac
	shift
done

# -h answers and ends the run, but only once the option loop has read the
# whole command line, as it does for every tool here: an option the script
# does not know, a missing value or a stray word is reported even beside it.
# The combination checks below come after, so -h beside two options that
# cannot both run answers rather than refusing, which is what the tools do.
if [ "$HELP" -eq 1 ]; then
	sed -n '3,/^# Copyright/p' "$0" | sed '/^# Copyright/d; s/^#\{1,\} \{0,1\}//; s/^#$//'
	# Printed rather than written in the comment above, so the pin itself
	# lives in exactly one place in this script.
	printf 'libbtbb defaults:\n  --libbtbb-url %s\n  --libbtbb-ref %s\n' \
		"$LIBBTBB_URL" "$LIBBTBB_REF"
	exit 0
fi

# An uninstall builds and installs nothing, so a command line that asks for
# both says two things and means one. --prefix says where to look and
# --no-sudo prints the privileged steps instead of running them, so both apply
# to either job.
if [ "$UNINSTALL" -eq 1 ]; then
	[ "$WITH_FIRMWARE" -eq 0 ] || die "--uninstall cannot be combined with --with-firmware, --flash or --flash-device"
	[ -z "$VENV" ] || die "--uninstall cannot be combined with --venv"
	[ "$SKIP_DEPS" -eq 0 ] || die "--uninstall cannot be combined with --skip-deps"
	[ "$LIBBTBB_DEFAULTS" -eq 1 ] || die "--uninstall cannot be combined with --libbtbb-url or --libbtbb-ref; it removes the libbtbb that was installed"
elif [ -n "$LIBBTBB_ACTION" ]; then
	die "--keep-libbtbb and --remove-libbtbb only apply to --uninstall"
fi

# --- platform check -------------------------------------------------------
case "$(uname -s)" in
	Linux)
		command -v apt-get >/dev/null 2>&1 || \
			die "on Linux this script supports Debian/Ubuntu (apt-get) only; see REQUIREMENTS.md for other platforms"
		# python3 comes from apt below if it is missing; check it here if not,
		# so an old one stops the script before anything is installed or built.
		# An uninstall builds nothing and runs no Python, so it does not care.
		if [ "$UNINSTALL" -eq 0 ] && command -v python3 >/dev/null 2>&1; then
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
		# An uninstall builds nothing and runs no Python, so it does not care.
		if [ "$UNINSTALL" -eq 0 ]; then
			command -v python3 >/dev/null 2>&1 || \
				die "python3 not found; install Python 3.8 or newer (for example: brew install python)"
			require_python python3 "install a newer one (for example: brew install python)"
		fi
		PLATFORM=macos
		BREW_PREFIX="$(brew --prefix)"
		PREFIX="${PREFIX:-$BREW_PREFIX}"
		JOBS="$(sysctl -n hw.ncpu)"
		;;
	*)
		die "unsupported platform $(uname -s); this script supports Debian/Ubuntu and macOS (Homebrew); see REQUIREMENTS.md"
		;;
esac

# --- uninstall ------------------------------------------------------------

# True when a recorded path is one this script could have installed: under the
# prefix, or the udev rule in one of the three directories a system keeps udev
# rules in, which is the one file the host build writes outside the prefix. A
# ".." could walk back out of the prefix, so a path holding one is refused
# whatever else it looks like. So a record that was damaged or edited by hand
# cannot name an arbitrary file and have it removed.
#
# The comparison is on the path as written. A prefix with a symlinked
# subdirectory therefore reaches the link's target, which is the point: the
# install wrote the file through that same link, and resolving the path here
# would leave a prefix laid out that way with files and no way to remove them.
#
# A build pointed at a fourth udev directory with -DUDEV_RULES_PATH has its
# rule reported and left behind; the message says which file it is.
ours_to_remove() { # <recorded path>
	case "$1" in
		*/../*|*/..) return 1 ;;
		"$PREFIX"/*) return 0 ;;
		/etc/udev/rules.d/40-ubertooth.rules) return 0 ;;
		/lib/udev/rules.d/40-ubertooth.rules) return 0 ;;
		/usr/lib/udev/rules.d/40-ubertooth.rules) return 0 ;;
	esac
	return 1
}

# Remove the files one record lists, and nothing else. Sets REMOVED, PRINTED
# (removals --no-sudo printed instead of running), ABSENT, SKIPPED and FAILED.
remove_recorded() { # <record file>
	local record="$1" path
	REMOVED=0; PRINTED=0; ABSENT=0; SKIPPED=0; FAILED=0
	# The test on the right of the || reads a last line with no newline
	# after it, which is how CMake writes a manifest.
	while IFS= read -r path || [ -n "$path" ]; do
		case "$path" in ''|'#'*) continue ;; esac
		if ! ours_to_remove "$path"; then
			# An error, not a note: it is why the run ends non-zero.
			err "left $path: not a path this script installs"
			SKIPPED=$((SKIPPED+1)); continue
		fi
		# A symlink whose target has gone is still ours, hence -L.
		if [ ! -e "$path" ] && [ ! -L "$path" ]; then
			ABSENT=$((ABSENT+1)); continue
		fi
		if ! as_installer rm -f -- "$path"; then
			err "could not remove $path"
			FAILED=$((FAILED+1))
		elif [ -e "$path" ] || [ -L "$path" ]; then
			PRINTED=$((PRINTED+1)) # --no-sudo, so nothing ran
		else
			REMOVED=$((REMOVED+1))
		fi
	done < "$record"
}

# Remove the directory this script keeps its record in, and its parent, once
# they are empty; rmdir refuses one with anything else in it. Nothing else is
# pruned. A manifest lists files, not directories, so which of bin, lib,
# include and share an install created and which were already there cannot be
# told from it, and a stock /usr/local ships several of them empty: an empty
# directory left behind harms nothing, where removing someone else's does.
prune_record_dirs() {
	local dir
	for dir in "$RECORD_DIR" "$(dirname "$RECORD_DIR")"; do
		[ -d "$dir" ] || continue
		# Emptiness is tested here rather than left to rmdir, so that
		# --no-sudo does not print a removal that would have been
		# refused, and so a real rmdir failure is heard.
		[ -z "$(ls -A "$dir" 2>/dev/null)" ] || continue
		as_installer rmdir "$dir" || true
	done
}

# Take a record away once nothing it lists is left. One that still names a
# file, because the removal failed or because the path was refused, is kept:
# the point of the record is that a user is never left with files and no list.
drop_record() { # <record file>
	if [ "$FAILED" -eq 0 ] && [ "$SKIPPED" -eq 0 ]; then
		as_installer rm -f -- "$1"
	fi
}

# Take away what an earlier run installed into PREFIX, and nothing else: the
# record that run wrote is the only list used, so a file this script never
# installed is not named there and cannot be removed here. Nothing else it
# touched is a file of ours to take back: the apt and Homebrew packages, the
# Python packages, and a venv the user named with --venv, all stay.
#
# Running it twice is a no-op rather than an error, the way "rm -f" is: the
# state asked for has been reached either way, so only a file that is still
# there after we tried to remove it is a failure.
uninstall() {
	local host_record="$RECORD_DIR/host-files" btbb_record="$RECORD_DIR/libbtbb-files"
	local removed=0 printed=0 absent=0 skipped=0 failed=0 libbtbb_what="" answer

	if [ ! -f "$host_record" ] && [ ! -f "$btbb_record" ]; then
		info "Nothing to remove: no record of an install.sh install in $PREFIX"
		info "An install writes $RECORD_DIR; with no record there is no list to go on, so remove any such files by hand"
		return 0
	fi

	# The script installed this libbtbb, so it may remove it, but other
	# builds on the machine link against it and a user who only wanted the
	# tools gone would not expect that. So: asked, and kept by default.
	if [ -f "$btbb_record" ]; then
		libbtbb_what="$(sed -n 's/^# //p' "$btbb_record" | head -1)"
		info "This install includes ${libbtbb_what:-libbtbb}, in $PREFIX"
		info "Anything else on this machine built against it stops building until it is put back"
		case "$LIBBTBB_ACTION" in
			remove) ;;
			keep) info "Leaving it in place (--keep-libbtbb)" ;;
			*)
				if [ -t 0 ]; then
					printf 'Remove that libbtbb as well? [y/N] ' >&2
					read -r answer || answer=""
					case "$answer" in
						[yY]|[yY][eE][sS]) LIBBTBB_ACTION=remove ;;
						*) LIBBTBB_ACTION=keep; info "Leaving libbtbb in place" ;;
					esac
				else
					LIBBTBB_ACTION=keep
					info "Leaving libbtbb in place: no terminal to ask. Re-run with --remove-libbtbb to remove it"
				fi ;;
		esac
	fi

	if [ -f "$host_record" ]; then
		info "Removing the Ubertooth host tools recorded in $host_record"
		remove_recorded "$host_record"
		removed=$((removed+REMOVED)); printed=$((printed+PRINTED))
		absent=$((absent+ABSENT)); skipped=$((skipped+SKIPPED))
		failed=$((failed+FAILED))
		drop_record "$host_record"
	fi

	if [ "$LIBBTBB_ACTION" = remove ] && [ -f "$btbb_record" ]; then
		info "Removing the libbtbb recorded in $btbb_record"
		remove_recorded "$btbb_record"
		removed=$((removed+REMOVED)); printed=$((printed+PRINTED))
		absent=$((absent+ABSENT)); skipped=$((skipped+SKIPPED))
		failed=$((failed+FAILED))
		drop_record "$btbb_record"
	fi

	prune_record_dirs
	if [ "$PLATFORM" = debian ] && [ "$removed" -gt 0 ]; then
		as_root ldconfig
	fi

	info "Removed $removed file(s) of this install from $PREFIX"
	if [ "$printed" -gt 0 ]; then
		info "$printed file(s) are still there: --no-sudo printed the removals above instead of running them"
	fi
	if [ "$absent" -gt 0 ]; then
		info "$absent recorded file(s) were already gone"
	fi
	if [ "$LIBBTBB_ACTION" = keep ] && [ -f "$btbb_record" ]; then
		info "libbtbb is still installed in $PREFIX"
	fi
	info "Left alone: the apt or Homebrew packages, the Python packages (numpy, qtpy, pyside6, libusb1), and any venv made with --venv"
	if [ "$((failed+skipped))" -gt 0 ]; then
		die "$((failed+skipped)) recorded path(s) are still there: $failed could not be removed, $skipped not this script's to remove; $RECORD_DIR still lists them"
	fi
}

ORIG_PATH="$PATH"
# Resolve python3 from the user's PATH, before PREFIX/bin is put first.
PYTHON3="$(command -v python3 || true)"
# Run paths and PATH need an absolute prefix. Create it (as root if needed) and
# resolve symlinks so it can be compared with the Homebrew prefix, and with the
# paths an earlier run recorded.
case "$PREFIX" in /*) ;; *) PREFIX="$PWD/$PREFIX" ;; esac
if [ "$UNINSTALL" -eq 0 ]; then
	# An uninstall must not create the prefix it is about to look in.
	mkdir -p "$PREFIX" 2>/dev/null || as_installer mkdir -p "$PREFIX"
fi
if [ -d "$PREFIX" ]; then
	PREFIX="$(cd "$PREFIX" && pwd -P)"
fi
# Where an install records what it put on the system, one absolute path a line,
# for --uninstall to take away again.
RECORD_DIR="$PREFIX/share/ubertooth/install-record"

if [ "$UNINSTALL" -eq 1 ]; then
	uninstall
	exit 0
fi

if [ "$PLATFORM" = macos ] && [ "$PREFIX" = "$(cd "$BREW_PREFIX" && pwd -P)" ]; then
	# Homebrew core has its own ubertooth and libbtbb; installing over them in
	# the Homebrew prefix would break both. Each "brew list" takes about a
	# second, so say what is happening: this is the script's first slow step.
	info "Checking Homebrew for an ubertooth or libbtbb that would conflict"
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

	# Reached with any selector, --flash-device <serial> included, and here
	# too a device that is busy or wedged makes each read wait.
	info "Looking for the Ubertooth to flash"
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
		# list_devices runs ubertooth-util twice (-s and -N), and a device
		# that is busy or wedged makes each read wait, so say so first.
		info "Checking which Ubertooth to flash"
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
		warn "$PY_DEPS_WARNING"
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
	[ -z "$PY_DEPS_WARNING" ] || warn "$PY_DEPS_WARNING"
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
record_installed libbtbb "$WORK/libbtbb/build" "libbtbb $LIBBTBB_REF from $LIBBTBB_URL"
[ "$PLATFORM" = debian ] && as_root ldconfig

# Which directory under PREFIX libbtbb put its library in. That is the one the
# system keeps libraries in: lib64 on a 64-bit Fedora, RHEL or SUSE, a
# multiarch lib/<triplet> where the prefix is /usr on Debian or Ubuntu, and lib
# on macOS and for other prefixes. So it is read from the install rather than
# assumed, and whatever libbtbb --libbtbb-ref built is answered for.
# libbtbb.pc goes in a pkgconfig directory inside it, and the install manifest
# names every file the install wrote.
BTBB_LIBDIR="$(sed -n 's#/pkgconfig/libbtbb\.pc$##p' \
	"$WORK/libbtbb/build/install_manifest.txt" 2>/dev/null | head -n 1)"
if [ -z "$BTBB_LIBDIR" ]; then
	BTBB_LIBDIR="$PREFIX/lib"
	warn "the libbtbb install did not record a libbtbb.pc, so the tools are built for a libbtbb in $BTBB_LIBDIR"
fi

# --- 6. host tools --------------------------------------------------------
# The installed tools find libubertooth and libbtbb under PREFIX through their
# run path, so a non-default prefix works without ldconfig or DYLD settings.
# Two directories where libbtbb's is not libubertooth's.
RPATH="$PREFIX/lib"
[ "$BTBB_LIBDIR" = "$PREFIX/lib" ] || RPATH="$RPATH;$BTBB_LIBDIR"
CMAKE_ARGS=(
	"-DCMAKE_INSTALL_PREFIX=$PREFIX"
	"-DCMAKE_PREFIX_PATH=$PREFIX"
	"-DCMAKE_INSTALL_RPATH=$RPATH"
)
# The configure step asks pkg-config for libbtbb, which answers for the
# install just made - its directories, not a guess at them - so a libbtbb in
# lib64 or a multiarch directory is found there like any other. An existing
# setting is kept after it, so a libbtbb the user pointed at stays reachable
# while this one is looked at first.
export PKG_CONFIG_PATH="$BTBB_LIBDIR/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [ "$PLATFORM" = debian ]; then
	info "Building Ubertooth host tools (REQUIRE_ALL=ON)"
	CMAKE_ARGS+=(-DREQUIRE_ALL=ON)
else
	# No BlueZ on macOS, so REQUIRE_ALL is not used. Nothing to add for
	# Homebrew's libusb headers either: the build takes their directory from
	# libusb itself.
	info "Building Ubertooth host tools"
fi
# Always passed, empty when the caller set no CFLAGS: CMake seeds its own C
# flags from CFLAGS only when a build directory is configured for the first
# time, and this script reuses one, so a run that left this out would build
# with whatever an earlier run's CFLAGS put in the cache.
CMAKE_ARGS+=("-DCMAKE_C_FLAGS=${CFLAGS:-}")
# Build the Python tools for the interpreter that got the runtime deps.
CMAKE_ARGS+=("-DPython3_EXECUTABLE=$(command -v "$PYTHON")")
# Forget a libbtbb or a libusb found by an earlier configure so this run's
# install is used. libusb for the same reason the build no longer passes an
# include flag for it: its directories come from the configure step now, and a
# cached one from before a brew upgrade names a version that is gone.
cmake -S "$REPO_ROOT/host" -B "$REPO_ROOT/host/build" \
	-U 'LIBBTBB_*' -U 'LIBUSB_*' "${CMAKE_ARGS[@]}"
cmake --build "$REPO_ROOT/host/build" -j"$JOBS"
as_installer cmake --install "$REPO_ROOT/host/build"
record_installed host "$REPO_ROOT/host/build"
if [ "$PLATFORM" = debian ]; then
	as_root ldconfig
	info "udev rules installed with the host tools (unplug/replug the Ubertooth)"
fi

# --- 7. firmware flash (deferred case: fresh install built tools just now) -
if [ "$WITH_FIRMWARE" -eq 1 ] && [ "$FLASHED" -eq 0 ]; then
	flash_firmware
fi

# --- 8. say what was installed and which libbtbb it uses -------------------
# A machine can have a distribution or Homebrew libbtbb beside this one, and
# which one a tool loads is decided by its link path, not by what was built
# last. So the tools just installed are asked, rather than this script
# repeating what it built: an answer naming a libbtbb outside $PREFIX is the
# one thing the user has to see here.
#
# Reported, not enforced: the install has already succeeded at this point, and
# a mixed machine is something to tell the user about rather than a reason to
# fail. A build of the host tools against the wrong libbtbb is refused earlier,
# by the configure step.
if [ ! -x "$PREFIX/bin/ubertooth-util" ]; then
	# --no-sudo prints the steps that need root instead of running them, so
	# there is nothing installed to ask.
	info "Not checking which libbtbb is in use: $PREFIX/bin/ubertooth-util is not installed"
else
	# -V answers the host part from the tool itself, so no Ubertooth need be
	# attached. It exits 1 when a board is attached and cannot be read, and
	# prints the host lines all the same, so what it printed is what decides
	# here and its exit status is not looked at. Its standard error is left
	# alone rather than discarded: a tool that will not run has said why
	# there, and that is the thing to show.
	#
	# Only the host lines are kept. The rest of the answer describes the
	# boards attached, which this section is not about, and a board the
	# freshly installed udev rule does not cover yet would report a denial
	# here that reads like a failed install.
	BTBB_IN_USE="$("$PREFIX/bin/ubertooth-util" -V || true)"
	BTBB_IN_USE="$(printf '%s\n' "$BTBB_IN_USE" | sed -n '/^Firmware /q;p')"
	case "$BTBB_IN_USE" in
		*"libbtbb loaded from: "*) UTIL_ANSWERED=1 ;;
		*) UTIL_ANSWERED=0 ;;
	esac
	if [ "$UTIL_ANSWERED" -eq 0 ]; then
		warn "the installed ubertooth-util would not run, so which libbtbb it uses is unknown"
	else
		# The lines below are the tool's own, quoted as it printed
		# them rather than given the script's "==> " prefix.
		info "What the installed tools report:"
		printf '%s\n' "$BTBB_IN_USE" >&2
		case "$BTBB_IN_USE" in
			*"libbtbb loaded from: $PREFIX/"*) ;;
			# The loader would not say, so there is nothing to
			# compare: say that rather than report a libbtbb
			# somewhere else.
			*"libbtbb loaded from: unknown"*)
				info "The tools cannot say which libbtbb they loaded, so another install would not be noticed here" ;;
			# The tool names itself, which a libbtbb linked into it
			# and a build without position-independent code both
			# do. Also not an answer about a second install.
			*"libbtbb loaded from: $PREFIX/bin/"*)
				info "The tools report themselves as the libbtbb in use, so another install would not be noticed here" ;;
			*) warn "the tools load a libbtbb from outside $PREFIX; remove the other install if packets decode oddly" ;;
		esac
	fi
fi

if [ -n "$PY_DEPS_WARNING" ]; then
	info "Done, but the Python tools (ubertooth-specan-ui, ubtbr) will not run: $PY_DEPS_WARNING"
	info "Try: ubertooth-util -V"
else
	info "Done. Try: ubertooth-util -V   (and: ubertooth-specan-ui)"
fi
case ":$ORIG_PATH:" in
	*":$PREFIX/bin:"*) ;;
	*) info "Add $PREFIX/bin to your PATH to run the installed tools" ;;
esac
