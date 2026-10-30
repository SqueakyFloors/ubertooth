#!/usr/bin/env python3
"""install.sh keeps its progress off standard output.

Run as: test_install_streams.py <path to install.sh>

The script installs nothing here, and touches no device: the first runs are
refused or answered while the command line is being read, and the rest are
faked (see FAKES below) and stop at a device that is not there or a clone that
cannot succeed.

  * --help is what the user asked for, so it goes to standard output and
    exits 0;
  * a command line the script will not take goes to standard error, as one
    "error: " line, and exits 1, with nothing on standard output, in the
    wording every tool in this project uses;
  * a lone "--" ends the options and leaves nothing behind it, so the run
    goes ahead with the defaults rather than being refused;
  * -h is answered once the option loop has read the whole command line, so a
    bad option, a missing value or a stray word beside it is still reported.

The "==> ..." progress lines, which used to go to standard output, are on
standard error now, so "./install.sh > log" no longer swallows them. The two
that announce a slow step before it starts are checked against the faked runs;
the rest are only reached by a run that installs something. Standard output
must stay empty throughout.
"""

import os
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

# A command line the script will not take: one "error: " line on standard
# error, nothing on standard output, exit 1. The line is the one every tool in
# this project prints for the same mistake (.kiro/steering/cli-output.md), so
# a script reading standard error sees the same words whatever it called.
#
# A word that does not start with "-", and a lone "-", are operands rather
# than options the script is missing, and are reported as such.
REFUSALS = {
    "--bogus": "error: unrecognized option '--bogus'\n",
    "--flash-device": "error: option '--flash-device' requires a value\n",
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

# A lone "--" ends the options and leaves nothing behind it, so the run goes
# ahead with the defaults rather than being refused. It is checked against the
# fakes further down, where the run installs nothing; here only a word behind
# it is refused, as a stray word.
result = run(["--", "extra"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == "error: unexpected argument 'extra'\n", result


# The slow quiet steps announce themselves before they run.
#
# Faking brew, uname, sysctl, python3, make, git and the two Ubertooth tools
# puts the macOS path of the script in reach on any platform and installs
# nothing: the formulae are reported absent, no device is found, and the
# libbtbb clone fails, so every run ends before anything is built.
#
# Each fake also appends a line naming the call it just received to the file
# the script's standard error is going to, so the announcement and the slow
# command it describes land in one stream in the order they happened. The test
# then checks that the announcement comes before that command rather than
# merely somewhere earlier in the run.
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
    # No device attached. A run that was told which device to flash goes
    # ahead and flashes it anyway, which is how the named-device run below
    # reaches the fake ubertooth-dfu; a run without a selector stops.
    "ubertooth-util": 'echo "ran: ubertooth-util $*" >> "$FAKE_LOG";'
                      ' [ "$1" = -N ] && echo 0; exit 0',
    # Reached only by a --flash run, which needs both of these on PATH.
    "ubertooth-dfu": 'echo "ran: ubertooth-dfu $*" >> "$FAKE_LOG"; exit 0',
    "make": 'exit 0',
    "git": 'exit 1',
}


def run_with_fakes(arguments):
    """Run install.sh against the fakes; return (exit status, lines, stdout).

    The lines are the script's standard error and the fakes' own records,
    interleaved in the order they were written.
    """
    directory = tempfile.mkdtemp(prefix="install-sh-fakes.")
    try:
        prefix = os.path.join(directory, "prefix")
        os.mkdir(prefix)
        binaries = os.path.join(directory, "bin")
        os.mkdir(binaries)
        for name, body in FAKES.items():
            path = os.path.join(binaries, name)
            with open(path, "w") as fake:
                fake.write("#!/bin/sh\n%s\n" % body)
            os.chmod(path, 0o755)
        log = os.path.join(directory, "stderr")
        environment = dict(os.environ)
        # Replace PATH rather than prepend to it, so an Ubertooth install on
        # this machine cannot be picked up instead of the fakes.
        environment["PATH"] = os.pathsep.join(
            [binaries, "/usr/bin", "/bin", "/usr/sbin", "/sbin"])
        environment["FAKE_PREFIX"] = prefix
        environment["FAKE_LOG"] = log
        # Append mode, so the script's writes and the fakes' both land at the
        # end of the file: a plain write handle keeps its own offset and would
        # overwrite what a fake appended.
        with open(log, "a") as stderr:
            completed = subprocess.run(
                ["bash", SCRIPT] + list(arguments),
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


# The Homebrew formula check, about a second per formula, is announced before
# the first "brew list" runs rather than after it.
status, lines, stdout = run_with_fakes(["--skip-deps"])
assert status != 0, lines                      # the faked clone failed
assert stdout == "", stdout
in_order(lines, ["==> Checking Homebrew for an ubertooth or libbtbb",
                 "ran: brew list --formula --versions ubertooth",
                 "ran: brew list --formula --versions libbtbb"])

# The same run with a lone "--" behind the options reaches the same steps: the
# "--" is taken as the end of the options and nothing else. The in_order check
# is what pins that; the non-zero status only says the faked clone failed, as
# it does in the run above, and a "--" that was refused would give that too.
status, lines, stdout = run_with_fakes(["--skip-deps", "--"])
assert status != 0, lines
assert stdout == "", stdout
in_order(lines, ["==> Checking Homebrew for an ubertooth or libbtbb",
                 "ran: brew list --formula --versions ubertooth"])

# So is each read of the device list that resolves the flash target: the one
# in the pre-flight check, which a --flash with no selector goes through, and
# the one before the flash itself, which every --flash run reaches. The two
# announcements differ, so one of them cannot stand in for both.
status, lines, stdout = run_with_fakes(["--flash", "--skip-deps"])
assert status != 0, lines
assert stdout == "", stdout
in_order(lines, ["==> Checking which Ubertooth to flash",
                 "ran: ubertooth-util -s",
                 "==> Looking for the Ubertooth to flash",
                 "ran: ubertooth-util -s"])

# A named device skips the pre-flight check but still reads the device list.
# The name has letters in it, so the script takes it as a serial rather than
# as an index.
serial = "ab" * 16
status, lines, stdout = run_with_fakes(
    ["--flash-device", serial, "--skip-deps"])
assert status != 0, lines
assert stdout == "", stdout
in_order(lines, ["==> Looking for the Ubertooth to flash",
                 "ran: ubertooth-util -s",
                 "==> Flashing bluetooth_rxtx to Ubertooth serial " + serial,
                 "ran: ubertooth-dfu -U " + serial])

print("install.sh: --help on stdout, refusals and progress on stderr, "
      "slow steps announced before they run")
