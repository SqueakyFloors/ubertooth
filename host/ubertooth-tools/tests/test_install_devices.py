#!/usr/bin/env python3
"""install.sh reads the device listing ubertooth-util actually prints.

Run as: test_install_devices.py <path to install.sh> <path to ubertooth-util.md>

install.sh maps each attached board's index to its serial by parsing
`ubertooth-util -s`, and uses that for `--flash-device all` and for the message
that lists the boards to choose from. A change to the tool's output used to
leave that parser behind without anything failing: every serial became "?",
`--flash-device all` refused to flash, and the message offered no serials to
pick from.

So the lines fed in here are the ones ubertooth-util's man page shows, which
the repository requires to be updated with the tool. Only install.sh's
list_devices function is run, with a fake ubertooth-util: nothing is installed
and no board is involved.
"""

import os
import re
import subprocess
import sys
import tempfile


SCRIPT = sys.argv[1]
MAN_PAGE = sys.argv[2]

# The function under test, taken out of install.sh so the rest of the script
# does not run. It is the only one at the top level between these markers.
FUNCTION = re.compile(r'^list_devices\(\) \{$.*?^\}$', re.M | re.S)

# A listing line as the man page shows it, indented as a code block.
DOCUMENTED_LINE = re.compile(r'^ +(Serial No \(\d+\): [0-9a-f]{32})$', re.M)


def list_devices(listing, count):
    """Run install.sh's list_devices() against a fake ubertooth-util."""
    with open(SCRIPT) as script:
        body = FUNCTION.search(script.read())
    assert body is not None, "list_devices() not found in " + SCRIPT
    with tempfile.TemporaryDirectory() as directory:
        util = os.path.join(directory, "ubertooth-util")
        with open(util, "w") as fake:
            fake.write('#!/bin/sh\ncase "$1" in\n'
                       '-s) printf \'%s\' "$LISTING" ;;\n'
                       '-N) printf \'%s\\n\' "$COUNT" ;;\n'
                       'esac\n')
        os.chmod(util, 0o755)
        result = subprocess.run(
            ["bash", "-c", body.group(0) + '\nlist_devices "$1"\n',
             "list_devices", util],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True, timeout=30,
            env=dict(os.environ, LISTING=listing, COUNT=str(count)))
        assert result.returncode == 0, result
        return result.stdout.splitlines()


with open(MAN_PAGE) as page:
    documented = DOCUMENTED_LINE.findall(page.read())
assert documented, \
    "%s no longer shows a listing line; it is where the shape install.sh " \
    "parses is written down" % MAN_PAGE

# Every documented line gives install.sh the index it names and the serial
# beside it. The page shows several examples, so each line is fed on its own
# rather than assuming they make up one listing.
for line in sorted(set(documented)):
    index = int(re.search(r'\((\d+)\)', line).group(1))
    serial = line.rsplit(": ", 1)[1]
    parsed = list_devices(line + "\n", index + 1)
    assert parsed[index] == "%d %s" % (index, serial), (line, parsed)
print("install.sh: the %d listing lines the man page shows are parsed"
      % len(set(documented)))

# The single-board form, which has no index in it, is still read as device 0.
SERIAL = "aa1100112233445566778899aabbccdd"
assert list_devices("Serial No: %s\n" % SERIAL, 1) == ["0 " + SERIAL]

# A board the listing could not read has no serial to give, so it keeps the
# "?" and the board beside it is still named: that is what makes the refusal
# say which boards are readable.
assert list_devices("Serial No (0): unavailable, in use by another program\n"
                    "Serial No (1): %s\n" % SERIAL, 2) == ["0 ?", "1 " + SERIAL]
print("install.sh: a board that could not be read is reported, not dropped")
