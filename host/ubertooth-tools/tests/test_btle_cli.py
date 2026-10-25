#!/usr/bin/env python3
"""Verify ubertooth-btle refuses bad -a values, get+set and a command line
with no work in it before USB access, and that "-q -" refuses a terminal.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using.
"""

import os
import subprocess
import sys

try:
    import pty
except ImportError:  # no pty module on this platform
    pty = None


BINARY = sys.argv[1]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")


def reject(arguments, expected):
    result = subprocess.run(
        [BINARY, "-U", "no-such-board", *arguments],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
        env=ENVIRONMENT,
    )
    output = result.stdout + result.stderr
    assert result.returncode == 1, result
    assert expected in output, result
    # refused before any device lookup
    assert "no ubertooth found" not in output.lower(), result


BAD = "error: access address must be 1-8 hex digits"
reject(("-azzzz",), BAD + " (got zzzz)")
reject(("-a", "zzzz"), BAD + " (got zzzz)")
reject(("-a123456789",), BAD + " (got 123456789)")
reject(("-a0x8e89bed6",), BAD + " (got 0x8e89bed6)")
BOTH = "error: -a cannot both read and set the access address"
reject(("-a8e89bed6", "-a"), BOTH)
reject(("-a", "-a11223344"), BOTH)
reject(("-a", "11223344", "-a"), BOTH)

# A command line with nothing to act on: refused before the device is opened,
# where it used to open the board, send nothing and exit 0 after printing the
# usage text on stdout.
NOTHING = "error: nothing to do: choose a major mode (-f, -n or -p)," \
    " or -a, -v, -s or -t"
reject((), NOTHING)
# the options that only shape work asked for by another option, including
# -j/-J, the older spellings of -i/-I
reject(("-A", "39"), NOTHING)
reject(("-x", "0"), NOTHING)
reject(("-i",), NOTHING)
reject(("-I",), NOTHING)
reject(("-j",), NOTHING)
reject(("-J",), NOTHING)

# the refusal goes to standard error, followed by the usage text (also on
# standard error, as after any bad command line), and nothing is printed on
# standard output: the usage text used to land there
result = subprocess.run(
    [BINARY],
    capture_output=True,
    text=True,
    timeout=5,
    check=False,
    env=ENVIRONMENT,
)
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr.startswith(NOTHING + "\n\n"), result
assert "Usage:" in result.stderr and "Major modes:" in result.stderr, result
# each of the options that do have work in it gets past this check: it is the
# device the tool then complains about, not the command line
for arguments in (("-f",), ("-n",), ("-p",), ("-a",), ("-a8e89bed6",),
                  ("-v",), ("-v1",), ("-s22:44:66:88:aa:cc",),
                  ("-t22:44:66:88:aa:cc",), ("-tnone",)):
    result = subprocess.run(
        [BINARY, "-U", "ffffffffffffffffffffffffffffffff", *arguments],
        capture_output=True,
        text=True,
        errors="replace",
        timeout=5,
        check=False,
        env=ENVIRONMENT,
    )
    output = result.stdout + result.stderr
    assert result.returncode == 1, (arguments, result)
    assert "nothing to do" not in output, (arguments, result)
    assert "no ubertooth found" in output.lower(), (arguments, result)
print("ubertooth-btle: a command line with no work in it is refused")

# "-q -" streams binary PCAP, so a terminal is refused with the same message
# ubertooth-specan prints for "-d -", from the shared check. A mode goes with
# it, since the capture file is opened only once the command line is accepted
# and a capture option on its own has no work in it.
if pty is None:
    master_fd = None
else:
    try:
        master_fd, slave_fd = pty.openpty()
    except OSError:  # a sandbox with no pty to allocate
        master_fd = None

if master_fd is None:
    print("ubertooth-btle: skipping the tty case, no pty available")
else:
    try:
        result = subprocess.run(
            [BINARY, "-n", "-q", "-"],
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
    assert result.returncode == 1, result
    assert "stdout is a tty, refusing to write binary capture data" \
        in result.stderr, result
    # refused before the device was opened, which is where this message would
    # come from
    assert "could not open" not in result.stderr.lower(), result
    print("ubertooth-btle: -q - refuses a tty")

# Usage text streams: -h asked for it, so it is the command's output and goes
# to standard output; after an unknown option it explains an error and goes to
# standard error, with standard output left empty.
result = subprocess.run([BINARY, "-h"], capture_output=True, text=True,
                        timeout=5, check=False, env=ENVIRONMENT)
assert "Usage:" in result.stdout, result
assert "Usage:" not in result.stderr, result
result = subprocess.run([BINARY, "-Z"], capture_output=True, text=True,
                        timeout=5, check=False, env=ENVIRONMENT)
assert result.returncode == 1, result
assert result.stdout == "", result
assert "Usage:" in result.stderr, result
print("ubertooth-btle: usage on stdout for -h, on stderr after a bad option")
