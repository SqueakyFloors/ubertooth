#!/usr/bin/env python3
"""What ubertooth-debug -r accepts and refuses, with no Ubertooth attached.

Run as: test_debug_cli.py <path to ubertooth-debug>

The whole -r value is read before a device is looked for, so both halves can
be checked without a board:

  * a refused value gets exactly one "error: ..." line and exit 1;
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

REFUSED = (
    ("0x100", "error: register address must be 0x00-0xff (got 0x100)"),
    ("0x00-0x100", "error: register address must be 0x00-0xff (got 0x100)"),
    ("300", "error: register address must be 0x00-0xff (got 0x12c)"),
    ("%nosuchregister",
     "error: register list must be names or addresses, single or as a range: "
     "unknown name (got %nosuchregister)"),
    ("0x02,",
     "error: register list must be names or addresses, single or as a range: "
     "empty value (got 0x02,)"),
    ("-5",
     "error: register list must be names or addresses, single or as a range: "
     "range with no start (got -5)"),
    ("09",
     "error: register list must be names or addresses, single or as a range: "
     "not an octal digit (got 09)"),
)

ACCEPTED = (
    "0x00",          # MAIN, the low end of the range
    "0xff",          # the high end
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
