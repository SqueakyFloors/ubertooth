#!/usr/bin/env python3
"""install.sh keeps its progress off standard output.

Run as: test_install_streams.py <path to install.sh>

The script installs nothing here: every run below is refused or answered
while the command line is being read.

  * --help is what the user asked for, so it goes to standard output and
    exits 0;
  * a command line the script will not take goes to standard error, as one
    "error: " line, and exits 1, with nothing on standard output.

The "==> ..." progress lines, which used to go to standard output, are on
standard error now, so "./install.sh > log" no longer swallows them. They are
only reached by a run that installs something, so they are not exercised here;
what is checked is that standard output stays empty for everything short of
that.
"""

import os
import subprocess
import sys


SCRIPT = sys.argv[1]


def run(arguments):
    # The script is bash (its shebang, "set -o pipefail", $'\n'), so it is run
    # as bash rather than through /bin/sh, which is dash on Debian and Ubuntu
    # and stops at the "set" line before reading any argument.
    return subprocess.run(["bash", SCRIPT] + list(arguments),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=60,
                          cwd=os.path.dirname(os.path.abspath(SCRIPT)))


# --help: the header comment on standard output, exit 0
result = run(["--help"])
assert result.returncode == 0, result
assert "install.sh" in result.stdout, result
assert result.stderr == "", result

# an option the script does not take: one "error: " line on standard error,
# nothing on standard output, exit 1
result = run(["--bogus"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr.startswith("error: "), result
assert "\n" not in result.stderr.rstrip(), result

# a value the script checks and refuses, which is reported the same way
result = run(["--flash-device"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr.startswith("error: "), result

print("install.sh: --help on stdout, refusals as one error: line on stderr")
