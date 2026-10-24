#!/usr/bin/env python3
"""Verify ubertooth-btle refuses bad -a values and get+set before USB access,
and that "-q -" refuses a terminal.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using (#219).
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

# "-q -" streams binary PCAP, so a terminal is refused with the same message
# ubertooth-specan prints for "-d -", from the shared check.
if pty is None:
    print("ubertooth-btle: skipping the tty case, no pty on this platform")
else:
    master_fd, slave_fd = pty.openpty()
    try:
        result = subprocess.run(
            [BINARY, "-q", "-"],
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
    # refused before any device lookup
    assert "no ubertooth found" not in result.stderr.lower(), result
    print("ubertooth-btle: -q - refuses a tty")
