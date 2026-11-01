#!/usr/bin/env python3
"""Exercise ubertooth-ducky device-selector parsing before hardware access.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using.
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
# How long one ubertooth-ducky run is given. The whole test takes about 0.1 s
# on an idle machine and 0.5 s on one busy with parallel builds, over five
# short runs, where the cost that grows is starting a process rather than the
# work a run does. Twenty seconds is therefore many times what any one run
# needs, with room for a four-core machine slower again than this one.
RUN_TIMEOUT_S = 20


def run(*arguments):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_S,
        check=False,
        env=ENVIRONMENT,
    )


def require_parsed(arguments):
    """The selector parsed, so -h ends the command: usage on stdout, status 0."""
    result = run(*arguments, "-h")
    assert result.returncode == 0, result
    assert "ubertooth-ducky" in result.stdout, result
    assert result.stderr == "", result


require_parsed(("-U0",))
require_parsed(("-U", "0"))
require_parsed(("-Ucc3300112233445566778899aabbccdd",))
require_parsed(("-U", "cc3300112233445566778899aabbccdd"))

missing = run("-U")
assert missing.returncode == 1, missing
assert missing.stdout == "", missing
assert missing.stderr.startswith("error: option '-U' requires a value"), missing
