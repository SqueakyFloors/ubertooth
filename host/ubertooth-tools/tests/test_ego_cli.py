#!/usr/bin/env python3
"""Verify invalid ubertooth-ego lengths fail before device access.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using.
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")


def reject(value):
    result = subprocess.run(
        [BINARY, "-l", value],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
        env=ENVIRONMENT,
    )
    output = result.stdout + result.stderr
    assert result.returncode != 0, result
    assert f"error: capture length must be 1-48 (got {value})" in output, result
    assert "could not open" not in output.lower(), result


for invalid in ("0", "49", "-1", "abc", "12x", "256", "257"):
    reject(invalid)

missing = subprocess.run(
    [BINARY, "-l"],
    capture_output=True,
    text=True,
    timeout=5,
    check=False,
    env=ENVIRONMENT,
)
assert missing.returncode != 0, missing
assert "requires an argument" in missing.stderr.lower(), missing
