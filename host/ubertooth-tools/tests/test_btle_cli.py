#!/usr/bin/env python3
"""Verify ubertooth-btle refuses bad -a values and get+set before USB access.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using (#219).
"""

import os
import subprocess
import sys


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
