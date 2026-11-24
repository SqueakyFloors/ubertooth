#!/usr/bin/env python3
"""install.sh keeps its progress off standard output.

Run as: test_install_streams.py <path to install.sh>

The script installs nothing here, and touches no device: the first runs are
refused or answered while the command line is being read, and the rest are
faked (see FAKES below), where the boards, the toolchain, and the two long
builds are all stand-ins, so a whole run takes a fraction of a second.

  * --help is what the user asked for, so it goes to standard output and
    exits 0, and it is the leading comment block plus the pinned libbtbb
    defaults and nothing else, so asking for help never prints the script;
  * a command line the script will not take goes to standard error, as one
    "error: " line, and exits 1, with nothing on standard output, in the
    wording every tool in this project uses;
  * a lone "--" ends the options and leaves nothing behind it, so the run
    goes ahead with the defaults rather than being refused;
  * -h is answered once the option loop has read the whole command line, so a
    bad option, a missing value or a stray word beside it is still reported;
  * which board a flash run writes is settled before any dependency or build
    work: with one readable board attached the selector is optional, and with
    none, with several and nothing saying which, or with a board waiting in
    the DFU bootloader, the run is refused there rather than after the build.

The "==> ..." progress lines, which used to go to standard output, are on
standard error now, so "./install.sh > log" no longer swallows them. The ones
that announce a slow step before it starts, and the ones that say which board
a flash is working on, are checked against the faked runs. Nothing of the
script's own goes to standard output at any point: what appears there is what
a tool it ran answered, and a --update-bootloader run keeps even that with the
rest of its output.
"""

import atexit
import os
import re
import shutil
import subprocess
import sys
import tempfile


SCRIPT = sys.argv[1]
SCRIPT_DIR = os.path.dirname(os.path.abspath(SCRIPT))


def run(arguments):
    # The script is bash (its shebang, "set -o pipefail", $'\n'), so it is run
    # as bash rather than through /bin/sh, which is dash on Debian and Ubuntu
    # and stops at the "set" line before reading any argument.
    return subprocess.run(["bash", SCRIPT] + list(arguments),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=60,
                          cwd=SCRIPT_DIR)


# --help: the header comment on standard output, exit 0
result = run(["--help"])
assert result.returncode == 0, result
assert "install.sh" in result.stdout, result
assert result.stderr == "", result


def leading_comment_block():
    """The script's own lines, and the help text it should print.

    The help text is every line from the third (below the shebang and the
    license line) up to the first line that is not a comment, with the "#"
    and the one space after it taken off, which is what the script's sed does.
    """
    with open(SCRIPT) as handle:
        lines = handle.read().split("\n")
    block = []
    for line in lines[2:]:
        if not line.startswith("#"):
            break
        block.append(re.sub(r"^#+ ?", "", line))
    return lines, block


# The help is that comment block and the two pinned libbtbb defaults, and
# nothing else: the body of the script is not help text, and a user who asks
# for help must not be handed the source.
lines, block = leading_comment_block()
assert len(block) > 20, block
help_lines = result.stdout.split("\n")
assert help_lines[-1] == "", result
assert help_lines[:len(block)] == block, (block, help_lines[:len(block)])
# Only the first few lines past the block are reported on a failure: a help
# text that ran on into the script would otherwise print the whole of it.
tail = help_lines[len(block):-1]
assert len(tail) == 4, tail[:8]
assert tail[0] == "", tail[:8]
assert tail[1] == "libbtbb defaults:", tail[:8]
# The pin itself moves, so the test checks that each line names its option and
# carries a value, not what the value is.
assert [line.split()[0] for line in tail[2:]] == \
    ["--libbtbb-url", "--libbtbb-ref"], tail[:8]
assert all(len(line.split()) == 2 for line in tail[2:]), tail[:8]

# The block ends at the first line that is not a comment, so a bare blank line
# in the middle of the header would end it early and quietly shorten the help.
# The header separates its paragraphs with "#" lines for that reason: between
# the end of the block and the first line of shell there is nothing but blank
# lines.
for line in lines[2 + len(block):]:
    if line.strip():
        assert not line.startswith("#"), line
        break

# A command line the script will not take: one "error: " line on standard
# error, nothing on standard output, exit 1. The line is the one every tool in
# this project prints for the same mistake (.kiro/steering/cli-output.md), so
# a script reading standard error sees the same words whatever it called.
#
# A word that does not start with "-", and a lone "-", are operands rather
# than options the script is missing, and are reported as such.
#
# Every run here is refused while the command line is being read, so none of
# them touches this machine. --flash-device and --update-bootloader with no
# selector are not in this set: a selector is optional with one board
# attached, so what they do depends on what is attached, and they are checked
# against the fakes further down.
REFUSALS = {
    "--bogus": "error: unrecognized option '--bogus'\n",
    # One spelling writes to a board, and it is --flash-device, so any other
    # spelling of it is an option the script does not know, like any other.
    "--flash": "error: unrecognized option '--flash'\n",
    "--prefix": "error: option '--prefix' requires a value\n",
    "--venv": "error: option '--venv' requires a value\n",
    "--libbtbb-url": "error: option '--libbtbb-url' requires a value\n",
    "--libbtbb-ref": "error: option '--libbtbb-ref' requires a value\n",
    "extra": "error: unexpected argument 'extra'\n",
    "-": "error: unexpected argument '-'\n",
}
for argument, line in REFUSALS.items():
    result = run([argument])
    assert result.returncode == 1, (argument, result)
    assert result.stdout == "", (argument, result)
    assert result.stderr == line, (argument, result)

# -h on its own is what the user asked for: the help on standard output,
# nothing on standard error, exit 0, the same as --help.
result = run(["-h"])
assert result.returncode == 0, result
assert "install.sh" in result.stdout, result
assert result.stderr == "", result

# -h is answered once the option loop has read the whole command line, as it
# is for every tool here, so a bad option, a missing value or a stray word
# beside it is still reported, in either order, with nothing on standard
# output.
for arguments in (["-h", "--bogus"], ["--bogus", "-h"]):
    result = run(arguments)
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr == "error: unrecognized option '--bogus'\n", result
result = run(["-h", "extra"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == "error: unexpected argument 'extra'\n", result

# An uninstall builds and flashes nothing, so asking it to install a
# bootloader says two things and means one. The refusal names the option that
# was given.
result = run(["--uninstall", "--update-bootloader"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == ("error: --uninstall cannot be combined with "
                         "--with-firmware, --flash-device, or "
                         "--update-bootloader\n"), result

# A lone "--" ends the options and leaves nothing behind it, so the run goes
# ahead with the defaults rather than being refused. It is checked against the
# fakes further down, where the run installs nothing; here only a word behind
# it is refused, as a stray word.
result = run(["--", "extra"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == "error: unexpected argument 'extra'\n", result


# The slow quiet steps announce themselves before they run, and a flash run
# settles which board it writes before any of them.
#
# Faking brew, uname, sysctl, python3, make, git, cmake and the two Ubertooth
# tools puts the macOS path of the script in reach on any platform and
# installs nothing: the formulae are reported absent, the boards are whatever
# FAKE_COUNT and FAKE_LISTING say, and the two long steps are told they are
# already done, so the run reaches the flash step in a fraction of a second
# and writes nothing anywhere.
#
# Each fake also appends a line naming the call it just received to the file
# the script's standard error is going to, so the announcement and the slow
# command it describes land in one stream in the order they happened. The test
# then checks that the announcement comes before that command rather than
# merely somewhere earlier in the run.

# The version the fakes agree on. The script skips the libbtbb and host-tools
# builds when what is installed matches what the tree would build, which is
# how these runs get to the flash step without a compiler: the fake git
# answers "describe" with FAKE_DESCRIBE, which the script turns into
# FAKE_VERSION, and the fake ubertooth-util reports that as the installed one.
FAKE_DESCRIBE = "2026-11-DEV9-3-gdeadbee"
FAKE_VERSION = "2026-11-DEV9+3"
# What a run is told is installed, so step_libbtbb skips. The ref is given on
# the command line as well, so the test does not have to know the pin.
FAKE_LIBBTBB_REF = "faked0000000000000000000000000000000000f"

FAKES = {
    # The script asks for "uname -s" and nothing else.
    "uname": 'echo Darwin',
    # "brew --prefix" names a scratch prefix (which the script then compares
    # with its install prefix, so the formula check runs); "brew list" reports
    # every formula absent.
    "brew": 'case "$1" in --prefix) echo "$FAKE_PREFIX" ;;'
            ' *) echo "ran: brew $*" >> "$FAKE_LOG"; exit 1 ;; esac',
    "sysctl": 'echo 1',
    # The version check passes, pip fails: a warning, not a stop.
    "python3": 'case "$*" in *pip*) exit 1 ;; *) exit 0 ;; esac',
    # FAKE_LISTING and FAKE_COUNT are what -s and -N answer, in the tool's own
    # wording: a run over several boards needs a listing, and by default none
    # is attached, which is what a flash run with no selector is refused for.
    #
    # -V answers with the version the fakes agree on, so the host-tools build
    # is skipped; -B with a bootloader line, as the tool does, because
    # --update-bootloader reads one back; and a selection that matches nothing
    # carries the note naming a board in DFU when FAKE_DFU_WAITING is set,
    # which is how the script finds such a board. A board's own firmware
    # version (-U <serial> -v) is left unanswered, so every board is taken to
    # need the flash.
    "ubertooth-util": 'echo "ran: ubertooth-util $*" >> "$FAKE_LOG";'
                      ' case "$*" in'
                      ' -s) printf \'%s\' "$FAKE_LISTING" ;;'
                      ' -N) echo "${FAKE_COUNT:-0}" ;;'
                      ' *-B*) echo "Bootloader: 2.00 git-faceb00c,'
                      ' CRC32:0x9d2fe1b4" ;;'
                      ' *-V*) echo "Host tools version: $FAKE_VERSION'
                      ' (libbtbb $FAKE_VERSION)" ;;'
                      ' *-U\\ zz*) [ -n "$FAKE_DFU_WAITING" ] && echo "note:'
                      ' a board is in the DFU bootloader, so no tool but'
                      ' ubertooth-dfu can open it" >&2 ;;'
                      ' esac; exit 0',
    # Reached only by a --flash-device run, which needs both of these on PATH.
    # FAKE_BL_CURRENT makes the bootloader update answer as it does for a board
    # that already has the bootloader the image carries, which is the one line
    # that tells that outcome from a board that was written; FAKE_BL_FAILS
    # makes it fail as a write that did not finish does.
    #
    # A plain -d answers on standard output, as the real tool does, so a run
    # that lets that through can be told from one that keeps it with the rest
    # of its own output.
    "ubertooth-dfu": 'echo "ran: ubertooth-dfu $*" >> "$FAKE_LOG";'
                     ' case " $* " in *" --update-bootloader "*)'
                     ' if [ -n "$FAKE_BL_FAILS" ]; then echo "error: the'
                     ' bootloader region did not change" >&2; exit 1; fi;'
                     ' [ -n "$FAKE_BL_CURRENT" ] && echo "The board already'
                     ' has this bootloader: ubertooth-util -B reports'
                     ' CRC32:0x9d2fe1b4." ;;'
                     ' *" -d "*) echo "Verified firmware revision:'
                     ' git-faceb00c" ;;'
                     ' esac; exit 0',
    "make": 'echo "ran: make $*" >> "$FAKE_LOG"; exit 0',
    # The version the tree would build, which the script takes from git
    # describe. Nothing else git is asked for can succeed here: a clone of
    # libbtbb would be a real one.
    "git": 'for argument in "$@"; do case "$argument" in'
           ' describe) echo "$FAKE_DESCRIBE"; exit 0 ;; esac; done;'
           ' echo "ran: git $*" >> "$FAKE_LOG"; exit 1',
    # Nothing here should reach a real build. If one ever does, this fails the
    # run at once rather than letting a test wait minutes for a compiler.
    "cmake": 'echo "ran: cmake $*" >> "$FAKE_LOG"; exit 1',
}


def make_fake_bin():
    """A directory holding every fake, built once and used by every run.

    Once rather than per run because macOS checks each newly written
    executable the first time it runs, about a fifth of a second apiece, which
    on a directory of fakes per run is most of what this test would cost.
    """
    directory = tempfile.mkdtemp(prefix="install-sh-fakes.")
    atexit.register(shutil.rmtree, directory, True)
    for name, body in FAKES.items():
        path = os.path.join(directory, name)
        with open(path, "w") as fake:
            fake.write("#!/bin/sh\n%s\n" % body)
        os.chmod(path, 0o755)
    return directory


FAKE_BIN = make_fake_bin()


def run_with_fakes(arguments, extra_environment=None):
    """Run install.sh against the fakes; return (exit status, lines, stdout).

    The lines are the script's standard error and the fakes' own records,
    interleaved in the order they were written. extra_environment is given to
    the fakes, which read it to choose which of two answers to give.
    """
    directory = tempfile.mkdtemp(prefix="install-sh-run.")
    try:
        prefix = os.path.join(directory, "prefix")
        # What the script's own record of an earlier install looks like, so
        # the libbtbb step finds the ref it was asked for already installed.
        record = os.path.join(prefix, "share", "ubertooth", "install-record")
        os.makedirs(record)
        with open(os.path.join(record, "libbtbb-files"), "w") as handle:
            handle.write("# libbtbb %s from https://example.invalid/fake.git\n"
                         % FAKE_LIBBTBB_REF)
        log = os.path.join(directory, "stderr")
        environment = dict(os.environ)
        # Replace PATH rather than prepend to it, so an Ubertooth install on
        # this machine cannot be picked up instead of the fakes.
        environment["PATH"] = os.pathsep.join(
            [FAKE_BIN, "/usr/bin", "/bin", "/usr/sbin", "/sbin"])
        environment["FAKE_PREFIX"] = prefix
        environment["FAKE_LOG"] = log
        environment["FAKE_VERSION"] = FAKE_VERSION
        environment["FAKE_DESCRIBE"] = FAKE_DESCRIBE
        environment["FAKE_DFU_WAITING"] = ""
        environment["FAKE_BL_CURRENT"] = ""
        environment["FAKE_BL_FAILS"] = ""
        environment["FAKE_LISTING"] = ""
        environment["FAKE_COUNT"] = "0"
        environment.update(extra_environment or {})
        # Append mode, so the script's writes and the fakes' both land at the
        # end of the file: a plain write handle keeps its own offset and would
        # overwrite what a fake appended.
        with open(log, "a") as stderr:
            completed = subprocess.run(
                ["bash", SCRIPT, "--libbtbb-ref", FAKE_LIBBTBB_REF]
                + list(arguments),
                stdout=subprocess.PIPE, stderr=stderr,
                universal_newlines=True, timeout=60,
                env=environment, cwd=SCRIPT_DIR)
        with open(log) as written:
            return completed.returncode, written.read().splitlines(), \
                completed.stdout
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def in_order(lines, expected):
    """Each expected text appears, in this order, each one its own line.

    A text repeated in `expected` must appear that many times, so a run that
    reads the device list twice cannot pass on one announcement.
    """
    at = 0
    for text in expected:
        found = next((i for i, line in enumerate(lines[at:], at)
                      if text in line), None)
        assert found is not None, (text, expected, at, lines)
        at = found + 1


def absent(lines, text):
    """No line holds this text."""
    assert not any(text in line for line in lines), (text, lines)


# The two long steps are skipped, so a run with nothing to flash gets all the
# way through: the Homebrew formula check, about a second per formula, is
# announced before the first "brew list" runs rather than after it, and the
# summary at the end reports what is installed.
status, lines, stdout = run_with_fakes(["--skip-deps"])
assert status == 0, lines
assert stdout == "", stdout
in_order(lines, ["==> Checking Homebrew for an ubertooth or libbtbb",
                 "ran: brew list --formula --versions ubertooth",
                 "ran: brew list --formula --versions libbtbb",
                 "==> libbtbb %s is already installed" % FAKE_LIBBTBB_REF,
                 "==> Host tools %s are already installed" % FAKE_VERSION,
                 "==> Done"])
# No board was asked about: this run flashes nothing.
absent(lines, "ran: ubertooth-dfu")
absent(lines, "==> Looking for the Ubertooth to flash")

# The same run with a lone "--" behind the options reaches the same steps: the
# "--" is taken as the end of the options and nothing else.
status, lines, stdout = run_with_fakes(["--skip-deps", "--"])
assert status == 0, lines
assert stdout == "", stdout
in_order(lines, ["==> Checking Homebrew for an ubertooth or libbtbb",
                 "ran: brew list --formula --versions ubertooth",
                 "==> Done"])


# --- which board a flash run writes, settled before any build work --------
#
# The selector is optional with exactly one readable board attached: there is
# nothing to choose between. With none attached, or with several, the run is
# refused -- and refused in the pre-flight check, before the firmware
# toolchain, the dependencies, the Python deps or either build, which is the
# whole point of that check. "Installing Python runtime deps" is the first
# thing a --skip-deps run would print past it, so its absence is what pins
# that the refusal came first.
BUILD_WORK = "==> Installing Python runtime deps"
ONE = "ab" * 16
TWO = "cd" * 16
one_board = {"FAKE_COUNT": "1",
             "FAKE_LISTING": "Serial No (0): %s\n" % ONE}
two_boards = {"FAKE_COUNT": "2",
              "FAKE_LISTING": "Serial No (0): %s\nSerial No (1): %s\n"
                              % (ONE, TWO)}
# One board whose serial the listing could not read. The count says it is
# there, so it is reported rather than dropped, and a flash needs the serial.
unreadable = {"FAKE_COUNT": "1", "FAKE_LISTING": ""}

for arguments in (["--flash-device"], ["--update-bootloader"],
                  ["--flash-device", "all"]):
    # Nothing attached.
    status, lines, stdout = run_with_fakes(arguments + ["--skip-deps"])
    assert status == 1, (arguments, lines)
    assert stdout == "", (arguments, stdout)
    in_order(lines, ["==> Checking which Ubertooth to flash",
                     "ran: ubertooth-util -s"])
    assert any("no Ubertooth is attached to flash" in line
               for line in lines), (arguments, lines)
    absent(lines, BUILD_WORK)
    absent(lines, "ran: make")
    absent(lines, "ran: ubertooth-dfu")

# Nothing running its firmware, but a board waiting in the DFU bootloader:
# that board is what a flash run was meant for, and it is reached by a
# different command, so the refusal names it rather than saying nothing is
# attached.
status, lines, stdout = run_with_fakes(["--flash-device", "--skip-deps"],
                                       {"FAKE_DFU_WAITING": "1"})
assert status == 1, lines
assert stdout == "", stdout
assert any("a board is waiting in the DFU bootloader" in line
           for line in lines), lines
absent(lines, BUILD_WORK)

for arguments in (["--flash-device"], ["--update-bootloader"]):
    # Several attached and nothing saying which: the refusal lists them, by
    # index and serial, so the user can name one.
    status, lines, stdout = run_with_fakes(arguments + ["--skip-deps"],
                                           two_boards)
    assert status == 1, (arguments, lines)
    assert stdout == "", (arguments, stdout)
    assert any("multiple Ubertooths attached; pick one with --flash-device"
               in line for line in lines), (arguments, lines)
    in_order(lines, ["0: " + ONE, "1: " + TWO])
    absent(lines, BUILD_WORK)
    absent(lines, "ran: ubertooth-dfu")

    # One attached, but its serial could not be read, and the flash goes by
    # serial. The installed tools may be older ones that cannot read it, so
    # the pre-flight check leaves it to this run's own, which refuse it after
    # the build.
    status, lines, stdout = run_with_fakes(arguments + ["--skip-deps"],
                                           unreadable)
    assert status == 1, (arguments, lines)
    assert stdout == "", (arguments, stdout)
    in_order(lines, ["could not read every Ubertooth", BUILD_WORK,
                     "the attached Ubertooth could not be read"])
    absent(lines, "ran: ubertooth-dfu")

# One readable board attached: the run goes ahead and flashes it, with no
# selector given. The pre-flight check and the flash step each read the device
# list, and the two announcements differ, so one of them cannot stand in for
# both: the second is the one that matters, because it is this run's own tools
# answering, after the build.
status, lines, stdout = run_with_fakes(["--flash-device", "--skip-deps"],
                                       one_board)
assert status == 0, lines
assert stdout == "Verified firmware revision: git-faceb00c\n", stdout
in_order(lines, ["==> Checking which Ubertooth to flash",
                 "ran: ubertooth-util -s",
                 BUILD_WORK,
                 "==> Host tools %s are already installed" % FAKE_VERSION,
                 "==> Looking for the Ubertooth to flash",
                 "ran: ubertooth-util -s",
                 "==> Building firmware (bluetooth_rxtx)",
                 "==> Flashing bluetooth_rxtx to Ubertooth " + ONE,
                 "ran: ubertooth-dfu -U %s -d " % ONE])
# One board, so no "board 0 of 1" line: there is nothing to tell apart.
absent(lines, "board 0 of 1")

# A named device is checked too, and the name has letters in it, so the script
# takes it as a serial rather than as an index.
#
# Standard output carries what ubertooth-dfu answered and nothing of the
# script's own: a plain flash run hands the board over to that tool, and its
# result is the tool's to report. (A --update-bootloader run keeps even that
# with the rest of its output; see the cases at the end of this file.)
status, lines, stdout = run_with_fakes(
    ["--flash-device", ONE, "--skip-deps"], one_board)
assert status == 0, lines
assert stdout == "Verified firmware revision: git-faceb00c\n", stdout
in_order(lines, ["==> Checking which Ubertooth to flash",
                 "==> Looking for the Ubertooth to flash",
                 "ran: ubertooth-util -s",
                 "==> Flashing bluetooth_rxtx to Ubertooth " + ONE,
                 "ran: ubertooth-dfu -U " + ONE])

# Every board at once, with the total said first and each board's own line
# naming it by the index --flash-device takes.
status, lines, stdout = run_with_fakes(
    ["--flash-device", "all", "--skip-deps"], two_boards)
assert status == 0, lines
in_order(lines, ["==> Looking for the Ubertooth to flash",
                 "==> Flashing all 2 attached Ubertooths",
                 "==>   board 0 of 2",
                 "ran: ubertooth-dfu -U %s -d " % ONE,
                 "==>   board 1 of 2",
                 "ran: ubertooth-dfu -U %s -d " % TWO])

print("install.sh: --help on stdout, refusals and progress on stderr, "
      "slow steps announced before they run, and which board to flash "
      "settled before the build")


# --- --update-bootloader --------------------------------------------------
#
# The bootloader goes on through ubertooth-dfu --update-bootloader, which
# leaves a board that already has it running its firmware and a board it wrote
# in DFU. Those two ends need different commands to put bluetooth_rxtx back,
# so each is run here, and the fake ubertooth-dfu answers as the tool does.
# Nothing is built or written: the images come from a fake make and the boards
# from a fake ubertooth-dfu.
#
# Standard output stays empty throughout, unlike the plain --flash-device run above:
# this option drives a procedure of several commands and reports one line per
# board itself, so every tool's answer is kept beside that report rather than
# leaving a board's result split across the two streams.

# A board that was written is left in DFU, where it has no serial for -U to
# name, so the firmware goes back on with no selector at all. The bootloader
# is read back off the board afterwards rather than assumed.
status, lines, stdout = run_with_fakes(
    ["--update-bootloader", "--flash-device", ONE, "--skip-deps"], one_board)
assert status == 0, lines
assert stdout == "", stdout
in_order(lines, ["==> Building firmware (bl_update",
                 "==> Installing the bootloader on " + ONE,
                 "ran: ubertooth-dfu --update-bootloader -d ",
                 "ran: ubertooth-dfu -d ",
                 "ran: ubertooth-util -U %s -B" % ONE,
                 "==> Board %s: bootloader installed" % ONE,
                 "==>   Bootloader: 2.00"])
# The warning the option is for is printed before the write, not after it.
in_order(lines, ["keep a serial programmer to hand",
                 "ran: ubertooth-dfu --update-bootloader -d "])
# --force is never passed to ubertooth-dfu: it writes without asking the board
# which bootloader it has, which costs the application region either way.
# (The script's own "use --force to rebuild" lines are not that.)
assert not any("--force" in line for line in lines
               if line.startswith("ran: ubertooth-dfu")), lines
# The firmware that goes back on is bluetooth_rxtx, and it is reset into.
flash = next(line for line in lines
             if line.startswith("ran: ubertooth-dfu -d "))
assert flash.endswith("bluetooth_rxtx.dfu -r"), flash
# The image written is bl_update's, and it is named for the board by serial.
update = next(line for line in lines
              if "ubertooth-dfu --update-bootloader" in line)
assert "bl_update/bl_update.dfu" in update, update
assert update.endswith("-U " + ONE), update

# A board that already has this bootloader is left alone, and the run says so
# and carries on to the ordinary flash, which reaches it by serial because the
# board never left its application.
status, lines, stdout = run_with_fakes(
    ["--update-bootloader", "--flash-device", ONE, "--skip-deps"],
    dict(one_board, FAKE_BL_CURRENT="1"))
assert status == 0, lines
assert stdout == "", stdout
in_order(lines, ["ran: ubertooth-dfu --update-bootloader -d ",
                 "ran: ubertooth-dfu -U %s -d " % ONE,
                 "==> Board %s: already had this bootloader, nothing written"
                 % ONE,
                 "==>   Bootloader: 2.00"])

# A board already waiting in DFU stops the run before anything is written, and
# before the build work: installing a bootloader leaves a second board there,
# and ubertooth-dfu with no selector will not choose between two.
status, lines, stdout = run_with_fakes(
    ["--update-bootloader", "--flash-device", ONE, "--skip-deps"],
    dict(one_board, FAKE_DFU_WAITING="1"))
assert status == 1, lines
assert stdout == "", stdout
assert any("a board is waiting in the DFU bootloader" in line
           for line in lines), lines
absent(lines, "ubertooth-dfu --update-bootloader")
absent(lines, BUILD_WORK)


# A bootloader update that fails stops the run, says the firmware was not put
# back on that board, and sends the reader to where the recovery is written
# down. Nothing else is flashed.
status, lines, stdout = run_with_fakes(
    ["--update-bootloader", "--flash-device", ONE, "--skip-deps"],
    dict(one_board, FAKE_BL_FAILS="1"))
assert status == 1, lines
assert stdout == "", stdout
assert any("error: installing the bootloader on %s failed" % ONE in line
           for line in lines), lines
assert any("UPDATING THE BOOTLOADER" in line for line in lines), lines
assert not any(line.startswith("ran: ubertooth-dfu -d ") for line in lines), \
    lines

# --flash-device all does the boards one at a time: each one's bootloader, its
# firmware and its read-back come before the next board is touched at all.
# Two boards are attached here, and the first is the one that gets written.
status, lines, stdout = run_with_fakes(
    ["--update-bootloader", "--flash-device", "all", "--skip-deps"],
    two_boards)
assert status == 0, lines
assert stdout == "", stdout
in_order(lines, ["==> Flashing all 2 attached Ubertooths",
                 "==>   board 0 of 2",
                 "==> Installing the bootloader on " + ONE,
                 "ran: ubertooth-dfu --update-bootloader -d ",
                 "ran: ubertooth-dfu -d ",
                 "ran: ubertooth-util -U %s -B" % ONE,
                 "==>   board 1 of 2",
                 "==> Installing the bootloader on " + TWO,
                 "ran: ubertooth-dfu --update-bootloader -d ",
                 "ran: ubertooth-dfu -d ",
                 "ran: ubertooth-util -U %s -B" % TWO])

print("install.sh: --update-bootloader writes one board at a time, never "
      "with --force, puts the firmware back and reads the bootloader off the "
      "board")


# With one readable board and no selector, --update-bootloader takes that
# board, by its serial, as a plain flash does.
status, lines, stdout = run_with_fakes(
    ["--update-bootloader", "--skip-deps"], one_board)
assert status == 0, lines
assert stdout == "", stdout
in_order(lines, ["==> Checking which Ubertooth to flash",
                 BUILD_WORK,
                 "==> Installing the bootloader on " + ONE,
                 "ran: ubertooth-dfu --update-bootloader -d ",
                 f"==> Board {ONE}: bootloader installed"])
update = next(line for line in lines
              if "ubertooth-dfu --update-bootloader" in line)
assert update.endswith("-U " + ONE), update

# The pre-flight check stands aside, rather than refusing, when the installed
# ubertooth-util cannot be relied on to answer: one that gives no count, and
# one from another tree that could not read a board (an older one lists only
# the first board's serial). The run's own tools decide, after the build.
status, lines, stdout = run_with_fakes(["--flash-device", "--skip-deps"],
                                       {"FAKE_COUNT": "x"})
assert status == 1, lines
in_order(lines, ["would not say how many Ubertooths are attached",
                 BUILD_WORK,
                 "==> Looking for the Ubertooth to flash"])
assert any("no Ubertooth is attached to flash" in line for line in lines), \
    lines

status, lines, stdout = run_with_fakes(
    ["--flash-device", "all", "--skip-deps"],
    dict(two_boards, FAKE_VERSION="2020-12-R1",
         FAKE_LISTING=f"Serial No: {ONE}\n"))
in_order(lines, ["could not read every Ubertooth", BUILD_WORK])
assert not any("a device could not be read" in line for line in lines), lines

# A named board that is not attached is refused before the build: an index
# past the last board, and a serial no board has.
for selector, refusal in (("2", "there is no Ubertooth at that index"),
                          ("ef" * 16, "no attached Ubertooth has a serial"),
                          ("", "matches more than one Ubertooth")):
    if not selector:
        # Two boards whose serials start the same way, and a prefix both
        # share: -U would refuse it, so the pre-flight check does too.
        status, lines, stdout = run_with_fakes(
            ["--flash-device", "abab", "--skip-deps"],
            {"FAKE_COUNT": "2",
             "FAKE_LISTING": f"Serial No (0): {ONE}\nSerial No (1): "
                             f"{'ab' * 8 + 'cd' * 8}\n"})
        assert status == 1, lines
        assert any(refusal in line for line in lines), lines
        absent(lines, BUILD_WORK)
        continue
    status, lines, stdout = run_with_fakes(
        ["--flash-device", selector, "--skip-deps"], two_boards)
    assert status == 1, (selector, lines)
    assert any(refusal in line for line in lines), (selector, lines)
    absent(lines, BUILD_WORK)

# A serial is matched as -U matches it: its start, in either case, names the
# board, and the flash goes to that board's full serial.
for selector in ("ABAB", ONE.upper(), "cd"):
    status, lines, stdout = run_with_fakes(
        ["--flash-device", selector, "--skip-deps"], two_boards)
    assert status == 0, (selector, lines)
    target = TWO if selector == "cd" else ONE
    in_order(lines, ["==> Flashing bluetooth_rxtx to Ubertooth " + target,
                     "ran: ubertooth-dfu -U " + target])

print("install.sh: --update-bootloader with no selector takes the one board, "
      "and the pre-flight check defers to this run's tools when the "
      "installed one cannot answer")
