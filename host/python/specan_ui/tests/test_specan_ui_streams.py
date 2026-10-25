#!/usr/bin/env python3
"""ubertooth-specan-ui refuses a bad range on standard error, with exit 1.

Run as: test_specan_ui_streams.py <path to ubertooth-specan-ui>

The checks it makes on --wifi, -l and -u all run before the Qt application is
built, so no window opens and no board is looked for. Importing the script
does need a Qt binding (qtpy plus PySide6 or PyQt5); without one the script
cannot run at all, so the test says so and passes.

What is pinned here is the stream and the status, not the wording beyond the
"error: " prefix: a refusal writes nothing to standard output, says what was
wrong on standard error, and exits 1.
"""

import os
import subprocess
import sys


SCRIPT = sys.argv[1]
# The script never opens a device for these cases, but if a later change makes
# it try, this keeps it off whatever board is attached.
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")

CASES = (
    # a --wifi value that is not a channel or a range of them
    (("--wifi", "nonsense"), "error: invalid channel range: nonsense"),
    (("--wifi", "1-2-3"), "error: invalid channel range: 1-2-3"),
    # a channel outside 1-14
    (("--wifi", "15"), "error: channel 15 is not a valid wifi channel"),
    (("--wifi", "0"), "error: channel 0 is not a valid wifi channel"),
    # frequencies outside what the radio tunes, and a reversed pair
    (("-l", "2000"), "error: frequency of 2000 MHz is below minimum"),
    (("-u", "3000"), "error: frequency of 3000 MHz is above maximum"),
    (("-l", "2470", "-u", "2410"),
     "error: lower frequency of 2470 MHz is above upper frequency"),
)


def run(arguments):
    return subprocess.run([sys.executable, SCRIPT] + list(arguments),
                          env=ENVIRONMENT, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True,
                          timeout=60)


# --help comes from argparse: standard output, exit 0. It also proves the
# script imports, which tells a missing Qt binding apart from a real failure.
result = run(["--help"])
if result.returncode != 0 and "qtpy" in result.stderr:
    print("ubertooth-specan-ui: skipping, no Qt binding available")
    sys.exit(0)
assert result.returncode == 0, result
assert result.stdout.startswith("usage:"), result
assert result.stderr == "", result

for arguments, expected in CASES:
    result = run(arguments)
    assert result.returncode == 1, (arguments, result)
    # nothing on standard output: these used to print "ERROR: ..." there
    assert result.stdout == "", (arguments, result)
    assert expected in result.stderr, (arguments, expected, result)

print("ubertooth-specan-ui: %d refusals, each on stderr with exit 1"
      % len(CASES))
