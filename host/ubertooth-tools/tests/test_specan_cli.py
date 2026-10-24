#!/usr/bin/env python3
"""Verify ubertooth-specan rejects invalid ranges before USB access.

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
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
        env=ENVIRONMENT,
    )
    output = result.stdout + result.stderr
    assert result.returncode != 0, result
    assert expected.lower() in output.lower(), result
    assert "could not open" not in output.lower(), result


reject(("-l",), "requires an argument")
reject(("-u",), "requires an argument")
reject(("-l", "bad"), "lower frequency")
reject(("-u", "2481"), "upper frequency")
reject(("-l", "2441", "-u", "2440"), "must not exceed")
