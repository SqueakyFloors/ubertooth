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
    ubertooth-btle that records its arguments, and reports how it ended: a
    child that failed makes the script exit non-zero, which is the only way
    Wireshark hears about a failed capture, while a capture Wireshark asked
    to stop still exits 0.

  * the options Wireshark adds to its own calls. --extcap-version comes with
    the --extcap-interfaces call Wireshark makes at start-up, and used to end
    the script with "option --extcap-version not recognized" before anything
    was listed, so Wireshark saw no interface at all. All three ways of
    writing it are checked ahead of another option, because the bare form was
    made to parse by appending "=" to it, which turned the value of the
    next-word form into a stray word and dropped every option after it. The
    options Wireshark sends only to a program that advertised the feature
    behind them are still refused, and so is a leftover word.

UBERTOOTH_TEST_NO_DEVICES=1 goes to the script as well, so a case that somehow
reached a real ubertooth-util would find no board rather than opening one this
machine is using.
"""

import importlib.machinery
import importlib.util
import os
import re
import signal
import subprocess
import sys
import tempfile
import time


SCRIPT = sys.argv[1]
# The channels ubertooth-btle will tune to, which is what the script offers
# in its configuration and the only thing it accepts for --channel.
ADVERTISING_CHANNELS = ("37", "38", "39")
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
#
# FAKE_UTIL_STATUS is the status it exits with, FAKE_UTIL_SIGNAL a signal it
# kills itself with instead, and FAKE_UTIL_SLEEP how long it stays running
# first, which is how a capture that is stopped from outside is tested. It
# records its arguments before sleeping, so the file appearing means it is up.
FAKE_UTIL = """#!%s
import os
import signal
import sys
import time

with open(os.environ["FAKE_UTIL_ARGS"], "w") as record:
    record.write("\\n".join(sys.argv[1:]))
with open(os.environ["FAKE_UTIL_OUTPUT"]) as listing:
    sys.stdout.write(listing.read())
sys.stdout.flush()
time.sleep(float(os.environ.get("FAKE_UTIL_SLEEP", "0")))
if os.environ.get("FAKE_UTIL_SIGNAL"):
    number = int(os.environ["FAKE_UTIL_SIGNAL"])
    try:
        # Python ignores SIGPIPE, and a signal that is ignored kills nothing
        signal.signal(number, signal.SIG_DFL)
    except (OSError, ValueError):
        pass
    os.kill(os.getpid(), number)
sys.exit(int(os.environ.get("FAKE_UTIL_STATUS", "0")))
"""

# Kept in a name of its own, so its finalizer removes the directory however
# the test ends, including on a failed assertion.
WORK = tempfile.TemporaryDirectory()
directory = WORK.name
# The same fake stands in for both programs the script runs: ubertooth-util
# for the listing, ubertooth-btle for a capture. Neither opens a board, and
# each records the arguments it was called with.
for program in ("ubertooth-util", "ubertooth-btle"):
    fake_program = os.path.join(directory, program)
    with open(fake_program, "w") as fake:
        fake.write(FAKE_UTIL % sys.executable)
    os.chmod(fake_program, 0o755)

ARGS_FILE = os.path.join(directory, "args")
OUTPUT_FILE = os.path.join(directory, "listing")
ENVIRONMENT = dict(
    os.environ,
    PATH=directory + os.pathsep + os.environ.get("PATH", ""),
    FAKE_UTIL_ARGS=ARGS_FILE,
    FAKE_UTIL_OUTPUT=OUTPUT_FILE,
    UBERTOOTH_TEST_NO_DEVICES="1",
)


def run(arguments, listing="", status=0, kill_with=0):
    with open(OUTPUT_FILE, "w") as output:
        output.write(listing)
    if os.path.exists(ARGS_FILE):
        os.remove(ARGS_FILE)
    environment = dict(ENVIRONMENT, FAKE_UTIL_STATUS=str(status),
                       FAKE_UTIL_SIGNAL=str(kill_with) if kill_with else "")
    return subprocess.run(
        [sys.executable, SCRIPT, *arguments],
        capture_output=True,
        text=True,
        # Every run is milliseconds, and there are several dozen, so this
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

# ---- the options Wireshark adds to its own calls ---------------------------

# the start-up call: Wireshark 2.9 and later append their version to it, with
# the value attached and, elsewhere, with no value at all; a long option's
# value can also be the next word. All three forms must leave the rest of the
# command line standing, so the version option comes first here: giving the
# next-word form an empty value as well left "5.0" behind as a stray word,
# which stopped getopt and dropped --extcap-interfaces with it.
for version in (("--extcap-version=5.0",),
                ("--extcap-version", "5.0"),
                ("--extcap-version",)):
    result = run((*version, "--extcap-interfaces"),
                 listing="0: %s\n" % SERIALS[0])
    assert result.returncode == 0, (version, result)
    assert "not recognized" not in result.stderr, (version, result)
    assert interfaces(result) == [
        "interface {value=ubertooth%s}{display=Ubertooth One %s}"
        % ((SERIALS[0],) * 2),
    ], (version, result)
# and the same option with the other calls, in the order Wireshark writes them
result = run(("--extcap-version=4.2", "--extcap-dlts",
              "--extcap-interface", "ubertooth" + SERIALS[0]))
assert result.returncode == 0, result
assert result.stdout.startswith("dlt "), result

# a word that is not an option and not the value of one is a mistake: no call
# here takes an operand, so it is refused rather than silently ignored, the
# same way the other tools refuse one. A bare "-" is a stray word too: it
# names standard output only as the value of a file option, which this script
# has none of.
for stray, word in ((("--extcap-interfaces", "extra"), "extra"),
                    (("--extcap-version", "-", "--extcap-interfaces"), "-"),
                    # -h does not excuse a stray word: it ends the command
                    # only once the whole command line has parsed
                    (("-h", "extra"), "extra"),
                    # the word quoted back is the user's own. After "--"
                    # nothing is an option, so the optional-value rewrite
                    # must not reach it and report a word nobody typed
                    (("--", "--extcap-version"), "--extcap-version"),
                    (("--extcap-interfaces", "--", "extra"), "extra")):
    result = run(stray, listing="0: %s\n" % SERIALS[0])
    assert result.returncode == 1, (stray, result)
    assert result.stdout == "", (stray, result)
    assert result.stderr == "error: unexpected argument '%s'\n" % word, \
        (stray, result)
    assert not os.path.exists(ARGS_FILE), \
        ("a stray word must start nothing", stray, result)

# a capture filter is accepted, so a capture still starts, and is reported as
# not applied rather than quietly dropped
fifo_path = os.path.join(directory, "capture-filter-fifo")
result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
              "--fifo=" + fifo_path, "--extcap-capture-filter", "btle"))
assert result.returncode == 0, result
assert result.stdout == "", result
assert result.stderr.startswith("warning: ") and "btle" in result.stderr, \
    result
with open(ARGS_FILE) as record:
    assert record.read().split("\n")[:3] == ["-f", "-U", SERIALS[0]], result
# an empty one is what Wireshark passes when the user typed no filter
result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
              "--fifo=" + fifo_path, "--extcap-capture-filter="))
assert result.returncode == 0 and result.stderr == "", result

# what Wireshark sends only to a program that advertised the feature behind
# it is refused: this listing advertises no reloadable argument and no toolbar
# control, so these can only arrive by mistake
for option in ("--extcap-reload-option=channel",
               "--extcap-control-in=" + os.devnull,
               "--extcap-control-out=" + os.devnull,
               "--extcap-config-option-name=channel",
               "--extcap-config-option-value=38"):
    result = run(("--extcap-config",
                  "--extcap-interface=ubertooth" + SERIALS[0], option))
    assert result.returncode == 1, (option, result)
    assert result.stdout == "", (option, result)
    assert result.stderr.startswith("error: ") and \
        "not recognized" in result.stderr, (option, result)
    assert "Usage:" in result.stderr, (option, result)

print("btle-extcap: Wireshark's start-up options are accepted")

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

# The fake ubertooth-btle on PATH records its arguments instead of opening a
# board. --capture passes the interface's serial to -U as its own word.
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

# ---- how --capture ends ----------------------------------------------------

# a channel ubertooth-btle will not take is refused here, before any child
# runs, so a refused capture starts nothing and writes nothing to the fifo
for channel in ("40", "36", "0", "37.0", "", "thirty-seven"):
    result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
                  "--fifo=" + fifo, "--channel=" + channel))
    assert result.returncode == 1, (channel, result)
    assert result.stdout == "", (channel, result)
    assert result.stderr == \
        "error: advertising channel index must be 37-39 (got %s)\n" % channel, \
        (channel, result)
    assert not os.path.exists(ARGS_FILE), \
        ("a refused channel must start no child", channel, result)
for channel in ADVERTISING_CHANNELS:
    result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
                  "--fifo=" + fifo, "--channel=" + channel))
    assert result.returncode == 0, (channel, result)
    with open(ARGS_FILE) as record:
        assert record.read().split("\n")[-2:] == ["-A", channel], \
            (channel, result)

# a failed ubertooth-btle is a failed capture, which Wireshark only hears
# about from a non-zero exit here
for status in (1, 2, 42):
    result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
                  "--fifo=" + fifo), status=status)
    assert result.returncode == status, (status, result)

# a child killed by a signal nobody here sent is a failure too, reported the
# way a shell reports it
result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
              "--fifo=" + fifo), kill_with=signal.SIGKILL)
assert result.returncode == 128 + int(signal.SIGKILL), result

# except SIGPIPE: the child gets that when whoever was reading the fifo
# stopped reading, which is one of the ways Wireshark ends a capture. Seen on
# a real board: ubertooth-btle died of SIGPIPE the moment the reader closed
# the fifo, and reporting 141 for it would put an error on every such stop.
result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
              "--fifo=" + fifo), kill_with=signal.SIGPIPE)
assert result.returncode == 0, result

# but a capture Wireshark stopped ended as it was asked to: the script passes
# the SIGTERM on to ubertooth-btle, which dies of it, and the script still
# exits 0 so Wireshark shows no error for a capture the user ended.
with open(OUTPUT_FILE, "w") as output:
    output.write("")
if os.path.exists(ARGS_FILE):
    os.remove(ARGS_FILE)
stopped = subprocess.Popen(
    [sys.executable, SCRIPT, "--capture",
     "--extcap-interface=ubertooth" + SERIALS[0], "--fifo=" + fifo],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    env=dict(ENVIRONMENT, FAKE_UTIL_STATUS="0", FAKE_UTIL_SIGNAL="",
             FAKE_UTIL_SLEEP="60"))
# the child writes its arguments before it sleeps, so the file appearing means
# it is running and the script has it to pass the signal on to
deadline = time.monotonic() + 10
while not os.path.exists(ARGS_FILE) and time.monotonic() < deadline:
    time.sleep(0.05)
if not os.path.exists(ARGS_FILE):
    # nothing to stop, and the long-sleeping fake must not be left behind
    stopped.kill()
    stopped.wait(timeout=10)
    raise AssertionError("the fake ubertooth-btle never started")
stopped.terminate()
output, errors = stopped.communicate(timeout=10)
assert stopped.returncode == 0, (stopped.returncode, output, errors)
assert output == "", output

# a stop that arrives before ubertooth-btle does, while the command line is
# still being read: the handler has no child to pass the signal on to, and
# the capture must not go on to start one afterwards. Driven in-process,
# because that window is microseconds wide from outside.
os.environ.update(ENVIRONMENT)
if os.path.exists(ARGS_FILE):
    os.remove(ARGS_FILE)
loader = importlib.machinery.SourceFileLoader("btle_extcap", SCRIPT)
script = importlib.util.module_from_spec(
    importlib.util.spec_from_loader(loader.name, loader))
# the script lives in the source tree, and a test must not leave a
# __pycache__ behind in it
sys.dont_write_bytecode = True
loader.exec_module(script)
script.sig_handler(signal.SIGTERM, None)
assert script.capture(SERIALS[0], fifo, ADVERTISING_CHANNELS[0]) == 0
assert script.child is None, "a capture already stopped must start no child"
assert not os.path.exists(ARGS_FILE), \
    "a capture already stopped must start no child"

print("btle-extcap: --capture reports how ubertooth-btle ended")

WORK.cleanup()
