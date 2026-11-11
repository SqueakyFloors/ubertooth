#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# install.sh - one-command from-source setup for the Ubertooth host tools
#              (and optionally firmware) on Debian/Ubuntu, on Fedora, and on
#              macOS with Homebrew.
#
# Builds and installs: system dependencies, libbtbb, the Ubertooth host tools,
# and the Python runtime deps for specan_ui. On Linux it also installs udev
# rules and builds with -DREQUIRE_ALL=ON so a missing optional dep fails
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
#                     and, on Linux, the udev rule. The list comes from what
#                     that run recorded, so nothing else on the machine is
#                     touched: the distribution or Homebrew packages, the
#                     Python packages and a venv made with --venv are all left
#                     alone. Safe to run twice.
#   --keep-libbtbb    with --uninstall: leave the libbtbb this script installed
#   --remove-libbtbb  with --uninstall: remove it as well, without asking.
#                     Anything else built against that libbtbb stops building
#                     until it is put back, so without either option a
#                     terminal is asked and a script keeps it.
#   --with-firmware   also build the firmware images (needs arm-none-eabi-gcc)
#   --flash-device <index|serial|all>
#                     build and flash bluetooth_rxtx to an attached Ubertooth
#                     (implies --with-firmware; STATE-CHANGING). With one board
#                     attached the selector is optional; with several, use an
#                     index, serial, or "all". --flash is an alias.
#   --update-bootloader
#                     also install the bootloader this tree builds
#                     (implies --flash-device and --with-firmware;
#                     STATE-CHANGING).
#                     With one board attached, --flash-device is optional;
#                     with several, it is required. A board that already has
#                     that bootloader is left alone.
#                     Boards are done one at a time, each finished and read
#                     back before the next, and bluetooth_rxtx goes back on
#                     after each. Keep a serial programmer to hand: an
#                     interrupted write leaves a board needing one. See
#                     ubertooth-dfu(1), UPDATING THE BOOTLOADER.
#   --venv <path>     create/use a Python venv at <path> for the Python tools
#                     and point the build at its interpreter
#   --libbtbb-url <u> git URL to clone libbtbb from
#   --libbtbb-ref <r> git commit, tag or branch of libbtbb to build. These two
#                     default to a pinned commit of this project's fork (both
#                     defaults are printed below). A libbtbb without the entry
#                     points the tools call, upstream's included, is refused
#                     when the tools are configured, naming what it is
#                     missing.
#   --prefix <path>   install prefix (default: /usr/local on Linux, the
#                     Homebrew prefix on macOS)
#   --skip-deps       do not install system packages; assume they are present
#   --force           rebuild and reinstall even if already up to date
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

set -euo pipefail

WITH_FIRMWARE=0
FLASH=0
FLASH_DEVICE=""
UPDATE_BOOTLOADER=0
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
LIBBTBB_REF="d356e9da7fb03969371aa14d603a8aad2f0ef198"
NO_SUDO=0
PREFIX=""
SKIP_DEPS=0
FORCE=0
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

# The version string this tree would build, in the format ubertooth-util -V
# reports: TAG+AHEAD (e.g. 2026-11-DEV2+11). Empty if git cannot answer.
tree_version() {
	local desc
	desc="$(git -C "$REPO_ROOT" describe --tags \
		--match '[0-9]*-[0-9]*-R[0-9]*' \
		--match '[0-9]*-[0-9]*-RC[0-9]*' \
		--match '[0-9]*-[0-9]*-DEV[0-9]*' \
		HEAD 2>/dev/null)" || return 1
	# git describe: TAG-AHEAD-gHASH → TAG+AHEAD
	printf '%s' "$desc" | sed 's/-\([0-9]*\)-g[0-9a-f]*$/+\1/'
}

# The host tools version the installed ubertooth-util reports, or empty.
# The trailing * (dirty flag) is stripped: it means the build had uncommitted
# changes, but the commit distance is the same.
installed_host_version() {
	local line
	line="$("$1" -V 2>/dev/null | head -1)" || return 1
	# "Host tools version: 2026-11-DEV2+11* (libbtbb ...)" → 2026-11-DEV2+11
	printf '%s' "$line" | sed -n 's/^Host tools version: \([^ ]*\).*/\1/p' | sed 's/\*$//'
}

# The libbtbb ref recorded by the last install into this prefix, or empty.
installed_libbtbb_ref() {
	local record="$1"
	[ -f "$record" ] || return 1
	# "# libbtbb <ref> from <url>" → <ref>
	sed -n 's/^# libbtbb \([^ ]*\) from .*/\1/p' "$record" | head -1
}

# The firmware version a specific board reports, or empty.
# The trailing * (dirty flag) is stripped, as in installed_host_version.
installed_fw_version() {
	local line
	line="$("$1" -U "$2" -v 2>/dev/null)" || return 1
	# "Firmware version: bluetooth_rxtx 2026-11-DEV2+11* (API:...)" → 2026-11-DEV2+11
	printf '%s' "$line" | sed -n 's/^Firmware version: [^ ]* \([^ ]*\).*/\1/p' | sed 's/\*$//'
}

# The Python tools need Python 3.8 or newer (host/python/cmake/UbertoothPythonFloor.cmake).
require_python() {
	local version
	"$1" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null && return 0
	# Python before 3.4 prints -V on stderr.
	version="$("$1" -V 2>&1)" || version="not runnable"
	die "Python 3.8 or newer is required for the Python tools (found $1: $version)${2:+; $2}"
}

# True on the Linux distributions this script installs on. They differ in
# which package manager installs the dependencies and in what the packages are
# called, and in nothing else: udev rules, ldconfig, a system Python that pip
# must not be forced into, and a BlueZ to build ubertooth-scan and
# ubertooth-follow against are common to all of them, and macOS has none of
# those. So the steps that are about Linux rather than about apt ask this, and
# only the two dependency steps look at which distribution it is.
on_linux() { [ "$PLATFORM" != macos ]; }

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
		--flash|--flash-device)
			FLASH=1; WITH_FIRMWARE=1
			# Optional argument: the next word, if it doesn't start with --.
			if [ $# -gt 1 ] && case "$2" in --*) false ;; *) true ;; esac; then
				shift; FLASH_DEVICE="$1"
			fi
			;;
		--update-bootloader) UPDATE_BOOTLOADER=1; FLASH=1; WITH_FIRMWARE=1 ;;
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
		--force) FORCE=1 ;;
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
	# The help is this file's leading comment block, so the text a user
	# reads cannot drift from the text in the source. The block starts
	# below the shebang and the license line, and ends at the first line
	# that is not a comment, which is where sed quits.
	sed -n '3,${/^#/!q;p;}' "$0" | sed 's/^#\{1,\} \{0,1\}//'
	# Printed rather than written in the comment above, so the pin itself
	# lives in exactly one place in this script.
	printf '\nlibbtbb defaults:\n  --libbtbb-url %s\n  --libbtbb-ref %s\n' \
		"$LIBBTBB_URL" "$LIBBTBB_REF"
	exit 0
fi

# An uninstall builds and installs nothing, so a command line that asks for
# both says two things and means one. --prefix says where to look and
# --no-sudo prints the privileged steps instead of running them, so both apply
# to either job.
if [ "$UNINSTALL" -eq 1 ]; then
	[ "$WITH_FIRMWARE" -eq 0 ] || die "--uninstall cannot be combined with --with-firmware, --flash-device, or --update-bootloader"
	[ -z "$VENV" ] || die "--uninstall cannot be combined with --venv"
	[ "$SKIP_DEPS" -eq 0 ] || die "--uninstall cannot be combined with --skip-deps"
	[ "$LIBBTBB_DEFAULTS" -eq 1 ] || die "--uninstall cannot be combined with --libbtbb-url or --libbtbb-ref; it removes the libbtbb that was installed"
elif [ -n "$LIBBTBB_ACTION" ]; then
	die "--keep-libbtbb and --remove-libbtbb only apply to --uninstall"
fi

# Installing a bootloader costs the board its application region and leaves it
# in DFU, so the board it happens to has to be named rather than found: a run
# with one board attached today and two tomorrow would otherwise write a
# different board each time.
if [ "$UPDATE_BOOTLOADER" -eq 1 ] && [ -z "$FLASH_DEVICE" ]; then
	# With one board attached, resolve it the same way --flash-device does
	# rather than forcing the user to name the sole device.
	if command -v ubertooth-util >/dev/null 2>&1; then
		_bl_devices="$(list_devices "$(command -v ubertooth-util)")"
		_bl_n="$(printf '%s\n' "$_bl_devices" | grep -c . || true)"
		if [ "$_bl_n" -eq 1 ]; then
			read -r _bl_idx _bl_serial <<< "$_bl_devices"
			if [ "$_bl_serial" != "?" ]; then
				FLASH_DEVICE="$_bl_serial"
			else
				FLASH_DEVICE="$_bl_idx"
			fi
		else
			die "--update-bootloader needs --flash-device <index|serial|all> when more than one board is attached"
		fi
	else
		die "--update-bootloader needs --flash-device <index|serial|all> (ubertooth-util not yet installed to auto-detect)"
	fi
fi

# --- platform check -------------------------------------------------------
case "$(uname -s)" in
	Linux)
		# Which package manager is here decides the package names below.
		# apt-get is looked for first, so a Debian or Ubuntu machine that also
		# has dnf installed is treated as the Debian machine it is.
		if command -v apt-get >/dev/null 2>&1; then
			PLATFORM=debian
		elif command -v dnf >/dev/null 2>&1; then
			PLATFORM=fedora
		else
			die "on Linux this script supports Debian/Ubuntu (apt-get) and Fedora (dnf) only; see REQUIREMENTS.md for other platforms"
		fi
		# python3 comes from the package manager below if it is missing; check
		# it here if not, so an old one stops the script before anything is
		# installed or built. An uninstall builds nothing and runs no Python,
		# so it does not care.
		if [ "$UNINSTALL" -eq 0 ] && command -v python3 >/dev/null 2>&1; then
			require_python python3
		fi
		PREFIX="${PREFIX:-/usr/local}"
		JOBS="$(nproc)"
		;;
	Darwin)
		command -v brew >/dev/null 2>&1 || \
			die "on macOS this script needs Homebrew (https://brew.sh); see REQUIREMENTS.md"
		# Homebrew does not install Python here; the Linux package
		# managers do.
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
		die "unsupported platform $(uname -s); this script supports Debian/Ubuntu, Fedora and macOS (Homebrew); see REQUIREMENTS.md"
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
# touched is a file of ours to take back: the distribution and Homebrew
# packages, the Python packages, and a venv the user named with --venv, all
# stay.
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
	if on_linux && [ "$removed" -gt 0 ]; then
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
	info "Left alone: the distribution or Homebrew packages, the Python packages (numpy, qtpy, pyside6, libusb1), and any venv made with --venv"
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

# Wait for a board to answer again after a reset. ubertooth-dfu -r has already
# seen the board come back and read the image's own revision off it, so this is
# the shorter wait for the read that follows: a board that has just
# re-enumerated can take a moment to be reachable by serial.
wait_for_board() { # <ubertooth-util> <serial>
	local waited=0
	while [ "$waited" -lt 15 ]; do
		"$1" -U "$2" -v >/dev/null 2>&1 && return 0
		sleep 1
		waited=$((waited+1))
	done
	return 1
}

# True when some board is waiting in the DFU bootloader.
#
# Such a board is not in list_devices' listing: it reports no serial number and
# no index counts it. ubertooth-util names it in a note when a selection
# matches nothing, which is what this asks for -- "zz" is not hex, so no
# serial can start with it. The refusal that comes with the note is expected
# and is not shown.
dfu_board_waiting() { # <ubertooth-util>
	local answer
	answer="$("$1" -U zz -s 2>&1 || true)"
	case "$answer" in
		*"in the DFU bootloader"*) return 0 ;;
	esac
	return 1
}

# Install the bootloader on one board, put bluetooth_rxtx back, and read off
# the board what it ended up with. One board start to finish, so a run over
# several boards finishes each before it touches the next.
#
# ubertooth-dfu decides whether to write: a board that already has this
# bootloader is left alone, still running its firmware, and one that is written
# is left in DFU with the updater in its application region. So the firmware
# goes back on either way. A written board is reached with no -U, because the
# bootloader reports no serial for -U to name, and ubertooth-dfu with no
# selector takes the one board waiting there -- which is why a board already in
# DFU stops the run before any of this (see dfu_board_waiting above).
#
# --force is never passed, and this script does not offer it: it writes without
# asking the board which bootloader it has, which costs the application region
# for a bootloader that may already be there.
update_bootloader_on() { # <dfu> <util> <bl_update.dfu> <firmware.dfu> <serial>
	local dfu="$1" util="$2" blimg="$3" img="$4" serial="$5"
	local answer="$WORK/bl-update.out" outcome bootloader

	# Run the update quietly; only announce if something is actually written.
	if ! "$dfu" --update-bootloader -d "$blimg" -U "$serial" </dev/null > "$answer" 2>&1; then
		info "Installing the bootloader on $serial"
		info "  keep a serial programmer to hand: a write that is interrupted leaves the board needing one"
		cat "$answer" >&2
		err "installing the bootloader on $serial failed"
		die "the firmware was not put back on this board; see ubertooth-dfu(1), UPDATING THE BOOTLOADER for what an interrupted write leaves behind and how a serial programmer recovers it"
	fi
	if grep -q 'already has this bootloader' "$answer"; then
		outcome="already had this bootloader, nothing written"
		# The board still runs its firmware — only reflash if the version
		# is different (or --force).
		local _tree_ver _fw_ver
		_tree_ver="$(tree_version 2>/dev/null || true)"
		_fw_ver="$(installed_fw_version "$util" "$serial" 2>/dev/null || true)"
		if [ "$FORCE" -eq 0 ] && [ -n "$_tree_ver" ] && [ "$_fw_ver" = "$_tree_ver" ]; then
			info "  $serial firmware already at $_tree_ver; skipping"
		else
			info "  $serial still runs its firmware; flashing bluetooth_rxtx"
			"$dfu" -U "$serial" -d "$img" -r </dev/null >&2 ||
				die "the bootloader on $serial was left as it was, and flashing the firmware failed"
		fi
	else
		# The bootloader was actually written — show the announcement and DFU output.
		info "Installing the bootloader on $serial"
		info "  keep a serial programmer to hand: a write that is interrupted leaves the board needing one"
		cat "$answer" >&2
		outcome="bootloader installed"
		info "  $serial is in DFU with the updater in its application region; flashing bluetooth_rxtx back"
		# A failure here leaves the board in DFU holding the updater, which
		# the same command puts right once whatever stopped it is dealt with.
		"$dfu" -d "$img" -r </dev/null >&2 ||
			die "the bootloader is installed on $serial, but the firmware did not go back on: the board is in the DFU bootloader holding the updater, and 'ubertooth-dfu -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r' with no -U is what reaches it"
	fi
	# Ends the run rather than warning: a board that has not come back is one
	# this script can no longer tell from a board left in DFU, and the next
	# board's reflash takes whichever single board is waiting there. Two of
	# them and ubertooth-dfu refuses to choose, which would cost that board
	# its firmware as well.
	wait_for_board "$util" "$serial" ||
		die "$serial has not answered since the flash, so no further board is touched; check it with ubertooth-util -U $serial -V, and if it is in the DFU bootloader, 'ubertooth-dfu -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r' with no -U reaches it"
	# Read off the board after a real write; skip the readback when nothing
	# was written — the board is unchanged and the output is just noise.
	if [ "$outcome" = "already had this bootloader, nothing written" ]; then
		bootloader="$("$util" -U "$serial" -B 2>/dev/null || true)"
		case "$bootloader" in
			*Bootloader:*)
				info "Board $serial: $outcome"
				info "  $bootloader" ;;
		esac
		return 0
	fi
	"$util" -U "$serial" -V >&2 || warn "$serial did not answer ubertooth-util -V"
	# Taken from what it printed, not from its status: -B exits 1 when part of
	# its answer is missing and still prints the hash, which is the line to
	# show rather than throw away.
	bootloader="$("$util" -U "$serial" -B || true)"
	case "$bootloader" in
		*Bootloader:*)
				info ""
			info "Board $serial: $outcome"
			info "  $bootloader" ;;
		*) warn "$serial: $outcome, but ubertooth-util -B would not say which bootloader it has" ;;
	esac
}

# Build the firmware and flash the selected device(s). Uses whichever
# ubertooth-dfu / ubertooth-util is on PATH (an existing install).
flash_firmware() {
	local dfu util img blimg devices n sel_serial index serial
	dfu="$(command -v ubertooth-dfu || true)"
	util="$(command -v ubertooth-util || true)"
	img="$REPO_ROOT/firmware/bluetooth_rxtx/bluetooth_rxtx.dfu"
	blimg="$REPO_ROOT/firmware/bl_update/bl_update.dfu"
	if [ -z "$dfu" ] || [ -z "$util" ]; then
		die "ubertooth-dfu/ubertooth-util not found on PATH; build the host tools first"
	fi

	# The version the tree would build.
	local tree_ver
	tree_ver="$(tree_version 2>/dev/null || true)"

	# Check whether a single device needs anything before building.
	# Returns 0 (needs work) or 1 (all up to date).
	device_needs_work() { # <util> <selector>
		if [ "$FORCE" -eq 1 ] || [ -z "$tree_ver" ]; then
			return 0
		fi
		local fw_ver
		fw_ver="$(installed_fw_version "$1" "$2" 2>/dev/null || true)"
		[ "$fw_ver" != "$tree_ver" ]
	}

	if [ "$FLASH" -ne 1 ]; then
		# --with-firmware without --flash: just build.
		if ! PATH="$(dirname "$dfu"):$PATH" make -C "$REPO_ROOT/firmware/bluetooth_rxtx" -s -q 2>/dev/null; then
			info "Building firmware (bluetooth_rxtx)"
			if ! PATH="$(dirname "$dfu"):$PATH" make -C "$REPO_ROOT/firmware/bluetooth_rxtx" -s >/dev/null 2>"$WORK/fw-build.err"; then
				cat "$WORK/fw-build.err" >&2
				die "firmware build failed"
			fi
		fi
		if [ "$UPDATE_BOOTLOADER" -eq 1 ]; then
			if ! PATH="$(dirname "$dfu"):$PATH" make -C "$REPO_ROOT/firmware/bl_update" -s -q 2>/dev/null; then
				info "Building firmware (bl_update, the bootloader updater)"
				if ! PATH="$(dirname "$dfu"):$PATH" make -C "$REPO_ROOT/firmware/bl_update" -s >/dev/null 2>"$WORK/bl-build.err"; then
					cat "$WORK/bl-build.err" >&2
					die "bootloader updater build failed"
				fi
			fi
		fi
		return 0
	fi

	# --- Resolve which devices to flash and check if any need work ---
	devices="$(list_devices "$util")"
	n="$(printf '%s\n' "$devices" | grep -c . || true)"
	if [ "$UPDATE_BOOTLOADER" -eq 1 ] && dfu_board_waiting "$util"; then
		die "a board is waiting in the DFU bootloader; finish or recover that flash first"
	fi

	# Build the list of serials to flash and check versions up front.
	local targets=() need_flash=0 need_bootloader=0
	if [ "$FLASH_DEVICE" = "all" ]; then
		[ "$n" -gt 0 ] || die "no Ubertooth found to flash"
		if printf '%s\n' "$devices" | grep -q ' ?$'; then
			die "$(multi_device_msg "$devices")"$'\n'"a device could not be read; flash each readable one with --flash-device <index|serial>"
		fi
		while read -r index serial; do
			targets+=("$serial")
			if device_needs_work "$util" "$serial"; then
				need_flash=1
			fi
		done <<< "$devices"
	elif [ -n "$FLASH_DEVICE" ]; then
		sel_serial="$FLASH_DEVICE"
		if printf '%s' "$FLASH_DEVICE" | grep -qE '^[0-9]+$'; then
			sel_serial="$(printf '%s\n' "$devices" | awk -v i="$FLASH_DEVICE" '$1 == i { print $2 }')"
			[ -n "$sel_serial" ] || sel_serial="?"
		fi
		if [ "$UPDATE_BOOTLOADER" -eq 1 ]; then
			[ "$sel_serial" != "?" ] || die "the serial of Ubertooth index $FLASH_DEVICE could not be read, and --update-bootloader needs it"
		fi
		targets+=("$FLASH_DEVICE")
		if device_needs_work "$util" "$FLASH_DEVICE"; then
			need_flash=1
		fi
	else
		[ "$n" -gt 0 ] || die "no Ubertooth found to flash"
		[ "$n" -le 1 ] || die "$(multi_device_msg "$devices")"
		read -r index serial <<< "$devices"
		[ "$serial" != "?" ] || die "the attached Ubertooth could not be read"
		targets+=("$serial")
		if device_needs_work "$util" "$serial"; then
			need_flash=1
		fi
	fi

	# The bootloader check is cheap (one USB read), so always let it decide.
	[ "$UPDATE_BOOTLOADER" -eq 1 ] && need_bootloader=1

	# --- Nothing to do? Say so and return. ---
	if [ "$need_flash" -eq 0 ] && [ "$need_bootloader" -eq 0 ]; then
		info "All devices already at $tree_ver; nothing to flash (use --force to reflash)"
		return 0
	fi

	# --- Build only what is needed ---
	# The .dfu file may already exist from a previous build at this version.
	if [ "$need_flash" -eq 1 ] || [ ! -f "$img" ]; then
		info "Building firmware (bluetooth_rxtx)"
		if ! PATH="$(dirname "$dfu"):$PATH" make -C "$REPO_ROOT/firmware/bluetooth_rxtx" -s >/dev/null 2>"$WORK/fw-build.err"; then
			cat "$WORK/fw-build.err" >&2
			die "firmware build failed"
		fi
	fi
	if [ "$need_bootloader" -eq 1 ] && [ ! -f "$blimg" ]; then
		info "Building firmware (bl_update, the bootloader updater)"
		if ! PATH="$(dirname "$dfu"):$PATH" make -C "$REPO_ROOT/firmware/bl_update" -s >/dev/null 2>"$WORK/bl-build.err"; then
			cat "$WORK/bl-build.err" >&2
			die "bootloader updater build failed"
		fi
	fi

	# --- Flash each target ---
	for serial in "${targets[@]}"; do
		if [ "$UPDATE_BOOTLOADER" -eq 1 ]; then
			update_bootloader_on "$dfu" "$util" "$blimg" "$img" "$serial"
		elif device_needs_work "$util" "$serial"; then
			info "Flashing bluetooth_rxtx to Ubertooth $serial"
			"$dfu" -U "$serial" -d "$img" -r </dev/null
		else
			info "Ubertooth $serial already at $tree_ver; skipping"
		fi
	done
}

# =========================================================================
# Step functions — each is one phase of the install, called from main below.
# =========================================================================

step_flash_preflight() {
	# Resolve/validate the flash target up front so an ambiguous situation
	# (multiple devices, no selector) aborts immediately instead of after a
	# full build. Only possible if ubertooth-util is already installed.
	[ "$FLASH" -eq 1 ] && command -v ubertooth-util >/dev/null 2>&1 || return 0
	if [ "$FLASH_DEVICE" != "all" ] && [ -z "$FLASH_DEVICE" ]; then
		info "Checking which Ubertooth to flash"
		local devices n
		devices="$(list_devices "$(command -v ubertooth-util)")"
		n="$(printf '%s\n' "$devices" | grep -c . || true)"
		[ "$n" -le 1 ] || die "$(multi_device_msg "$devices")"
	fi
}

step_firmware_toolchain_deps() {
	[ "$WITH_FIRMWARE" -eq 1 ] && [ "$SKIP_DEPS" -eq 0 ] || return 0

	# --update-bootloader needs the ARM C++ runtime for the bootloader build.
	local bl_packages_debian="" bl_packages_fedora=""
	if [ "$UPDATE_BOOTLOADER" -eq 1 ]; then
		bl_packages_debian="libstdc++-arm-none-eabi-newlib"
		bl_packages_fedora="arm-none-eabi-gcc-cs-c++"
	fi

	if [ "$PLATFORM" = debian ]; then
		info "Installing firmware toolchain (apt)"
		as_root apt-get update -qq
		# shellcheck disable=SC2086
		as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y \
			gcc-arm-none-eabi libnewlib-arm-none-eabi dfu-util make $bl_packages_debian
	elif [ "$PLATFORM" = fedora ]; then
		info "Installing firmware toolchain (dnf)"
		# shellcheck disable=SC2086
		as_root dnf install -y \
			arm-none-eabi-gcc-cs arm-none-eabi-newlib dfu-util make $bl_packages_fedora
	else
		info "Installing firmware toolchain (Homebrew)"
		brew_install dfu-util
		if ! printf '#include <string.h>\n' | \
				arm-none-eabi-gcc -x c -c - -o /dev/null >/dev/null 2>&1; then
			command -v arm-none-eabi-gcc >/dev/null 2>&1 && \
				die "arm-none-eabi-gcc cannot compile firmware (no C library?); install the gcc-arm-embedded cask instead"
			brew install --cask gcc-arm-embedded
		fi
		if [ "$UPDATE_BOOTLOADER" -eq 1 ] && \
				! printf '#include <cstring>\n' | \
				arm-none-eabi-g++ -x c++ -c - -o /dev/null >/dev/null 2>&1; then
			die "arm-none-eabi-g++ cannot compile the bootloader (no ARM C++ runtime?); install the gcc-arm-embedded cask"
		fi
	fi
}

step_system_deps() {
	if [ "$SKIP_DEPS" -eq 1 ]; then
		info "Skipping system dependencies (--skip-deps)"
		return 0
	fi
	if [ "$PLATFORM" = debian ]; then
		info "Installing system dependencies (apt)"
		as_root apt-get update -qq
		as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y \
			cmake build-essential pkg-config libusb-1.0-0-dev libbluetooth-dev \
			libfftw3-dev python3 python3-venv python3-usb1 git
	elif [ "$PLATFORM" = fedora ]; then
		info "Installing system dependencies (dnf)"
		as_root dnf install -y \
			cmake gcc gcc-c++ make pkgconf-pkg-config libusb1-devel \
			bluez-libs-devel fftw-devel python3 python3-pip python3-libusb1 git
	else
		info "Installing system dependencies (Homebrew)"
		brew_install cmake pkg-config libusb fftw
	fi
}

step_python_deps() {
	# On Linux python3 may only exist after the package step above.
	PYTHON="${PYTHON3:-$(command -v python3 || echo python3)}"
	require_python "$PYTHON"
	if [ -n "$VENV" ]; then
		info "Creating/using Python venv at $VENV"
		[ -d "$VENV" ] || "$PYTHON" -m venv "$VENV"
		PYTHON="$VENV/bin/python"
		"$PYTHON" -m pip install --quiet --upgrade pip
	fi
	info "Installing Python runtime deps for specan_ui and ubtbr"
	local pip_deps=(numpy qtpy pyside6)
	PY_DEPS_WARNING=""
	[ "$PLATFORM" = macos ] && pip_deps+=(libusb1==3.4.0)
	local gui_deps_hint
	case "$PLATFORM" in
		debian) gui_deps_hint="install python3-numpy python3-qtpy python3-pyqt5 (apt) or use --venv" ;;
		fedora) gui_deps_hint="install python3-numpy python3-QtPy python3-pyside6 (dnf) or use --venv" ;;
		*) gui_deps_hint="re-run with --venv <path> (recommended on macOS)" ;;
	esac
	if [ -n "$VENV" ]; then
		local venv_deps=("${pip_deps[@]}")
		on_linux && venv_deps+=(libusb1==3.4.0)
		if ! "$PYTHON" -m pip install --quiet "${venv_deps[@]}"; then
			PY_DEPS_WARNING="could not pip-install Python runtime deps into the venv $VENV"
			warn "$PY_DEPS_WARNING"
		fi
	elif ! "$PYTHON" -m pip install --quiet "${pip_deps[@]}" 2>/dev/null; then
		if on_linux; then
			"$PYTHON" -m pip install --quiet --break-system-packages "${pip_deps[@]}" 2>/dev/null || \
				PY_DEPS_WARNING="could not pip-install GUI deps; $gui_deps_hint"
		else
			PY_DEPS_WARNING="could not pip-install GUI deps; $gui_deps_hint"
		fi
		[ -z "$PY_DEPS_WARNING" ] || warn "$PY_DEPS_WARNING"
	fi
}

step_libbtbb() {
	if [ "$FORCE" -eq 0 ]; then
		local installed_ref
		installed_ref="$(installed_libbtbb_ref "$RECORD_DIR/libbtbb-files" 2>/dev/null || true)"
		if [ -n "$installed_ref" ] && [ "$installed_ref" = "$LIBBTBB_REF" ]; then
			info "libbtbb $LIBBTBB_REF is already installed; skipping (use --force to rebuild)"
			return 0
		fi
	fi
	info "Building libbtbb ($LIBBTBB_REF from $LIBBTBB_URL)"
	git clone --quiet "$LIBBTBB_URL" "$WORK/libbtbb"
	git -C "$WORK/libbtbb" checkout --quiet "$LIBBTBB_REF"
	cmake -S "$WORK/libbtbb" -B "$WORK/libbtbb/build" -DENABLE_PYTHON=OFF \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
	cmake --build "$WORK/libbtbb/build" -j"$JOBS" >/dev/null
	as_installer cmake --install "$WORK/libbtbb/build" >/dev/null
	record_installed libbtbb "$WORK/libbtbb/build" "libbtbb $LIBBTBB_REF from $LIBBTBB_URL"
	on_linux && as_root ldconfig || true
}

# Each directory once, in the order they are to be searched.
join_rpath() {
	local joined="" dir
	for dir in "$@"; do
		case ";$joined;" in *";$dir;"*) continue ;; esac
		joined="${joined:+$joined;}$dir"
	done
	printf '%s' "$joined"
}

# Resolve where libbtbb's library landed: from the build manifest if it was
# just built, otherwise from pkg-config, falling back to PREFIX/lib.
resolve_btbb_libdir() {
	BTBB_LIBDIR="$(sed -n 's#/pkgconfig/libbtbb\.pc$##p' \
		"$WORK/libbtbb/build/install_manifest.txt" 2>/dev/null | head -n 1 || true)"
	if [ -z "$BTBB_LIBDIR" ]; then
		BTBB_LIBDIR="$(pkg-config --variable=libdir libbtbb 2>/dev/null || true)"
	fi
	if [ -z "$BTBB_LIBDIR" ]; then
		BTBB_LIBDIR="$PREFIX/lib"
		warn "could not determine where libbtbb is installed; assuming $BTBB_LIBDIR"
	fi
}

step_host_tools() {
	resolve_btbb_libdir

	if [ "$FORCE" -eq 0 ] && command -v ubertooth-util >/dev/null 2>&1; then
		local tv iv
		tv="$(tree_version 2>/dev/null || true)"
		iv="$(installed_host_version ubertooth-util 2>/dev/null || true)"
		if [ -n "$tv" ] && [ -n "$iv" ] && [ "$tv" = "$iv" ]; then
			info "Host tools $iv are already installed; skipping (use --force to rebuild)"
			return 0
		fi
	fi

	local RPATH CMAKE_ARGS
	RPATH="$(join_rpath "$BTBB_LIBDIR" "$PREFIX/lib")"
	CMAKE_ARGS=(
		"-DCMAKE_INSTALL_PREFIX=$PREFIX"
		"-DCMAKE_PREFIX_PATH=$PREFIX"
		"-DCMAKE_INSTALL_RPATH=$RPATH"
	)
	export PKG_CONFIG_PATH="$BTBB_LIBDIR/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
	if on_linux; then
		info "Building Ubertooth host tools (REQUIRE_ALL=ON)"
		CMAKE_ARGS+=(-DREQUIRE_ALL=ON)
	else
		info "Building Ubertooth host tools"
	fi
	CMAKE_ARGS+=("-DCMAKE_C_FLAGS=${CFLAGS:-}")
	CMAKE_ARGS+=("-DPython3_EXECUTABLE=$(command -v "$PYTHON")")
	cmake -S "$REPO_ROOT/host" -B "$REPO_ROOT/host/build" \
		-U 'LIBBTBB_*' -U 'LIBUSB_*' "${CMAKE_ARGS[@]}"

	# Resolve libubertooth's install directory and adjust the rpath if needed.
	local UBERTOOTH_LIBDIR WANTED_RPATH
	UBERTOOTH_LIBDIR="$(sed -n 's/^UBERTOOTH_INSTALL_LIBDIR:[^=]*=//p' \
		"$REPO_ROOT/host/build/CMakeCache.txt" 2>/dev/null | head -n 1)"
	case "$UBERTOOTH_LIBDIR" in
		'') UBERTOOTH_LIBDIR="$PREFIX/lib" ;;
		/*) ;;
		*) UBERTOOTH_LIBDIR="$PREFIX/$UBERTOOTH_LIBDIR" ;;
	esac
	WANTED_RPATH="$(join_rpath "$UBERTOOTH_LIBDIR" "$BTBB_LIBDIR" "$PREFIX/lib")"
	if [ "$WANTED_RPATH" != "$RPATH" ]; then
		info "Setting the installed tools' run path to $WANTED_RPATH"
		cmake -S "$REPO_ROOT/host" -B "$REPO_ROOT/host/build" \
			"-DCMAKE_INSTALL_RPATH=$WANTED_RPATH"
	fi
	if [ "$UBERTOOTH_LIBDIR" != "$PREFIX/lib" ] &&
	   ls "$PREFIX"/lib/libubertooth.* >/dev/null 2>&1; then
		warn "an earlier install left libubertooth in $PREFIX/lib; this one installs to $UBERTOOTH_LIBDIR"
	fi

	cmake --build "$REPO_ROOT/host/build" -j"$JOBS"
	as_installer cmake --install "$REPO_ROOT/host/build"
	record_installed host "$REPO_ROOT/host/build"
	if on_linux; then
		as_root ldconfig
		info "udev rules installed with the host tools (unplug/replug the Ubertooth)"
	fi
}

step_firmware_flash() {
	[ "$WITH_FIRMWARE" -eq 1 ] || return 0
	flash_firmware
}

step_summary() {
	if [ ! -x "$PREFIX/bin/ubertooth-util" ]; then
		info "Not checking which libbtbb is in use: $PREFIX/bin/ubertooth-util is not installed"
	else
		local BTBB_ANSWER BTBB_IN_USE UTIL_ANSWERED
		BTBB_ANSWER="$("$PREFIX/bin/ubertooth-util" -V 2>&1 || true)"
		BTBB_IN_USE="$(printf '%s\n' "$BTBB_ANSWER" \
			| grep -e '^Host tools version: ' -e 'libbtbb was loaded from ' \
			|| true)"
		case "$BTBB_IN_USE" in
			*"Host tools version: "*) UTIL_ANSWERED=1 ;;
			*) UTIL_ANSWERED=0 ;;
		esac
		if [ "$UTIL_ANSWERED" -eq 0 ]; then
			warn "the installed ubertooth-util would not run, so which libbtbb it uses is unknown"
			printf '%s\n' "$BTBB_ANSWER" >&2
		else
			info "Installed versions:"
			printf '  %s\n' "$BTBB_IN_USE" >&2
			case "$BTBB_IN_USE" in
				*"libbtbb was loaded from "*)
					warn "the tools load a libbtbb other than the one they were built against" ;;
			esac
		fi
	fi

	if [ -n "$PY_DEPS_WARNING" ]; then
		info "Done, but the Python tools (ubertooth-specan-ui, ubtbr) will not run: $PY_DEPS_WARNING"
		info "Try: ubertooth-util -V"
	else
		info "Done. Try: ubertooth-util -V"
	fi
	case ":$ORIG_PATH:" in
		*":$PREFIX/bin:"*) ;;
		*) info "Add $PREFIX/bin to your PATH to run the installed tools" ;;
	esac
}

# =========================================================================
# Main flow
# =========================================================================

step_flash_preflight
step_firmware_toolchain_deps
step_system_deps
step_python_deps
step_libbtbb
step_host_tools
step_firmware_flash
step_summary
