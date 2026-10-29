#!/usr/bin/env python3
"""Verify invalid ubertooth-ego lengths fail before device access.

UBERTOOTH_TEST_NO_DEVICES=1 hides attached boards from the tool, so a case
that reached device selection reports no device instead of opening a board
this machine is using.
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
# How long one ubertooth-ego run is given. The whole test takes about 0.55 s on
# an idle machine and 1.4 s on one busy with parallel builds, where the cost
# that grows is starting a process rather than the work these runs do. The
# four-core host this test went red on is slower again, so the twenty seconds
# here is about ten to twenty times an idle run there rather than here. A tool
# that hangs is still reported in tens of seconds rather than stalling the
# suite.
RUN_TIMEOUT_S = 20


def reject(value):
    result = subprocess.run(
        [BINARY, "-l", value],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_S,
        check=False,
        env=ENVIRONMENT,
    )
    output = result.stdout + result.stderr
    assert result.returncode != 0, result
    assert f"error: capture length must be 1-48 (got {value})" in output, result
    assert "could not open" not in output.lower(), result


for invalid in ("0", "49", "-1", "abc", "12x", "256", "257"):
    reject(invalid)

missing = subprocess.run(
    [BINARY, "-l"],
    capture_output=True,
    text=True,
    timeout=RUN_TIMEOUT_S,
    check=False,
    env=ENVIRONMENT,
)
assert missing.returncode != 0, missing
assert missing.stdout == "", missing
assert missing.stderr.startswith("error: option '-l' requires a value"), missing

# The usage text names -j, the original "jam" spelling, alongside -i.
help_run = subprocess.run(
    [BINARY, "-h"],
    capture_output=True,
    text=True,
    timeout=RUN_TIMEOUT_S,
    check=False,
    env=ENVIRONMENT,
)
assert help_run.returncode == 0, help_run
assert "-i, -j" in help_run.stdout, help_run
# -c is only used by continuous receive.
assert "continuous rx only" in help_run.stdout, help_run
# The channel -r receives on without -c, and the access code a run without -a
# captures with, are both real defaults: a mode start puts the code back to
# 630f9ffe, so the usage text names it the way the -l line names 18.
assert "default: 2418" in help_run.stdout, help_run
assert "default: 18" in help_run.stdout, help_run
assert "default: 630f9ffe" in help_run.stdout, help_run

# A channel the chosen mode never sends is refused before device selection, so
# nothing reports a missing board.
for mode in ("-f", "-i", "-j"):
    result = subprocess.run(
        [BINARY, mode, "-c", "2408"],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_S,
        check=False,
        env=ENVIRONMENT,
    )
    assert result.returncode == 1, (mode, result)
    assert result.stdout == "", (mode, result)
    assert result.stderr == (
        f"error: -c cannot be combined with {mode}; "
        "only continuous receive (-r) uses a channel\n"
    ), (mode, result)

# -j is the original jam spelling of -i: it is a major mode, so it gets past
# the "nothing to do" check and on to device selection, where no board is
# found (hidden by UBERTOOTH_TEST_NO_DEVICES), exactly as -i does.
for mode in ("-i", "-j"):
    result = subprocess.run(
        [BINARY, "-U", "no-such-board", mode],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_S,
        check=False,
        env=ENVIRONMENT,
    )
    output = result.stdout + result.stderr
    assert result.returncode != 0, (mode, result)
    assert "nothing to do" not in output, (mode, result)
