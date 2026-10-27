#!/usr/bin/env python3
"""install.sh keeps its progress off standard output.

Run as: test_install_streams.py <path to install.sh>

The script installs nothing here: every run below is refused or answered
while the command line is being read.

  * --help is what the user asked for, so it goes to standard output and
    exits 0;
  * a command line the script will not take goes to standard error, as one
    "error: " line, and exits 1, with nothing on standard output.

The "==> ..." progress lines, which used to go to standard output, are on
standard error now, so "./install.sh > log" no longer swallows them. The two
that announce a slow step before it starts are checked here, with fake brew,
uname and python3 on PATH; the rest are only reached by a run that installs
something. Standard output must stay empty throughout.
"""

import os
import subprocess
import sys


SCRIPT = sys.argv[1]
SCRIPT_DIR = os.path.dirname(os.path.abspath(SCRIPT))


def run(arguments, path=None):
    # The script is bash (its shebang, "set -o pipefail", $'\n'), so it is run
    # as bash rather than through /bin/sh, which is dash on Debian and Ubuntu
    # and stops at the "set" line before reading any argument.
    environment = dict(os.environ)
    if path is not None:
        # Replace PATH rather than prepend to it, so an Ubertooth install on
        # this machine cannot be picked up instead of the fakes.
        environment["PATH"] = os.pathsep.join(
            [path, "/usr/bin", "/bin", "/usr/sbin", "/sbin"])
    return subprocess.run(["bash", SCRIPT] + list(arguments),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=60,
                          env=environment, cwd=SCRIPT_DIR)


# --help: the header comment on standard output, exit 0
result = run(["--help"])
assert result.returncode == 0, result
assert "install.sh" in result.stdout, result
assert result.stderr == "", result

# an option the script does not take: one "error: " line on standard error,
# nothing on standard output, exit 1
result = run(["--bogus"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr.startswith("error: "), result
assert "\n" not in result.stderr.rstrip(), result

# a value the script checks and refuses, which is reported the same way
result = run(["--flash-device"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr.startswith("error: "), result


# The slow quiet steps announce themselves before they run.
#
# Faking brew, uname, sysctl, python3, ubertooth-util and git puts the macOS
# path of the script in reach on any platform and installs nothing: the
# formulae are reported absent, no device is found, and the libbtbb clone
# fails, which ends the run before anything is built. What matters is the
# order of the lines: each announcement must come out before the step it
# describes, which here means before the next announcement.
FAKES = {
    # Only "uname -s" is faked; anything else goes to the real one.
    "uname": '[ "$1" = -s ] && echo Darwin || exec /usr/bin/uname "$@"',
    # "brew --prefix" names a scratch prefix (which the script then compares
    # with its install prefix, so the formula check runs); "brew list" reports
    # every formula absent.
    "brew": 'case "$1" in --prefix) echo "$FAKE_PREFIX" ;; *) exit 1 ;; esac',
    "sysctl": 'echo 1',
    # The version check passes, pip fails: a warning, not a stop.
    "python3": 'case "$*" in *pip*) exit 1 ;; *) exit 0 ;; esac',
    # No device attached, so the flash pre-flight has nothing to refuse.
    "ubertooth-util": '[ "$1" = -N ] && echo 0; exit 0',
    "git": 'exit 1',
}


def run_with_fakes(arguments):
    import shutil
    import tempfile

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
        os.environ["FAKE_PREFIX"] = prefix
        return run(arguments, path=binaries)
    finally:
        os.environ.pop("FAKE_PREFIX", None)
        shutil.rmtree(directory, ignore_errors=True)


def lines_in_order(result, expected):
    lines = result.stderr.splitlines()
    found = [next((i for i, line in enumerate(lines) if text in line), None)
             for text in expected]
    assert None not in found, (expected, found, result)
    assert found == sorted(found), (expected, found, result)


# The Homebrew formula check, about a second per formula, is announced before
# the first "brew list" rather than after it.
result = run_with_fakes(["--skip-deps"])
assert result.returncode != 0, result          # the faked clone failed
assert result.stdout == "", result
lines_in_order(result, ["Checking Homebrew for an ubertooth or libbtbb",
                        "Skipping system dependencies"])

# So is the device probe that resolves the flash target.
result = run_with_fakes(["--flash", "--skip-deps"])
assert result.returncode != 0, result
assert result.stdout == "", result
lines_in_order(result, ["Checking Homebrew for an ubertooth or libbtbb",
                        "Looking for the Ubertooth to flash",
                        "Skipping system dependencies"])

print("install.sh: --help on stdout, refusals and progress on stderr, "
      "slow steps announced before they run")
