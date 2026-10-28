#!/usr/bin/env python3
"""Which stream the usage text and a bad option land on, and the exit status.

Run as: test_output_streams.py <path to one ubertooth-* binary>

The rule:

  * -h is what the user asked for: the usage text goes to standard output,
    nothing goes to standard error, and the status is 0;
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


BINARY = sys.argv[1]
TOOL = os.path.basename(BINARY)
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")

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
                      "(-f, -n or -p), or -a, -v, -s or -t",
    "ubertooth-debug": "error: -r <reg>[,<reg>[,...]] is required: at least "
                       "one register to read",
    "ubertooth-dfu": "error: nothing to do: give -d to write an image, -u to "
                     "read one back, -s to sign one, or -r to reset",
    "ubertooth-ducky": "error: nothing to do: give -q to quack, or -b to "
                       "signal the bootloader",
    "ubertooth-ego": "error: nothing to do: choose a major mode (-f, -r or -i)",
    "ubertooth-tx": "error: -l <LAP> and -u <UAP> are both required",
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
    "could not open ubertooth",
    "no ubertooth found",
    "no board is selected",
    "usb descriptor",
)


def run(arguments):
    return subprocess.run([BINARY] + list(arguments), env=ENVIRONMENT,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=20)


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

# ---- a command line with nothing to do -------------------------------------

checked = ["-h", "a bad option"]

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
         "error: give -t once"),
        (("-l", "445566", "-l", "445566"), "error: give -l once"),
        (("-u", "33", "-u", "33"), "error: give -u once"),
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

print("%s: the rule holds for %s" % (TOOL, " and ".join(checked)))
