#!/usr/bin/env python3
"""Which stream the usage text and a bad option land on, and the exit status.

Run as: test_output_streams.py <path to one ubertooth-* binary>

The rule (.kiro/steering/cli-output.md):

  * -h is what the user asked for: the usage text goes to standard output,
    nothing goes to standard error, and the status is 0;
  * a command line the tool will not take gets one "error: " line and then
    the usage text, both on standard error, nothing on standard output, and
    the status is 1.

No device is opened in either case, so nothing here depends on what is
attached; UBERTOOTH_TEST_NO_DEVICES=1 hides any board that is.

Tools are converted one at a time (see issue #323). A tool not yet in
FOLLOWS_THE_RULE is reported as pending and nothing is asserted for it, so
adding a tool to that set is what turns these checks on for it.
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
TOOL = os.path.basename(BINARY)
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")

# The tools whose output streams have been brought in line with the rule.
FOLLOWS_THE_RULE = {
    "ubertooth-afh",
    "ubertooth-ducky",
    "ubertooth-dump",
    "ubertooth-ego",
    "ubertooth-tx",
}

# One option per tool that takes a required value, for the missing-value case.
# A tool with no such option is left out of that check.
OPTION_NEEDING_A_VALUE = {
    "ubertooth-afh": "l",
    "ubertooth-dump": "d",
    "ubertooth-ego": "c",
    "ubertooth-tx": "l",
}


def run(arguments):
    return subprocess.run([BINARY] + list(arguments), env=ENVIRONMENT,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=20)


def usage_text(text):
    """The usage text names the tool and lists at least the -U line."""
    return TOOL in text and "-U <index|serial>" in text


if TOOL not in FOLLOWS_THE_RULE:
    print("%s: output streams pending, see issue #323" % TOOL)
    sys.exit(0)

# ---- -h: the usage text on stdout, nothing on stderr, status 0 -------------

result = run(["-h"])
assert result.returncode == 0, result
assert usage_text(result.stdout), result
assert result.stderr == "", result

# ---- an option the tool does not know --------------------------------------

result = run(["-Z"])
assert result.returncode == 1, result
# Nothing on stdout: a script reading the tool's output sees no usage text.
assert result.stdout == "", result
# The "error: " line comes first, then the usage text, both on stderr.
first, _, rest = result.stderr.partition("\n")
assert first == "error: unrecognized option '-Z'", result
assert usage_text(rest), result

# A word starting with -- is quoted as the user wrote it: getopt rejects its
# second '-', which would otherwise be reported as the option "--".
result = run(["--bogus"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr.startswith("error: unrecognized option '--bogus':"), result
assert usage_text(result.stderr), result

# ---- an option whose value is missing --------------------------------------

option = OPTION_NEEDING_A_VALUE.get(TOOL)
if option is not None:
    result = run(["-" + option])
    assert result.returncode == 1, result
    assert result.stdout == "", result
    first, _, rest = result.stderr.partition("\n")
    assert first == "error: option '-%s' requires a value" % option, result
    assert usage_text(rest), result

print("%s: output streams and exit status follow the rule" % TOOL)
