#!/usr/bin/env python3
"""Verify ubertooth-specan rejects invalid ranges before USB access, that only
a bare "-" means stdout for -d, and that "-d -" refuses a terminal.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using.
"""

import os
import subprocess
import sys
import tempfile

try:
    import pty
except ImportError:  # no pty module on this platform
    pty = None


BINARY = sys.argv[1]
# LC_ALL=C so the strerror() text in a file error is the one asserted below.
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1", LC_ALL="C")
# A serial no board has, so a run gets past the -d value and stops at device
# selection without touching a board.
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"


def run(arguments, directory=None):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
        env=ENVIRONMENT,
        cwd=directory,
    )


def reject(arguments, expected):
    result = run(arguments)
    output = result.stdout + result.stderr
    assert result.returncode != 0, result
    assert expected.lower() in output.lower(), result
    assert "could not open" not in output.lower(), result


reject(("-l",), "error: option '-l' requires a value")
reject(("-u",), "error: option '-u' requires a value")
reject(("-l", "bad"), "lower frequency")
reject(("-u", "2481"), "upper frequency")
reject(("-l", "2441", "-u", "2440"), "must not exceed")

# -d: only a bare "-" means stdout; every other value is a file name, even one
# starting with "-". Each run stops at device selection, after the -d value
# has been handled, so what it did with the value is visible on disk.
# Each case gets an empty directory of its own, so the file it is expected to
# create cannot be one an earlier case left behind.
for value, written in (("-x", "-x"), ("./-x", "-x"), ("-v", "-v"),
                       ("plain.dat", "plain.dat"), ("-", None)):
    with tempfile.TemporaryDirectory() as directory:
        result = run(("-U", NO_SUCH_SERIAL, "-d", value), directory)
        assert result.returncode == 1, (value, result)
        assert "no ubertooth found" in (result.stdout + result.stderr).lower(), \
            (value, result)
        listing = os.listdir(directory)
        # None: a bare "-" is stdout, so it writes no file of its own
        assert listing == ([] if written is None else [written]), \
            (value, listing, result)

def run_with_tty_stdout(arguments):
    """The tool with standard output on a pty. None if there is no pty to be
    had: the module is missing, or the sandbox allocates none."""
    if pty is None:
        return None
    try:
        master_fd, slave_fd = pty.openpty()
    except OSError:
        return None
    try:
        return subprocess.run(
            [BINARY, *arguments],
            stdin=subprocess.DEVNULL,
            stdout=slave_fd,
            stderr=subprocess.PIPE,
            text=True,
            timeout=5,
            check=False,
            env=ENVIRONMENT,
        )
    finally:
        os.close(slave_fd)
        os.close(master_fd)


# "-d -" on a terminal would fill it with raw sample records, so it is refused
# before any device is opened. The accepted cases are the "-" run above, whose
# stdout is a pipe, and "-d - -g" below, where the output is text again.
result = run_with_tty_stdout(("-d", "-"))
if result is None:
    print("ubertooth-specan: skipping the tty cases, no pty available")
else:
    assert result.returncode == 1, result
    assert "stdout is a tty, refusing to write binary samples" in result.stderr, \
        result
    # refused before the device was opened, which is where this message
    # would come from
    assert "could not open" not in result.stderr.lower(), result

    # -g means text output, which a terminal is welcome to, so this run gets
    # as far as device selection instead
    result = run_with_tty_stdout(("-d", "-", "-g"))
    assert result.returncode == 1, result
    assert "tty" not in result.stderr.lower(), result
    assert "could not open" in result.stderr.lower(), result
    print("ubertooth-specan: -d - refuses a tty, -d - -g writes text to one")

print("ubertooth-specan: ranges refused, -d takes '-' as stdout and the rest as files")

# A -d file that cannot be opened is reported like every other error: one
# line starting "error: ", on standard error, with nothing on standard output
# and exit 1. It used to be perror's unprefixed "<file>: <reason>".
result = run(("-d", "/no/such/dir/samples.bin"))
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr == \
    "error: /no/such/dir/samples.bin: No such file or directory\n", result
# refused before the device was opened
assert "could not open ubertooth" not in result.stderr.lower(), result

print("ubertooth-specan: a -d file that cannot be opened says error: on stderr")
