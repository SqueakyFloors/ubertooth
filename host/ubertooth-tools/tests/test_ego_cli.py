#!/usr/bin/env python3
"""Verify invalid ubertooth-ego lengths fail before device access."""

import subprocess
import sys


BINARY = sys.argv[1]


def reject(value):
    result = subprocess.run(
        [BINARY, "-l", value],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )
    output = result.stdout + result.stderr
    assert result.returncode != 0, result
    assert "length must be in [1,48]" in output.lower(), result
    assert "could not open" not in output.lower(), result


for invalid in ("0", "49", "-1", "abc", "12x", "256", "257"):
    reject(invalid)

missing = subprocess.run(
    [BINARY, "-l"],
    capture_output=True,
    text=True,
    timeout=5,
    check=False,
)
assert missing.returncode != 0, missing
assert "requires an argument" in missing.stderr.lower(), missing
