#!/usr/bin/env python3
"""ubertooth-specan-ui refuses a bad range on standard error, with exit 1.

Run as: test_specan_ui_streams.py <path to the specan package's directory>
                                 [<path to ubertooth-specan-ui>]

The checks live in specan/limits.py, which imports nothing but sys, so these
cases run wherever the Python tools are built -- no Qt binding, no numpy, no
window and no board. Each refusal writes one line starting "error: " to
standard error, nothing to standard output, and raises ValueError, which the
script turns into exit 1.

With the script named as well, the same cases are run through it end to end.
That needs its Qt binding and numpy; where they are missing the script cannot
start at all, so those runs say so and are skipped.
"""

import contextlib
import io
import os
import subprocess
import sys


PACKAGE_DIR = sys.argv[1]
SCRIPT = sys.argv[2] if len(sys.argv) > 2 else None

sys.path.insert(0, PACKAGE_DIR)
from specan import limits  # noqa: E402  (after the path is set up)


# Each case: the arguments the script would be given, the call the script
# makes for them, and the whole line it must write to standard error.
CASES = (
    (("--wifi", "nonsense"), lambda: limits.parse_wifi_range("nonsense"),
     "error: invalid channel range: nonsense\n"),
    (("--wifi", "1-2-3"), lambda: limits.parse_wifi_range("1-2-3"),
     "error: invalid channel range: 1-2-3\n"),
    (("--wifi", "15"), lambda: limits.convert_wifi(15),
     "error: channel 15 is not a valid wifi channel\n"),
    (("--wifi", "0"), lambda: limits.convert_wifi(0),
     "error: channel 0 is not a valid wifi channel\n"),
    (("-l", "2000"), lambda: limits.check_freq_pair(2000, limits.MAX_FREQ),
     "error: frequency of 2000 MHz is below minimum frequency of 2402\n"),
    (("-u", "3000"), lambda: limits.check_freq_pair(limits.MIN_FREQ, 3000),
     "error: frequency of 3000 MHz is above maximum frequency of 2480\n"),
    (("-l", "2470", "-u", "2410"), lambda: limits.check_freq_pair(2470, 2410),
     "error: lower frequency of 2470 MHz is above upper frequency of 2410 MHz\n"),
)


# ---- the checks themselves -------------------------------------------------

for _arguments, call, expected in CASES:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            call()
        except ValueError:
            pass
        else:
            raise AssertionError("%s was accepted" % expected.strip())
    # nothing on standard output: these used to print "ERROR: ..." there
    assert out.getvalue() == "", (expected, out.getvalue())
    assert err.getvalue() == expected, (expected, err.getvalue())

# A good value is accepted and says nothing at all.
for call in (lambda: limits.parse_wifi_range("1-11"),
             lambda: limits.convert_wifi(6),
             lambda: limits.check_freq_pair(2410, 2470)):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        call()
    assert out.getvalue() == "" and err.getvalue() == "", (out.getvalue(),
                                                           err.getvalue())

print("specan/limits.py: %d refusals, each one line on stderr" % len(CASES))


# ---- and the same cases through the script, where it can run --------------

if SCRIPT is None:
    sys.exit(0)

# The script never opens a device for these cases, but if a later change makes
# it try, this keeps it off whatever board is attached.
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1",
                   PYTHONPATH=os.pathsep.join(
                       [PACKAGE_DIR] + [os.environ["PYTHONPATH"]]
                       if "PYTHONPATH" in os.environ else [PACKAGE_DIR]))


def run(arguments):
    return subprocess.run([sys.executable, SCRIPT] + list(arguments),
                          env=ENVIRONMENT, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True,
                          timeout=60)


# --help comes from argparse: standard output, exit 0. It also proves the
# script's imports are satisfied, which tells a missing third-party module
# apart from a real failure.
result = run(["--help"])
if result.returncode != 0 and "ModuleNotFoundError" in result.stderr:
    missing = [line for line in result.stderr.splitlines()
               if "ModuleNotFoundError" in line]
    print("ubertooth-specan-ui: skipping the end-to-end cases (%s)"
          % "; ".join(missing))
    sys.exit(0)
assert result.returncode == 0, result
assert result.stdout.startswith("usage:"), result
assert result.stderr == "", result

for arguments, _call, expected in CASES:
    result = run(arguments)
    assert result.returncode == 1, (arguments, result)
    assert result.stdout == "", (arguments, result)
    assert result.stderr == expected, (arguments, expected, result)

print("ubertooth-specan-ui: the same %d refusals through the script"
      % len(CASES))
