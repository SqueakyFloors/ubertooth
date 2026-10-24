#!/usr/bin/env python3
"""A board that is not found ends the tool with exit 1, never a crash.

Run as: test_board_not_found.py <path to one ubertooth-* binary>

Each tool is started with a serial number no board has, plus whatever options
it needs to get past argument checking, so it reaches device selection and
finds nothing. That works with or without Ubertooths attached: no board is
opened. ubertooth-afh used to dereference the NULL handle here and die with
SIGSEGV (#185); ubertooth-follow ignored the failed selection.
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
TOOL = os.path.basename(BINARY)
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"

# Options each tool needs before it looks for a device.
ARGUMENTS = {
    "ubertooth-afh": ["-l", "446688", "-u", "22", "-t", "1"],
    "ubertooth-btle": ["-n"],
    "ubertooth-ducky": ["-b"],
    "ubertooth-dump": ["-d", os.devnull],
    "ubertooth-ego": ["-r"],
    "ubertooth-follow": ["-l", "446688", "-u", "22"],
    "ubertooth-rx": ["-t", "1"],
    "ubertooth-scan": [],
    "ubertooth-specan": [],
    "ubertooth-tx": ["-l", "446688", "-u", "22", "-t", "1"],
    "ubertooth-util": ["-v"],
}

if TOOL not in ARGUMENTS:
    print(f"{TOOL}: no board-not-found case")
    sys.exit(0)

command = [BINARY, "-U", NO_SUCH_SERIAL, *ARGUMENTS[TOOL]]
result = subprocess.run(command, capture_output=True, text=True, timeout=10,
                        check=False)
output = result.stdout + result.stderr
print("$", " ".join(command))
print(output, end="")
print(f"[exit {result.returncode}]")

# A negative return code is a signal (-11 is SIGSEGV).
assert result.returncode == 1, result
assert f"no ubertooth found with serial matching '{NO_SUCH_SERIAL}'" in output.lower(), result
