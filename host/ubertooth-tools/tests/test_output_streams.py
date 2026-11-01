#!/usr/bin/env python3
"""Which stream the usage text and a bad option land on, and the exit status.

Run as: test_output_streams.py <path to one ubertooth-* binary>

The rule:

  * -h is what the user asked for: the usage text goes to standard output,
    nothing goes to standard error, and the status is 0;
  * so is -V, which every tool answers the same way: the same stream and
    status, and each line says what it is;
  * a bare command line -- no arguments at all -- gets one "error: " line and
    then the usage text, both on standard error, nothing on standard output,
    and the status is 1;
  * every other command line the tool will not take gets that "error: " line
    alone, with no usage text after it: the line describes the problem, and a
    reader who wants the usage text runs -h.

No device is opened in any of those cases, so nothing here depends on what is
attached; UBERTOOTH_TEST_NO_DEVICES=1 hides any board that is, and a run that
reached device selection is caught by the message that reports it.

Every check runs for every tool. A bad option used to be the exception, while
the tools were converted one at a time to report one the shared way
(opterr = 0, an option string starting with ':', opt_report_bad_option());
they all do now, so there is no longer a list of the ones that do.
"""

import os
import subprocess
import sys

try:
    import pty
except ImportError:  # no pty module on this platform
    pty = None


BINARY = sys.argv[1]
TOOL = os.path.basename(BINARY)
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
# How long one run of the tool is given. The whole test takes about 0.2 s per
# tool on an idle machine and 0.3 s on one busy with parallel builds, so twenty
# seconds is many times what any single run needs.
RUN_TIMEOUT_S = 20

# One option per tool that takes a required value, for the missing-value case.
# ubertooth-ducky is left out because test_ducky_options.py already covers its
# missing -U value, in the middle of the rest of its selector parsing.
OPTION_NEEDING_A_VALUE = {
    "ubertooth-afh": "l",
    "ubertooth-btle": "A",
    "ubertooth-debug": "r",
    "ubertooth-dfu": "d",
    "ubertooth-dump": "d",
    "ubertooth-ego": "c",
    "ubertooth-follow": "l",
    "ubertooth-rx": "l",
    "ubertooth-scan": "t",
    "ubertooth-specan": "d",
    "ubertooth-tx": "l",
    "ubertooth-util": "U",
}

# One combination per tool that cannot be carried out, and the whole line it
# is refused with: the "error: " line alone, with no usage text after it.
REFUSED_COMBINATION = {
    "ubertooth-rx": (["-l", "446688", "-z"],
                     "error: survey mode (-z) discovers addresses, so it "
                     "cannot be given -l or -u"),
}

# The tools whose empty command line has nothing to do, and the "error: " line
# each one refuses it with. The rest have work to do with no options at all
# (ubertooth-dump captures, for example), so they are left out of that check.
NOTHING_TO_DO = {
    "ubertooth-afh": "error: -l <LAP> and -u <UAP> are both required: "
                     "the map is detected for one piconet",
    "ubertooth-btle": "error: nothing to do: choose a major mode "
                      "(-f, -n or -p), or -a, -k, -s or -t",
    "ubertooth-debug": "error: nothing to do: give -r to read registers, "
                       "or -w to write them",
    "ubertooth-dfu": "error: nothing to do: give -d to write an image, -u to "
                     "read one back, -s to sign one, or -r to reset",
    "ubertooth-ducky": "error: nothing to do: give -q to quack, or -b to "
                       "signal the bootloader",
    "ubertooth-ego": "error: nothing to do: choose a major mode (-f, -r or -i)",
    "ubertooth-tx": "error: nothing to do: give -g to transmit a generic "
                    "packet, or -l and -u to retransmit to a piconet",
    "ubertooth-util": "error: nothing to do: give an option that reads or sets "
                      "something, or -N to count the attached boards",
}

# The same nothing to do from a command line that did say something. -U is a
# selector and never work of its own, so it is what every tool is given;
# ubertooth-util names the selector in its answer, so it has a line of its own.
SAID_SOMETHING = {
    "ubertooth-util": "error: nothing to do: -U '%s' selects a device but no "
                      "option asks anything of it",
}

# Anything libubertooth prints once a tool starts looking for a device: a
# command with nothing to do must be refused before that happens.
DEVICE_ACCESS = (
    "no ubertooth devices found",
    "no ubertooth found",
    "no board is selected",
    "usb descriptor",
)


def run(arguments):
    return subprocess.run([BINARY] + list(arguments), env=ENVIRONMENT,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=RUN_TIMEOUT_S)


def usage_text(text):
    """The usage text names the tool and lists at least the -U line."""
    return TOOL in text and "-U <index|serial>" in text


# A serial no board has, for a refusal that has to be the command line's and
# not the fault of whichever boards are attached.
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"


# ---- a bad command line ----------------------------------------------------

# Flags that are gone: dead, a documented no-op, a developer scratch hook, or
# superseded by another tool. A removed flag is an option the tool does not
# know, so a script still passing one must fail loudly instead of having it
# taken as something else.
REMOVED_OPTIONS = {
    "ubertooth-afh": ["-a"],
    "ubertooth-util": ["-9", "-f"],
}

# ---- -h, for every tool ----------------------------------------------------

# What the user asked for: the usage text on stdout, nothing on stderr, and
# the status of a command that did what was asked.
result = run(["-h"])
assert result.returncode == 0, result
assert usage_text(result.stdout), result
assert result.stderr == "", result

# an option the tool does not know: the one "error: " line, and no usage
# text after it
result = run(["-Z"])
assert result.returncode == 1, result
# Nothing on stdout: a script reading the tool's output sees no usage text.
assert result.stdout == "", result
assert result.stderr == "error: unrecognized option '-Z'\n", result

# A word starting with -- is quoted as the user wrote it: getopt rejects
# its second '-', which would otherwise be reported as the option "--".
result = run(["--bogus"])
assert result.returncode == 1, result
assert result.stdout == "", result
assert result.stderr.startswith("error: unrecognized option '--bogus':"), result
assert not usage_text(result.stderr), result

# every option the tool used to have, refused the same way
for removed in REMOVED_OPTIONS.get(TOOL, []):
    result = run([removed])
    assert result.returncode == 1, (removed, result)
    assert result.stdout == "", (removed, result)
    assert result.stderr == "error: unrecognized option '%s'\n" % removed, (
        removed, result)

# an option whose value is missing. With ':' leading the option string getopt
# reports that as ':' rather than '?', so a tool that handles only '?' would
# fall through silently: this is the check that catches it.
option = OPTION_NEEDING_A_VALUE.get(TOOL)
if option is not None:
    result = run(["-" + option])
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr == "error: option '-%s' requires a value\n" % option, \
        result

# a combination the tool cannot carry out: that one line and nothing else,
# before any device is opened
combination = REFUSED_COMBINATION.get(TOOL)
if combination is not None:
    arguments, refusal = combination
    result = run(arguments)
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr == refusal + "\n", result
    for message in DEVICE_ACCESS:
        assert message not in result.stderr.lower(), (message, result)

# ---- the version answer ----------------------------------------------------

# Every tool answers -V, and answers it in the same words. Like -h it is what
# the user asked for, so it goes to standard output with nothing on standard
# error. No board is attached here, which the answer says on its own line and
# still exits 0: the host half of it needs no board.
#
# Each line names what it is, so neither can be read as the other.
HOST_VERSION_PREFIX = "Host tools version: libubertooth "
# And the libbtbb the run loaded gets a labelled line of its own, because a
# machine can have a distribution or Homebrew libbtbb beside this fork's and
# the version alone does not say which one is being used.
BTBB_PATH_PREFIX = "libbtbb loaded from: "
NO_BOARD_LINE = "Firmware version: no Ubertooth attached"

result = run(["-V"])
assert result.returncode == 0, result
assert result.stderr == "", result
lines = result.stdout.splitlines()
# Three lines with no board attached: the versions, where libbtbb came from,
# and what the firmware half has to say. Each is labelled, so any of them can
# be grepped for on its own.
assert len(lines) == 3, result
assert lines[0].startswith(HOST_VERSION_PREFIX), result
assert "libbtbb " in lines[0], result
assert lines[1].startswith(BTBB_PATH_PREFIX), result
# A path, not an empty label: the fallback when the loader cannot say is the
# word "unknown".
assert lines[1][len(BTBB_PATH_PREFIX):].strip() != "", result
assert lines[2] == NO_BOARD_LINE, result

# ---- a command line with nothing to do -------------------------------------

checked = ["-h", "-V", "a bad option"]

refusal = NOTHING_TO_DO.get(TOOL)
if refusal is not None:
    checked.append("a bare command line")
    # A bare command line: the "error: " line, then the usage text, since the
    # user has not said anything for the line alone to answer.
    result = run([])
    assert result.returncode == 1, result
    assert result.stdout == "", result
    first, _, rest = result.stderr.partition("\n")
    assert first == refusal, result
    assert usage_text(rest), result
    # refused before any device was looked for
    for message in DEVICE_ACCESS:
        assert message not in result.stderr.lower(), (message, result)

    # The same nothing to do, but the command line did say something: that one
    # line is the whole answer, with no usage text after it.
    said_something = SAID_SOMETHING.get(TOOL, refusal)
    if "%s" in said_something:
        said_something = said_something % NO_SUCH_SERIAL
    result = run(["-U", NO_SUCH_SERIAL])
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr == said_something + "\n", result
    for message in DEVICE_ACCESS:
        assert message not in result.stderr.lower(), (message, result)

# ---- refusals pinned to the exact line and stream --------------------------

# Per tool: command lines whose whole answer is one named line on standard
# error. test_cli_arguments.py checks the wording of many more, but reads the
# two streams together, so a line on the wrong one would pass there.
REFUSED_EXACTLY = {
    "ubertooth-follow": (
        (("-t", "11:22:33:44:55"),
         "error: target address must be xx:xx:xx:xx:xx:xx (got 11:22:33:44:55)"),
        # the refused address is quoted as the user wrote it, in their case
        (("-t", "11:22:33:44:55:GG"),
         "error: target address must be xx:xx:xx:xx:xx:xx "
         "(got 11:22:33:44:55:GG)"),
        (("-t", "11:22:33:44:55:66", "-t", "11:22:33:44:55:66"),
         "error: option '-t' given more than once"),
        (("-l", "445566", "-l", "445566"),
         "error: option '-l' given more than once"),
        (("-u", "33", "-u", "33"),
         "error: option '-u' given more than once"),
        (("-t", "11:22:33:44:55:66", "-l", "445566"),
         "error: -t gives the whole target address, so it cannot be combined "
         "with -l or -u"),
        (("-l", "445566"), "error: -l <LAP> requires -u <UAP>"),
        (("-u", "33"), "error: -u <UAP> requires -l <LAP>"),
        (("-a",),
         "error: -a reads the AFH map over a Bluetooth connection to the "
         "target, so it needs the whole target address: "
         "give -t xx:xx:xx:xx:xx:xx"),
    ),
}

for arguments, expected in REFUSED_EXACTLY.get(TOOL, ()):
    result = run(arguments)
    assert result.returncode == 1, (arguments, result)
    assert result.stdout == "", (arguments, result)
    assert result.stderr == expected + "\n", (arguments, result)
    for message in DEVICE_ACCESS:
        assert message not in result.stderr.lower(), (arguments, message, result)
if REFUSED_EXACTLY.get(TOOL):
    checked.append("%d named refusals" % len(REFUSED_EXACTLY[TOOL]))

# ---- a capture on standard output refuses a terminal -----------------------

# The command line that sends this tool's capture to standard output, and the
# data named in the refusal when standard output is a terminal. Binary there
# fills the screen with control characters and is a mistake rather than a
# stream anyone reads, so it is refused before any device is opened
# (opt_capture_path(), opt_open_dump_file()). ubertooth-btle needs a major
# mode beside the capture to have work to do.
CAPTURE_TO_STDOUT = {
    "ubertooth-btle": ((["-n", "-q", "-"], "binary capture data"),),
    # ubertooth-dump streams to standard output with no -d at all, so the
    # bare command line is refused on a terminal too. -b is refused as well,
    # and is named for what it is: a '0' or a '1' per symbol is text, and
    # the refusal used to call it binary.
    "ubertooth-dump": (([], "binary dump data"),
                       (["-d", "-"], "binary dump data"),
                       (["-b"], "the bitstream dump"),
                       (["-b", "-d", "-"], "the bitstream dump")),
    "ubertooth-follow": ((["-q", "-"], "binary capture data"),
                         (["-d", "-"], "binary dump data")),
    "ubertooth-rx": ((["-q", "-"], "binary capture data"),
                     (["-d", "-"], "binary dump data")),
    "ubertooth-specan": ((["-d", "-"], "binary samples"),),
}

for arguments, what in CAPTURE_TO_STDOUT.get(TOOL, ()):
    if pty is None:
        print("%s: skipping the tty case, no pty module" % TOOL)
        break
    try:
        pty_fd, tty_fd = pty.openpty()
    except OSError:  # a sandbox with no pty to allocate
        print("%s: skipping the tty case, no pty available" % TOOL)
        break
    try:
        result = subprocess.run([BINARY] + arguments, env=ENVIRONMENT,
                                stdin=subprocess.DEVNULL, stdout=tty_fd,
                                stderr=subprocess.PIPE,
                                universal_newlines=True,
                                timeout=RUN_TIMEOUT_S)
    finally:
        os.close(tty_fd)
        os.close(pty_fd)
    assert result.returncode == 1, (arguments, result)
    assert result.stderr == (
        "error: stdout is a tty, refusing to write %s to it\n" % what), \
        (arguments, result)
    # refused before the device was looked for, which is where the capture
    # would otherwise have been opened
    for message in DEVICE_ACCESS:
        assert message not in result.stderr.lower(), (arguments, message,
                                                      result)
else:
    if CAPTURE_TO_STDOUT.get(TOOL):
        checked.append("a capture on standard output refusing a tty")

print("%s: the rule holds for %s" % (TOOL, " and ".join(checked)))
