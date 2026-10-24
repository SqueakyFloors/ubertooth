#!/usr/bin/env python3
"""Exercise ubertooth-ducky device-selector parsing before hardware access.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using (#219).
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")


def run(*arguments):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
        env=ENVIRONMENT,
    )


def require_parsed(arguments):
    result = run(*arguments, "-h")
    assert result.returncode == 1, result
    assert "ubertooth-ducky" in result.stdout, result
    assert "invalid option" not in result.stderr.lower(), result
    assert "requires an argument" not in result.stderr.lower(), result


require_parsed(("-U0",))
require_parsed(("-U", "0"))
require_parsed(("-U1ff0ff1967102caf8334fa5ac52000f5",))
require_parsed(("-U", "1ff0ff1967102caf8334fa5ac52000f5"))

missing = run("-U")
assert missing.returncode == 1, missing
assert "requires an argument" in missing.stderr.lower(), missing
