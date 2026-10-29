#!/usr/bin/env python3
"""btle-extcap.py's command line and its interface listing, without a board.

Run as: test_btle_extcap.py <path to btle-extcap.py> [path to ubertooth-util]

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

  * a listing that failed, against a fake ubertooth-util that fails the way a
    libusb failure does. Its standard error used to be read and thrown away
    and its exit status never looked at, so a failure came out as an empty
    listing and exit 0, which to Wireshark is exactly "no Ubertooth
    attached". A listing that named one board but could not open another is a
    failure too, and exits 1: Wireshark throws away everything a failed call
    printed, so its capture list is then without the boards that were fine,
    which is the price of saying what happened. The one case that is not a
    failure is no board attached, since the empty listing answers that.

    The line that tells no board apart from a failure is checked against the
    built ubertooth-util when CMake passes its path, so rewording it on the C
    side cannot leave this script matching text nothing prints.

  * a tool that is not on PATH, which used to end the script in a Python
    traceback rather than an "error: " line.

  * --capture runs ubertooth-btle on the selected board, against a fake
    ubertooth-btle that records its arguments, and reports how it ended: any
    failure is exit 1, which is the only thing Wireshark needs to hear about
    a failed capture, while a capture asked to stop exits 0. Both stop
    signals are checked: Wireshark's SIGTERM and tshark's Ctrl-C, which
    sends SIGINT to the process group and used to kill the script outright.

  * each call as Wireshark itself writes it, including the capture-filter
    verification call, which is the filter and an interface with no
    operation. Wireshark reads that call's standard output as the filter's
    problem, so answering with nothing left the user a filter box that
    looked accepted and a capture with no filter applied.

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
# The built ubertooth-util, when the caller has one to offer.
REAL_UTIL = sys.argv[2] if len(sys.argv) > 2 else None
# ubertooth-util's man page, which is where the shape of the device listing
# this script parses is written down. Required: a check that can vanish when
# the file is not where it was looked for is no check.
MAN_PAGE = sys.argv[3]
# How long one run of the script, or one wait on a child it started, is given.
# The whole test takes about 8 s on an idle machine, almost all of it starting
# the interpreter once for each of some eighty runs, and 25 s on a machine busy
# with parallel builds. Twenty seconds is therefore many times what any single
# run needs, while still leaving a hung run room to be reported inside this
# test's own CTest timeout: that is what sizes it, rather than a multiple of
# the 8 s above, since eighty runs cannot each be given a multiple of the whole.
RUN_TIMEOUT_S = 20
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
# What "ubertooth-util -U all -s" says when no board is attached, which is the
# only thing telling that apart from a failed listing. The cases below pin
# that the script suppresses exactly this line, and REAL_UTIL pins that this
# is what ubertooth-util prints, so the two cannot drift apart silently.
NO_DEVICES = "error: no Ubertooth devices found"
# What libubertooth says for a board it could not open, which is what a
# partial listing reports beside the boards it could name.
OPEN_REFUSED = "error: Access denied (insufficient permissions) (libusb -3)\n"
# What the script answers Wireshark's capture-filter verification call with,
# and repeats as a warning on a capture started with a filter anyway. Pinned
# here so the two cannot drift apart: Wireshark shows this one sentence in the
# capture filter box.
NO_CAPTURE_FILTER = ("this interface does not apply capture filters;"
                     " use a display filter")
SERIALS = ("1ff0ff1967102caf8334fa5ac52000f5",
           "0b700017490c2cafa039ee5ac52000f5")

# The shape of "ubertooth-util -U all -s", which is what the script parses.
LISTING_ONE = "Serial No (0): %s\n"
LISTING = LISTING_ONE + "Serial No (1): %s\n"

# ubertooth-util's man page is the one place that shape is written down, and
# the repository requires it to be updated with the tool. The lines it shows
# are run through the script further down, so a change to the listing that
# leaves this script's parser behind fails here instead of silently costing
# Wireshark its capture interface.
DOCUMENTED_LINE = re.compile(r'^ +(Serial No \(\d+\): [0-9a-f]{32})$', re.M)

# A fake ubertooth-util: prints the lines the test asks for and records the
# arguments it was called with, so no board is involved. It runs under the
# interpreter running this test, which is the one CMake selected, not whatever
# python3 is first on PATH.
#
# FAKE_UTIL_STATUS is the status it exits with, FAKE_UTIL_SIGNAL a signal it
# kills itself with instead, and FAKE_UTIL_SLEEP how long it stays running
# first, which is how a capture that is stopped from outside is tested. It
# records its arguments before sleeping, so the file appearing means it is up.
# FAKE_UTIL_ERRORS is what it writes to standard error, which is where a real
# ubertooth-util reports a failure.
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
sys.stderr.write(os.environ.get("FAKE_UTIL_ERRORS", ""))
sys.stderr.flush()
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


def wait_for_fake():
    """Wait until the fake tool has written its arguments, which is how a case
    knows the script has a child to pass a signal on to. True if it started,
    False if the deadline passed first."""
    deadline = time.monotonic() + RUN_TIMEOUT_S
    while time.monotonic() < deadline:
        if os.path.exists(ARGS_FILE):
            return True
        time.sleep(0.05)
    return os.path.exists(ARGS_FILE)


def run(arguments, listing="", status=0, kill_with=0, errors="", path=None):
    with open(OUTPUT_FILE, "w") as output:
        output.write(listing)
    if os.path.exists(ARGS_FILE):
        os.remove(ARGS_FILE)
    # int() before str(): kill_with is a signal.Signals member, and str() on
    # one gives "Signals.SIGTERM" before Python 3.11 and "15" from 3.11 on,
    # while the fake reads the number back with int().
    kill_signal = str(int(kill_with)) if kill_with else ""
    environment = dict(ENVIRONMENT, FAKE_UTIL_STATUS=str(status),
                       FAKE_UTIL_SIGNAL=kill_signal,
                       FAKE_UTIL_ERRORS=errors)
    if path is not None:
        environment["PATH"] = path
    return subprocess.run(
        [sys.executable, SCRIPT, *arguments],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_S,
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
    result = run((argument,), listing=LISTING_ONE % SERIALS[0])
    assert "unrecognized option" not in (result.stdout + result.stderr), \
        (argument, result)
    assert result.returncode != 2, (argument, result)
print("btle-extcap: the usage line names %d options, all accepted" % len(named))

# an option that really is unknown still says so: one "error: " line on
# standard error, with no usage line after it, nothing on standard output,
# exit status 1. The wording is the one every other tool here gives for an
# option it does not know, not getopt's.
unknown = run(("--list-dlts",))
assert unknown.returncode == 1, unknown
assert unknown.stdout == "", unknown
assert unknown.stderr == "error: unrecognized option '--list-dlts'\n", unknown
assert "Usage:" not in unknown.stderr, unknown

# and a single letter it does not know, and a long option whose value is
# missing, read the same way they do in the C tools
unknown = run(("-Z",))
assert unknown.returncode == 1, unknown
assert unknown.stderr == "error: unrecognized option '-Z'\n", unknown
# a single letter that is also the start of two long options is still the
# single letter the user typed, not an abbreviation of either
unknown = run(("-c",))
assert unknown.returncode == 1, unknown
assert unknown.stderr == "error: unrecognized option '-c'\n", unknown
missing = run(("--fifo",))
assert missing.returncode == 1, missing
assert missing.stderr == "error: option '--fifo' requires a value\n", missing
# an option that takes no value, given one
extra = run(("--capture=yes",))
assert extra.returncode == 1, extra
assert extra.stderr == "error: option '--capture' takes no value\n", extra
# an abbreviation that could be either of two options
short = run(("--extcap-c",))
assert short.returncode == 1, short
assert short.stderr == \
    "error: option '--extcap-c' is short for more than one option\n", short

# an interface and no operation to carry out with it: the one "error: " line,
# and the call fails rather than exiting 0 having done nothing
nothing = run(("--extcap-interface=ubertooth" + SERIALS[0],))
assert nothing.returncode == 1, nothing
assert nothing.stdout == "", nothing
assert nothing.stderr.startswith("error: nothing to do: give"), nothing
assert "Usage:" not in nothing.stderr, nothing

# a bare call is the one refusal the usage line follows, on standard error
bare = run(())
assert bare.returncode == 1, bare
assert bare.stdout == "", bare
assert bare.stderr.startswith("error: nothing to do: give"), bare
assert "Usage:" in bare.stderr, bare

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
                 listing=LISTING_ONE % SERIALS[0])
    assert result.returncode == 0, (version, result)
    assert "unrecognized option" not in result.stderr, (version, result)
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
# names standard output as the value of --fifo, not on its own.
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
    result = run(stray, listing=LISTING_ONE % SERIALS[0])
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
assert result.stderr == "warning: %s (got btle)\n" % NO_CAPTURE_FILTER, result
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
    assert result.stderr.startswith("error: unrecognized option '--"), \
        (option, result)
    assert "Usage:" not in result.stderr, (option, result)

print("btle-extcap: Wireshark's start-up options are accepted")

# ---- the interface listing -------------------------------------------------

# several boards: one line each, named and shown by the board's serial
result = run(("--extcap-interfaces",),
             listing=LISTING % SERIALS)
assert result.returncode == 0, result
assert interfaces(result) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[0],) * 2),
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[1],) * 2),
], result
with open(ARGS_FILE) as record:
    assert record.read().split("\n") == ["-U", "all", "-s"], \
        "the listing must be asked for explicitly with -U all"

# one board: the same form, so there is one parser rather than two
result = run(("--extcap-interfaces",), listing=LISTING_ONE % SERIALS[0])
assert interfaces(result) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[0],) * 2),
], result

# a board that could not be read has no serial to offer, so it is left out,
# while the board that has one is still listed. ubertooth-util exits 1 for the
# board it could not open, with the message libubertooth prints for one, and
# the script exits 1 with it: a partial failure is a failure. The listing and
# the reason are both still written, for whoever runs the call by hand
result = run(("--extcap-interfaces",),
             listing="Serial No (0): unavailable, access to the board was "
                     "denied\nSerial No (1): %s\n" % SERIALS[1],
             errors=OPEN_REFUSED, status=1)
assert result.returncode == 1, result
assert interfaces(result) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[1],) * 2),
], result
assert result.stderr == OPEN_REFUSED, result

# the same listing with ubertooth-util exiting 0: nothing failed, so neither
# does the script, the board without a serial is still left out, and no
# diagnosis is added for it
result = run(("--extcap-interfaces",),
             listing="Serial No (0): unavailable, access to the board was "
                     "denied\nSerial No (1): %s\n" % SERIALS[1])
assert result.returncode == 0, result
assert interfaces(result) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[1],) * 2),
], result
assert result.stderr == "", result

# a partial listing whose tool was killed by a signal is a failure too, and
# names the signal
result = run(("--extcap-interfaces",), listing=LISTING_ONE % SERIALS[0],
             kill_with=signal.SIGTERM)
assert result.returncode == 1, result
assert len(interfaces(result)) == 1, result
assert result.stderr == "error: ubertooth-util failed (killed by signal %d)\n" \
    % int(signal.SIGTERM), result

# nothing that is not a listing line is taken for a serial
result = run(("--extcap-interfaces",),
             listing="ubertooth-util: command not found\nSerial No (0):\n"
                     "Serial No (0): \n0: %s\nSerial No: %s\n"
                     % (SERIALS[0], SERIALS[0]))
assert interfaces(result) == [], result

# the listing lines ubertooth-util's own man page shows, parsed by this script
with open(MAN_PAGE) as page:
    documented = DOCUMENTED_LINE.findall(page.read())
assert documented, \
    "%s no longer shows a listing line; it is where the shape this script " \
    "parses is written down" % MAN_PAGE
for line in sorted(set(documented)):
    result = run(("--extcap-interfaces",), listing=line + "\n")
    assert len(interfaces(result)) == 1, (line, result)
print("btle-extcap: the %d listing lines the man page shows are parsed"
      % len(set(documented)))

print("btle-extcap: the interface listing reports each board's serial")

# ---- a listing that failed -------------------------------------------------

# no board at all: ubertooth-util exits 1 and says so, and an empty listing is
# the right answer to that, so the script exits 0. Its one error line is not
# passed on, since nothing failed
result = run(("--extcap-interfaces",), errors=NO_DEVICES + "\n", status=1)
assert result.returncode == 0, result
assert result.stdout == "" and result.stderr == "", result

# and whatever else ubertooth-util wrote still reaches the user: a note is
# what UBERTOOTH_TEST_NO_DEVICES adds to the same listing
result = run(("--extcap-interfaces",),
             errors="note: UBERTOOTH_TEST_NO_DEVICES is set\n"
                    + NO_DEVICES + "\n", status=1)
assert result.returncode == 0, result
assert result.stdout == "", result
assert result.stderr == "note: UBERTOOTH_TEST_NO_DEVICES is set\n", result

# a ubertooth-util that failed for any other reason listed nothing because it
# failed, not because there is no board: the status is 1 and the reason is
# passed through. This used to be an empty listing and exit 0,
# indistinguishable from no board being attached
for reason in ("error: libusb_init failed\n",
               NO_DEVICES + "\nerror: libusb_init failed\n"):
    result = run(("--extcap-interfaces",), errors=reason, status=1)
    assert result.returncode == 1, (reason, result)
    assert interfaces(result) == [], (reason, result)
    assert result.stderr == reason, (reason, result)

# one that failed without saying why still has to name the failure, otherwise
# the reason never reaches the user, which is the whole complaint. A note or a
# warning is not a reason, so one of those on its own does not stand in for it
for said in ("", "note: something odd\n", "warning: something odd\n", "\n"):
    result = run(("--extcap-interfaces",), errors=said, status=3)
    assert result.returncode == 1, (said, result)
    assert result.stdout == "", (said, result)
    assert result.stderr == \
        said + "error: ubertooth-util failed (exit status 3)\n", (said, result)

# a diagnosis with no "error: " prefix is still a reason, since libubertooth
# writes some of its detail lines without one: nothing is added to it
UNOPENED = "1 attached Ubertooth device could not be opened\n"
result = run(("--extcap-interfaces",), errors=UNOPENED, status=1)
assert result.returncode == 1, result
assert result.stderr == UNOPENED, result
# a tool killed by a signal is reported with the signal's number. SIGTERM is
# the signal to send here: an aborted or faulted process gets a crash report
# from macOS, and a dialog on the screen of whoever runs the tests
result = run(("--extcap-interfaces",), kill_with=signal.SIGTERM)
assert result.returncode == 1, result
assert result.stderr == "error: ubertooth-util failed (killed by signal %d)\n" \
    % int(signal.SIGTERM), result

# a failure that still listed a board is a failure, and one that said nothing
# of itself still has to name the failure, exactly as an empty one does
result = run(("--extcap-interfaces",), listing=LISTING_ONE % SERIALS[0],
             status=3)
assert result.returncode == 1, result
assert len(interfaces(result)) == 1, result
assert result.stderr == "error: ubertooth-util failed (exit status 3)\n", result

# a failure that still listed a board keeps both the board and the reason on
# their streams, and exits 1
result = run(("--extcap-interfaces",), listing=LISTING_ONE % SERIALS[0],
             errors="error: libusb_init failed\n", status=1)
assert result.returncode == 1, result
assert interfaces(result) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}" % ((SERIALS[0],) * 2),
], result
assert result.stderr == "error: libusb_init failed\n", result

# a warning from a listing that worked reaches the user too, and is not a
# failure
result = run(("--extcap-interfaces",), listing=LISTING_ONE % SERIALS[0],
             errors="warning: something to know\n")
assert result.returncode == 0, result
assert len(interfaces(result)) == 1, result
assert result.stderr == "warning: something to know\n", result

print("btle-extcap: a failed listing exits 1 and says why")

# The script matches that one line to tell no board apart from a failure, so
# the line has to be the one ubertooth-util prints. UBERTOOTH_TEST_NO_DEVICES
# makes it report no board without one being attached, and without opening a
# board this machine may be using.
if REAL_UTIL:
    real = subprocess.run(
        [REAL_UTIL, "-U", "all", "-s"],
        capture_output=True, text=True, timeout=RUN_TIMEOUT_S, check=False,
        env=dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1"))
    assert real.returncode == 1, real
    assert NO_DEVICES in real.stderr.splitlines(), (NO_DEVICES, real)
    print("btle-extcap: ubertooth-util still reports %r" % NO_DEVICES)

# ---- a tool that is not on PATH --------------------------------------------

# Nothing is on this PATH, so the script cannot run either tool. That used to
# end it in a Python traceback; the message has to be the same one-line form
# as every other error here. The script itself runs under this interpreter by
# path, so an empty PATH costs it nothing.
EMPTY = os.path.join(directory, "empty")
os.mkdir(EMPTY)
for arguments, tool in (
        (("--extcap-interfaces",), "ubertooth-util"),
        (("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
          "--fifo=" + os.devnull), "ubertooth-btle")):
    result = run(arguments, path=EMPTY)
    assert result.returncode == 1, (tool, result)
    assert result.stdout == "", (tool, result)
    assert "Traceback" not in result.stderr, (tool, result)
    assert result.stderr == \
        "error: could not run %s: No such file or directory\n" % tool, \
        (tool, result)

# a tool that is there but cannot be executed is reported the same way
NOT_EXECUTABLE = os.path.join(directory, "not-executable")
os.mkdir(NOT_EXECUTABLE)
with open(os.path.join(NOT_EXECUTABLE, "ubertooth-util"), "w") as blocked:
    blocked.write("#!/bin/sh\nexit 0\n")
os.chmod(os.path.join(NOT_EXECUTABLE, "ubertooth-util"), 0o644)
result = run(("--extcap-interfaces",), path=NOT_EXECUTABLE)
assert result.returncode == 1, result
assert result.stdout == "" and "Traceback" not in result.stderr, result
assert result.stderr.startswith("error: could not run ubertooth-util: "), \
    result

print("btle-extcap: a tool that is not on PATH is reported, not a traceback")

# ---- -V, which the script answers by running ubertooth-util ----------------
#
# The same fake stands in for it, printing the answer the test gives it, so
# no board is involved. What is checked is that the script passes
# ubertooth-util's answer through rather than writing a version of its own.

# What the fake answers with. Deliberately not the real lines, which
# ubertooth(7), COMMON OPTIONS, describes: those are the C side's words, held
# to the C tests by host/ubertooth-tools/tests/test_doc_example_lines.py, and
# a copy of them here would let that check pass against this fake instead.
# What this pins is that every line arrives unchanged, whatever it says.
VERSION_ANSWER = ("the first line ubertooth-util printed\n"
                  "the second line ubertooth-util printed\n"
                  "the third line ubertooth-util printed\n")

# -V on its own: the answer, unchanged, on standard output, and exit 0. It
# asks ubertooth-util for the version and nothing else; the script has no -U,
# so no board is named and every attached board is reported.
result = run(("-V",), listing=VERSION_ANSWER)
assert result.returncode == 0, result
assert result.stdout == VERSION_ANSWER, result
assert result.stderr == "", result
with open(ARGS_FILE) as record:
    assert record.read().split("\n") == ["-V"], result

# a board that is attached and could not be read exits 1 there, and so here:
# its line says why, and the answer is still printed
result = run(("-V",), listing="Firmware version: read failed\n", status=1)
assert result.returncode == 1, result
assert result.stdout == "Firmware version: read failed\n", result

# -V beside -h prints both, the version first, and exits 0
result = run(("-h", "-V"), listing=VERSION_ANSWER)
assert result.returncode == 0, result
assert result.stdout.startswith(VERSION_ANSWER), result
assert result.stdout.rstrip("\n").endswith("-V | -h"), result
assert result.stderr == "", result

# ubertooth-util not on the PATH: one error line, not a traceback
result = run(("-V",), listing=VERSION_ANSWER, path=EMPTY)
assert result.returncode == 1, result
assert result.stdout == "", result
assert "Traceback" not in result.stderr, result
assert result.stderr == \
    "error: could not run ubertooth-util: No such file or directory\n", result

# and the usage line beside it is still printed, since -h was asked for too:
# a version that could not be read does not take the rest of the answer with
# it. The status is the version's.
result = run(("-h", "-V"), listing=VERSION_ANSWER, path=EMPTY)
assert result.returncode == 1, result
assert result.stdout.startswith("Usage:"), result
assert result.stderr == \
    "error: could not run ubertooth-util: No such file or directory\n", result

print("btle-extcap: -V passes ubertooth-util's answer through")

# -h ends the call ahead of every operation, whichever order they were
# written in: the listing used to be answered first when it came first, so -h
# printed nothing and Wireshark's interface list was the answer instead.
result = run(("--extcap-interfaces", "-h"), listing=LISTING_ONE % SERIALS[0])
assert result.returncode == 0, result
assert result.stdout.startswith("Usage:"), result
assert interfaces(result) == [], result
assert not os.path.exists(ARGS_FILE), "ubertooth-util was run anyway"

print("btle-extcap: -h ends the call ahead of the interface listing")

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
# about from a non-zero exit here. Every failure is 1: Wireshark needs no
# more than non-zero, and a wrapper does not invent exit statuses of its own
# for a child's (.kiro/steering/cli-output.md). The child's status used to be
# passed through, so a capture could end 42.
for status in (1, 2, 42):
    result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
                  "--fifo=" + fifo), status=status)
    assert result.returncode == 1, (status, result)

# a child killed by a signal nobody here sent is a failure too, and the same
# 1 rather than 128 plus the signal's number
result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
              "--fifo=" + fifo), kill_with=signal.SIGKILL)
assert result.returncode == 1, result

# except SIGPIPE: the child gets that when whoever was reading the fifo
# stopped reading, which is one of the ways Wireshark ends a capture. Seen on
# a real board: ubertooth-btle died of SIGPIPE the moment the reader closed
# the fifo, and reporting 141 for it would put an error on every such stop.
result = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
              "--fifo=" + fifo), kill_with=signal.SIGPIPE)
assert result.returncode == 0, result

# but a capture that was asked to stop ended as it was asked to: the script
# passes the signal on to ubertooth-btle, which dies of it, and the script
# still exits 0 so Wireshark shows no error for a capture the user ended.
# SIGTERM is what Wireshark stops a capture with; SIGINT is what tshark's
# Ctrl-C sends to the whole process group, and the script used to die of it
# with nothing to report, since only SIGTERM was handled.
for stop_with in (signal.SIGTERM, signal.SIGINT):
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
    # the child writes its arguments before it sleeps, so the file appearing
    # means it is running and the script has it to pass the signal on to
    if not wait_for_fake():
        # nothing to stop, so end the script rather than wait on it. The fake
        # it may have started is left to finish its own sleep.
        stopped.kill()
        stopped.wait(timeout=RUN_TIMEOUT_S)
        raise AssertionError(
            "timed out after %d s: the fake ubertooth-btle never started"
            % RUN_TIMEOUT_S)
    stopped.send_signal(stop_with)
    output, errors = stopped.communicate(timeout=RUN_TIMEOUT_S)
    assert stopped.returncode == 0, \
        (stop_with, stopped.returncode, output, errors)
    assert output == "", (stop_with, output)
    # no traceback either: the signal is handled, not left to kill Python
    assert "Traceback" not in errors, (stop_with, errors)

# a call that is not a capture keeps the default disposition, where a stop
# signal ends the command: only --capture has a stop to report, so that is
# the only call that handles one. Handling them for every call left a stop
# noted and ignored, so a listing waiting on ubertooth-util carried on and
# exited 0.
with open(OUTPUT_FILE, "w") as output:
    output.write("")
if os.path.exists(ARGS_FILE):
    os.remove(ARGS_FILE)
listing = subprocess.Popen(
    [sys.executable, SCRIPT, "--extcap-interfaces"],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    env=dict(ENVIRONMENT, FAKE_UTIL_STATUS="0", FAKE_UTIL_SIGNAL="",
             # The stop has to land while the listing still waits on this
             # sleep, and the few lines between seeing the fake start and
             # sending the stop can themselves take seconds on a machine busy
             # with parallel builds. Ten seconds covers that, and a fake that
             # outlives the test only finishes its own sleep in a temporary
             # directory.
             FAKE_UTIL_SLEEP="10"))
started = wait_for_fake()
listing.terminate()
output, errors = listing.communicate(timeout=RUN_TIMEOUT_S)
# the stop has to land while the listing waits on ubertooth-util, so a fake
# that never ran leaves nothing to check: say that, rather than failing on the
# exit status of a script that was already finished.
assert started, \
    "timed out after %d s: the fake ubertooth-util never started" \
    % RUN_TIMEOUT_S
assert listing.returncode == -int(signal.SIGTERM), \
    (listing.returncode, output, errors)

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
for stop_with in (signal.SIGTERM, signal.SIGINT):
    script.stop_signal = None
    script.child = None
    script.sig_handler(stop_with, None)
    assert script.capture(SERIALS[0], fifo, ADVERTISING_CHANNELS[0]) == 0, \
        stop_with
    assert script.child is None, \
        ("a capture already stopped must start no child", stop_with)
    assert not os.path.exists(ARGS_FILE), \
        ("a capture already stopped must start no child", stop_with)

print("btle-extcap: --capture reports how ubertooth-btle ended")

# ---- the calls Wireshark makes ---------------------------------------------

# Each call as Wireshark writes it, values in the next word rather than
# attached, in the order extcap.c appends them. All five answer on standard
# output and exit 0: Wireshark throws away what a call that failed printed.
listing_call = run(("--extcap-interfaces", "--extcap-version=5.0"),
                   listing=LISTING_ONE % SERIALS[0])
assert listing_call.returncode == 0, listing_call
assert interfaces(listing_call) == [
    "interface {value=ubertooth%s}{display=Ubertooth One %s}"
    % ((SERIALS[0],) * 2)], listing_call

dlts_call = run(("--extcap-dlts", "--extcap-interface",
                 "ubertooth" + SERIALS[0]))
assert dlts_call.returncode == 0, dlts_call
assert dlts_call.stdout.startswith(
    "dlt {number=256}{name=DLT_BLUETOOTH_LE_LL_WITH_PHDR}"), dlts_call
assert dlts_call.stderr == "", dlts_call

config_call = run(("--extcap-config", "--extcap-interface",
                   "ubertooth" + SERIALS[0]))
assert config_call.returncode == 0, config_call
assert config_call.stderr == "", config_call
config_lines = config_call.stdout.splitlines()
assert config_lines[0] == \
    "arg {number=0}{call=--channel}{display=Advertising Channel}" \
    "{type=selector}", config_call
# the channel --capture uses without --channel is the one offered first, and
# it is the default Wireshark starts with
assert config_lines[1:] == [
    "value {arg=0}{value=%s}{display=%s}{default=%s}"
    % (ch, ch, "true" if ch == ADVERTISING_CHANNELS[0] else "false")
    for ch in ADVERTISING_CHANNELS], config_call

# the capture call, with the configured channel as its own word
capture_fifo = os.path.join(directory, "wireshark-fifo")
capture_call = run(("--capture", "--extcap-interface",
                    "ubertooth" + SERIALS[0], "--fifo", capture_fifo,
                    "--channel", "39"))
assert capture_call.returncode == 0, capture_call
assert capture_call.stdout == "" and capture_call.stderr == "", capture_call
with open(ARGS_FILE) as record:
    assert record.read().split("\n") == \
        ["-f", "-U", SERIALS[0], "-q", capture_fifo, "-A", "39"], capture_call

# what ubertooth-btle writes to its own standard output is dropped: the data
# this call asked for goes to the fifo, and its -f decode of every packet
# would otherwise land on the script's standard output, where only the
# listings belong. Passing it on to standard error instead would crowd out
# the reason a failed capture gives there, of which Wireshark keeps 1024
# bytes. The fake writes the listing to its standard output, so that is the
# noise here.
noisy = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
             "--fifo=" + capture_fifo),
            listing="a decode of a packet, 560 kB of them in 8 seconds\n")
assert noisy.returncode == 0, noisy
assert noisy.stdout == "" and noisy.stderr == "", noisy

# except for a fifo of "-", which is that standard output: ubertooth-btle
# writes the capture there, so dropping it would throw the capture away, and
# it prints no decode beside it. A capture run by hand, never one Wireshark
# asked for.
streamed = run(("--capture", "--extcap-interface=ubertooth" + SERIALS[0],
                "--fifo=-"), listing="the capture data\n")
assert streamed.returncode == 0, streamed
assert streamed.stdout == "the capture data\n", streamed
with open(ARGS_FILE) as record:
    assert record.read().split("\n")[3:5] == ["-q", "-"], streamed

# a filter beside a call that has work to do is not the verification call:
# the work is done, and the filter is nothing to that call
for operation, answer in (("--extcap-dlts", "dlt "),
                          ("--extcap-config", "arg ")):
    result = run((operation, "--extcap-interface=ubertooth" + SERIALS[0],
                  "--extcap-capture-filter=btle"))
    assert result.returncode == 0, (operation, result)
    assert result.stdout.startswith(answer), (operation, result)
    assert result.stderr == "", (operation, result)

# the capture-filter verification call: the filter and an interface, with no
# operation. Wireshark reads what this call writes to standard output as the
# filter's problem and shows it in the filter box, in red, and an empty answer
# as a filter that is good; it discards the output of a call that exited
# non-zero and says "Unable to check capture filter" instead, so the call
# exits 0 either way. It used to answer with an empty standard output, which
# left the user with a filter box that looked accepted and an unfiltered
# capture.
for filter_call in (("--extcap-capture-filter", "btle",
                     "--extcap-interface", "ubertooth" + SERIALS[0]),
                    ("--extcap-interface=ubertooth" + SERIALS[0],
                     "--extcap-capture-filter=ether host 00:11:22:33:44:55")):
    result = run(filter_call)
    assert result.returncode == 0, (filter_call, result)
    assert result.stdout == NO_CAPTURE_FILTER + "\n", (filter_call, result)
    assert result.stderr == "", (filter_call, result)
    assert not os.path.exists(ARGS_FILE), \
        ("verifying a filter must start nothing", filter_call, result)

# the empty filter Wireshark verifies when the box is empty is good: nothing
# on standard output is what says so
for filter_call in (("--extcap-capture-filter", "",
                     "--extcap-interface", "ubertooth" + SERIALS[0]),
                    ("--extcap-interface=ubertooth" + SERIALS[0],
                     "--extcap-capture-filter=")):
    result = run(filter_call)
    assert result.returncode == 0, (filter_call, result)
    assert result.stdout == "" and result.stderr == "", (filter_call, result)

# an interface with no operation and no filter is still a call that did
# nothing, which fails rather than looking like a verified empty filter
nothing = run(("--extcap-interface=ubertooth" + SERIALS[0],))
assert nothing.returncode == 1 and nothing.stdout == "", nothing

print("btle-extcap: every call Wireshark makes is answered on standard output")

WORK.cleanup()
