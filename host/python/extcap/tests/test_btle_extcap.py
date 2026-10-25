#!/usr/bin/env python3
"""btle-extcap.py's command line and its interface listing, without a board.

Run as: test_btle_extcap.py <path to btle-extcap.py>

Two things are checked:

  * every option the usage line names is one the script accepts. The usage
    line used to offer --list-interfaces, --list-dlts and --config, none of
    which getopt was given, so following the help got "option --list-dlts not
    recognized".

  * the interface listing, against a fake ubertooth-util on PATH that prints
    the listing a real one does. With several boards the listing is
    "<index>: <serial>" per board, which the old parser read as a serial of
    "0:". Each interface is named by the board's serial, which --capture
    passes to "ubertooth-btle -U <serial>", so re-plugging boards between the
    listing and the capture cannot move the capture to the other board.

  * --capture runs ubertooth-btle on the selected board, against a fake
    ubertooth-btle that records its arguments.

UBERTOOTH_TEST_NO_DEVICES=1 goes to the script as well, so a case that somehow
reached a real ubertooth-util would find no board rather than opening one this
machine is using.
"""

import os
import re
import subprocess
import sys
import tempfile


SCRIPT = sys.argv[1]
# The options the script accepts, with a value for those that take one.
ACCEPTED = {
    "--extcap-interfaces": None,
    "--extcap-dlts": None,
    "--extcap-config": None,
    "--capture": None,
    "--extcap-interface": "ubertooth" + "1ff0ff1967102caf8334fa5ac52000f5",
    "--fifo": os.devnull,
    "--channel": "37",
    "--help": None,
}
GONE = ("--list-interfaces", "--list-dlts", "--config")
SERIALS = ("1ff0ff1967102caf8334fa5ac52000f5",
           "0b700017490c2cafa039ee5ac52000f5")

# A fake ubertooth-util: prints the lines the test asks for and records the
# arguments it was called with, so no board is involved. It runs under the
# interpreter running this test, which is the one CMake selected, not whatever
# python3 is first on PATH.
FAKE_UTIL = """#!%s
import os
import sys

with open(os.environ["FAKE_UTIL_ARGS"], "w") as record:
    record.write("\\n".join(sys.argv[1:]))
with open(os.environ["FAKE_UTIL_OUTPUT"]) as listing:
    sys.stdout.write(listing.read())
sys.exit(int(os.environ.get("FAKE_UTIL_STATUS", "0")))
"""

# Kept in a name of its own, so its finalizer removes the directory however
# the test ends, including on a failed assertion.
WORK = tempfile.TemporaryDirectory()
directory = WORK.name
fake_util = os.path.join(directory, "ubertooth-util")
with open(fake_util, "w") as fake:
    fake.write(FAKE_UTIL % sys.executable)
os.chmod(fake_util, 0o755)

ARGS_FILE = os.path.join(directory, "args")
OUTPUT_FILE = os.path.join(directory, "listing")
ENVIRONMENT = dict(
    os.environ,
    PATH=directory + os.pathsep + os.environ.get("PATH", ""),
    FAKE_UTIL_ARGS=ARGS_FILE,
    FAKE_UTIL_OUTPUT=OUTPUT_FILE,
    UBERTOOTH_TEST_NO_DEVICES="1",
)


def run(arguments, listing="", status=0):
    with open(OUTPUT_FILE, "w") as output:
        output.write(listing)
    if os.path.exists(ARGS_FILE):
        os.remove(ARGS_FILE)
    environment = dict(ENVIRONMENT, FAKE_UTIL_STATUS=str(status))
    return subprocess.run(
        [sys.executable, SCRIPT, *arguments],
        capture_output=True,
        text=True,
        # Every run is milliseconds, and there are about a dozen, so this
        # stays well inside the CTest timeout and reports the hung case
        # itself.
        timeout=5,
        check=False,
        env=environment,
    )


def interfaces(result):
    return [line for line in result.stdout.split("\n")
            if line.startswith("interface ")]


# ---- the usage line names the options the script accepts -------------------

help_result = run(("-h",))
assert help_result.returncode == 0, help_result
usage_line = help_result.stdout
assert usage_line.startswith("Usage:"), help_result
for option in ("--extcap-interfaces", "--extcap-interface", "--extcap-dlts",
               "--extcap-config", "--capture", "--fifo", "--channel"):
    assert option in usage_line, (option, help_result)
for option in GONE:
    # each of these is a prefix of nothing the script accepts, so a plain
    # substring test is enough
    assert option not in usage_line, (option, help_result)

# and every option the usage line names is in fact accepted
named = set(re.findall(r"--[a-z-]+", usage_line))
assert named, usage_line
for option in sorted(named):
    assert option in ACCEPTED, (option, usage_line)
    value = ACCEPTED[option]
    argument = option if value is None else "%s=%s" % (option, value)
    result = run((argument,), listing="0: %s\n" % SERIALS[0])
    assert "not recognized" not in (result.stdout + result.stderr), \
        (argument, result)
    assert result.returncode != 2, (argument, result)
print("btle-extcap: the usage line names %d options, all accepted" % len(named))

# an option that really is unknown still says so: an "error: " line and the
# usage line on standard error, nothing on standard output, exit status 1
unknown = run(("--list-dlts",))
assert unknown.returncode == 1, unknown
assert unknown.stdout == "", unknown
assert unknown.stderr.startswith("error: ") and \
    "not recognized" in unknown.stderr, unknown
assert "Usage:" in unknown.stderr, unknown

# ---- the interface listing -------------------------------------------------

# several boards: one line each, named and shown by the board's serial
result = run(("--extcap-interfaces",),
             listing="0: %s\n1: %s\n" % SERIALS)
assert result.returncode == 0, result
assert interfaces(result) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[0],) * 2),
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[1],) * 2),
], result
with open(ARGS_FILE) as record:
    assert record.read().split("\n") == ["-U", "all", "-s"], \
        "the listing must be asked for explicitly with -U all"

# one board: the same form, so there is one parser rather than two
result = run(("--extcap-interfaces",), listing="0: %s\n" % SERIALS[0])
assert interfaces(result) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[0],) * 2),
], result

# a board that could not be read has no serial to offer, so it is left out
result = run(("--extcap-interfaces",),
             listing="0: <unavailable>\n1: %s\n" % SERIALS[1],
             status=1)
assert interfaces(result) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[1],) * 2),
], result

# no board at all: no interface lines, and no crash
result = run(("--extcap-interfaces",),
             listing="error: no Ubertooth devices found\n", status=1)
assert result.returncode == 0, result
assert interfaces(result) == [], result

# nothing that is not a listing line is taken for a serial
result = run(("--extcap-interfaces",),
             listing="ubertooth-util: command not found\n0:\n0: \n"
                     "Serial No: %s\n" % SERIALS[0])
assert interfaces(result) == [], result

print("btle-extcap: the interface listing reports each board's serial")

# ---- --capture --------------------------------------------------------------

# A fake ubertooth-btle on PATH records its arguments instead of opening a
# board. --capture passes the interface's serial to -U as its own word.
fake_btle = os.path.join(directory, "ubertooth-btle")
with open(fake_btle, "w") as fake:
    fake.write(FAKE_UTIL % sys.executable)
os.chmod(fake_btle, 0o755)

fifo = os.path.join(directory, "fifo")
result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[1],
              "--fifo=" + fifo, "--channel=38"))
assert result.returncode == 0, result
with open(ARGS_FILE) as record:
    assert record.read().split("\n") == \
        ["-f", "-U", SERIALS[1], "-q", fifo, "-A", "38"], result

# an interface saved by an older version of the script, "ubertooth<index>",
# still reaches ubertooth-btle, which reads an all-digit -U as an index
result = run(("--capture", "--extcap-interface=ubertooth0", "--fifo=" + fifo))
assert result.returncode == 0, result
with open(ARGS_FILE) as record:
    assert record.read().split("\n")[:3] == ["-f", "-U", "0"], result

# a malformed interface or a missing fifo is an error on standard error
result = run(("--capture", "--extcap-interface=bogus", "--fifo=" + fifo))
assert result.returncode == 1 and result.stdout == "", result
assert result.stderr.startswith("error: "), result
result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0]))
assert result.returncode == 1 and result.stdout == "", result
assert result.stderr.startswith("error: "), result

print("btle-extcap: --capture selects the board by serial")

WORK.cleanup()
