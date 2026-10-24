#!/usr/bin/env python3
"""A board that is not found ends the tool with exit 1, never a crash.

Run as: test_board_not_found.py <path to one ubertooth-* binary>

Each tool is started with a serial number no board has, plus whatever options
it needs to get past argument checking, so it reaches device selection and
finds nothing. ubertooth-afh used to dereference the NULL handle here and die
with SIGSEGV (#185).

The tools run with UBERTOOTH_TEST_NO_DEVICES=1, so selection finds nothing
whatever is attached. Matching a serial otherwise opens every attached board to
read its serial, which stalls on, and reports an error for, a board another
process is using, making this case depend on what the machine is doing (#219).
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
TOOL = os.path.basename(BINARY)
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"
# Hide attached boards from the tool: see the module docstring.
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")

# Options each tool needs before it looks for a device. None means the tool
# cannot reach device selection here, with the reason.
ARGUMENTS = {
    "ubertooth-afh": ["-l", "446688", "-u", "22", "-t", "1"],
    "ubertooth-btle": ["-n"],
    "ubertooth-debug": ["-r", "0"],
    "ubertooth-dfu": ["-r"],
    "ubertooth-ducky": ["-b"],
    "ubertooth-dump": ["-d", os.devnull],
    "ubertooth-ego": ["-r"],
    # follow and scan open a BlueZ HCI adapter (and follow connects to the
    # target) before they select the Ubertooth, so without an adapter they
    # stop before selection. Their selection checks are covered by reading.
    "ubertooth-follow": None,
    "ubertooth-rx": ["-t", "1"],
    "ubertooth-scan": None,
    "ubertooth-specan": [],
    "ubertooth-tx": ["-l", "446688", "-u", "22", "-t", "1"],
    "ubertooth-util": ["-v"],
}

if TOOL not in ARGUMENTS:
    sys.exit(f"no board-not-found case for {TOOL}: add it to ARGUMENTS")
if ARGUMENTS[TOOL] is None:
    print(f"{TOOL}: skipped, needs a Bluetooth adapter before device selection")
    sys.exit(0)

command = [BINARY, "-U", NO_SUCH_SERIAL, *ARGUMENTS[TOOL]]
result = subprocess.run(command, capture_output=True, text=True, timeout=10,
                        check=False, env=ENVIRONMENT)
output = result.stdout + result.stderr
print("$", " ".join(command))
print(output, end="")
print(f"[exit {result.returncode}]")

# A negative return code is a signal (-11 is SIGSEGV).
assert result.returncode == 1, result
assert f"no ubertooth found with serial matching '{NO_SUCH_SERIAL}'" in output.lower(), result
# No board was opened, so nothing can report a transfer error: a libusb
# complaint here means the switch stopped working and a board was touched.
assert "libusb error" not in output.lower(), result
