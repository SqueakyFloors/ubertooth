#!/usr/bin/env python3
"""Verify ubertooth-specan rejects invalid ranges before USB access, and that
only a bare "-" means stdout for -d.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using (#219).
"""

import os
import subprocess
import sys
import tempfile


BINARY = sys.argv[1]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
# A serial no board has, so a run gets past the -d value and stops at device
# selection without touching a board.
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"


def run(arguments, directory=None):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
        env=ENVIRONMENT,
        cwd=directory,
    )


def reject(arguments, expected):
    result = run(arguments)
    output = result.stdout + result.stderr
    assert result.returncode != 0, result
    assert expected.lower() in output.lower(), result
    assert "could not open" not in output.lower(), result


reject(("-l",), "requires an argument")
reject(("-u",), "requires an argument")
reject(("-l", "bad"), "lower frequency")
reject(("-u", "2481"), "upper frequency")
reject(("-l", "2441", "-u", "2440"), "must not exceed")

# -d: only a bare "-" means stdout; every other value is a file name, even one
# starting with "-" (#231). Each run stops at device selection, after the -d
# value has been handled, so what it did with the value is visible on disk.
with tempfile.TemporaryDirectory() as directory:
    for name in ("-x", "./-x", "-v", "plain.dat"):
        result = run(("-U", NO_SUCH_SERIAL, "-d", name), directory)
        assert result.returncode == 1, (name, result)
        assert "no ubertooth found" in (result.stdout + result.stderr).lower(), \
            (name, result)
        written = os.path.join(directory, os.path.basename(name))
        assert os.path.exists(written), (name, os.listdir(directory), result)

    # A bare "-" is stdout, so it writes no file of its own.
    before = set(os.listdir(directory))
    result = run(("-U", NO_SUCH_SERIAL, "-d", "-"), directory)
    assert result.returncode == 1, result
    assert "no ubertooth found" in (result.stdout + result.stderr).lower(), result
    assert set(os.listdir(directory)) == before, os.listdir(directory)

print("ubertooth-specan: ranges refused, -d takes '-' as stdout and the rest as files")
