#!/usr/bin/env python3
"""What ubertooth-debug -r accepts and refuses, with no Ubertooth attached.

Run as: test_debug_cli.py <path to ubertooth-debug>

The whole -r value is read before a device is looked for, so both halves can
be checked without a board:

  * a refused value gets exactly one "error: ..." line and exit 1, quoting
    the offending item as the user wrote it;
  * an accepted value gets no diagnostic of its own and reaches device
    selection, which with no device found is itself the proof that the value
    parsed. 0x00 (MAIN) and 0xff are both accepted, which is what the old
    "> 0x00 and < 0xff" message denied.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so selection
finds nothing whatever this machine has plugged in.
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
NO_DEVICE = "could not open ubertooth device"

SYNTAX = ("error: register list must be names or addresses, single or as a "
          "range: ")

REFUSED = (
    # the refused item is quoted as the user wrote it, never re-rendered, and
    # the whole value comes with it when the item is only part of it
    ("0x100", "error: register address must be 0x00-0xff (got 0x100)"),
    ("300", "error: register address must be 0x00-0xff (got 300)"),
    ("0x02,300",
     "error: register address must be 0x00-0xff (got 300 in 0x02,300)"),
    ("0x00-0x100",
     "error: register address must be 0x00-0xff (got 0x00-0x100)"),
    # too large for an int: refused as the digits are read, so the message
    # cannot name a wrapped-round value the user never typed
    ("99999999999",
     "error: register address must be 0x00-0xff (got 99999999999)"),
    ("0xffffffff",
     "error: register address must be 0x00-0xff (got 0xffffffff)"),
    # a range is refused before it is expanded, so a huge one costs nothing
    ("0-0xffffff",
     "error: register address must be 0x00-0xff (got 0-0xffffff)"),
    ("%nosuchregister", SYNTAX + "unknown name (got %nosuchregister)"),
    ("0x02,%nosuch,0x03",
     SYNTAX + "unknown name (got %nosuch in 0x02,%nosuch,0x03)"),
    ("0x02,", SYNTAX + "empty value (got 0x02,)"),
    ("-5", SYNTAX + "range with no start (got -5)"),
    ("09", SYNTAX + "not an octal digit (got 09)"),
)

ACCEPTED = (
    "0x00",          # MAIN, the low end of the range
    "0xff",          # the high end
    "255",           # the same, in decimal
    "0x02,0x01,0x00",
    "0x00-0xff",
    "%main",
    "%MAIN,%FSDIV",  # a name anywhere in a list, not only last
    "%main-%fsdiv",  # a range written with names
    "0x01,%MAIN",
)


def run(value):
    return subprocess.run(
        [BINARY, "-r", value],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
        env=ENVIRONMENT,
    )


for value, expected in REFUSED:
    result = run(value)
    output = (result.stdout + result.stderr).lower()
    assert result.returncode == 1, (value, result)
    assert expected in output, (value, expected, result)
    assert NO_DEVICE not in output, (value, result)
    # exactly one diagnostic line, not a terse parser line plus this one
    assert len([line for line in output.splitlines() if line.strip()]) == 1, \
        (value, result)

for value in ACCEPTED:
    result = run(value)
    output = (result.stdout + result.stderr).lower()
    assert result.returncode == 1, (value, result)
    assert "register" not in output, (value, result)
    assert NO_DEVICE in output, (value, result)

print("ubertooth-debug: %d refused and %d accepted -r values"
      % (len(REFUSED), len(ACCEPTED)))
