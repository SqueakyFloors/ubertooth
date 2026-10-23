#!/usr/bin/env python3
"""Exercise ubertooth-ducky device-selector parsing before hardware access."""

import subprocess
import sys


BINARY = sys.argv[1]


def run(*arguments):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
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
