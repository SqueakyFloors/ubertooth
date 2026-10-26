#!/usr/bin/env python3
"""Which stream the usage text and a bad option land on, and the exit status.

Run as: test_output_streams.py <path to one ubertooth-* binary>

The rule:

  * -h is what the user asked for: the usage text goes to standard output,
    nothing goes to standard error, and the status is 0;
  * a command line the tool will not take, including one with nothing to do,
    gets one "error: " line and then the usage text, both on standard error,
    nothing on standard output, and the status is 1.

No device is opened in any of those cases, so nothing here depends on what is
attached; UBERTOOTH_TEST_NO_DEVICES=1 hides any board that is, and a run that
reached device selection is caught by the message that reports it.

Reporting a bad command line the shared way (opterr = 0, an option string
starting with ':', opt_report_bad_option()) is a smaller step than the whole
rule, so it has a set of its own, REPORTS_BAD_OPTIONS, which FOLLOWS_THE_RULE
is part of. For a tool in it, `-h`, an unknown option, a missing option value,
and every option the tool used to have (REMOVED_OPTIONS) are checked.

Tools are converted one at a time, so a tool in neither set is reported as
pending and nothing is asserted for it: adding a tool to a set is what turns
those checks on for it.
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
    "ubertooth-btle",
    "ubertooth-ducky",
    "ubertooth-dump",
    "ubertooth-ego",
    "ubertooth-specan",
    "ubertooth-tx",
    "ubertooth-util",
}

# The tools that report a bad command line the shared way: opterr = 0, an
# option string starting with ':', and opt_report_bad_option(). That is a
# smaller step than the whole rule, so a tool can be here before it is in
# FOLLOWS_THE_RULE, and the bad-option checks below run for it either way.
REPORTS_BAD_OPTIONS = FOLLOWS_THE_RULE | {
    "ubertooth-rx",
}

# One option per tool that takes a required value, for the missing-value case.
# ubertooth-ducky is left out because test_ducky_options.py already covers its
# missing -U value, in the middle of the rest of its selector parsing.
OPTION_NEEDING_A_VALUE = {
    "ubertooth-afh": "l",
    "ubertooth-btle": "A",
    "ubertooth-dump": "d",
    "ubertooth-ego": "c",
    "ubertooth-rx": "l",
    "ubertooth-specan": "d",
    "ubertooth-tx": "l",
    "ubertooth-util": "U",
}

# The tools whose empty command line has nothing to do, and the "error: " line
# each one refuses it with. The rest have work to do with no options at all
# (ubertooth-dump captures, for example), so they are left out of that check.
NOTHING_TO_DO = {
    "ubertooth-afh": "error: -l <LAP> and -u <UAP> are both required: "
                     "the map is detected for one piconet",
    "ubertooth-btle": "error: nothing to do: choose a major mode "
                      "(-f, -n or -p), or -a, -v, -s or -t",
    "ubertooth-ducky": "error: nothing to do: give -q to quack, or -b to "
                       "signal the bootloader",
    "ubertooth-ego": "error: nothing to do: choose a major mode (-f, -r or -i)",
    "ubertooth-tx": "error: -l <LAP> and -u <UAP> are both required",
    "ubertooth-util": "error: nothing to do: give an option that reads or sets "
                      "something, or -N to count the attached boards",
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


# ---- a bad command line, for every tool that reports one the shared way ----

# Flags that are gone: dead, a documented no-op, a developer scratch hook, or
# superseded by another tool. A removed flag is an option the tool does not
# know, so a script still passing one must fail loudly instead of having it
# taken as something else.
REMOVED_OPTIONS = {
    "ubertooth-afh": ["-a"],
    "ubertooth-util": ["-9", "-f"],
}

if TOOL in REPORTS_BAD_OPTIONS:
    # -h is what the user asked for: the usage text on stdout, nothing on
    # stderr, status 0. The other half of reading a command line properly,
    # so it is checked alongside the ways one can be wrong.
    result = run(["-h"])
    assert result.returncode == 0, result
    assert usage_text(result.stdout), result
    assert result.stderr == "", result

    # an option the tool does not know
    result = run(["-Z"])
    assert result.returncode == 1, result
    # Nothing on stdout: a script reading the tool's output sees no usage text.
    assert result.stdout == "", result
    # The "error: " line comes first, then the usage text, both on stderr.
    first, _, rest = result.stderr.partition("\n")
    assert first == "error: unrecognized option '-Z'", result
    assert usage_text(rest), result

    # A word starting with -- is quoted as the user wrote it: getopt rejects
    # its second '-', which would otherwise be reported as the option "--".
    result = run(["--bogus"])
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr.startswith("error: unrecognized option '--bogus':"), result
    assert usage_text(result.stderr), result

    # every option the tool used to have, refused the same way
    for removed in REMOVED_OPTIONS.get(TOOL, []):
        result = run([removed])
        assert result.returncode == 1, (removed, result)
        assert result.stdout == "", (removed, result)
        first, _, rest = result.stderr.partition("\n")
        assert first == "error: unrecognized option '%s'" % removed, (removed, result)
        assert usage_text(rest), (removed, result)

    # an option whose value is missing
    option = OPTION_NEEDING_A_VALUE.get(TOOL)
    if option is not None:
        result = run(["-" + option])
        assert result.returncode == 1, result
        assert result.stdout == "", result
        first, _, rest = result.stderr.partition("\n")
        assert first == "error: option '-%s' requires a value" % option, result
        assert usage_text(rest), result
elif REMOVED_OPTIONS.get(TOOL):
    sys.exit("%s has removed options but does not report a bad option the "
             "shared way" % TOOL)

if TOOL not in FOLLOWS_THE_RULE:
    if TOOL in REPORTS_BAD_OPTIONS:
        print("%s: streams not converted yet; a bad command line asserted"
              % TOOL)
    else:
        print("%s: output streams not converted yet, nothing asserted" % TOOL)
    sys.exit(0)

# ---- a command line with nothing to do -------------------------------------

refusal = NOTHING_TO_DO.get(TOOL)
if refusal is not None:
    result = run([])
    assert result.returncode == 1, result
    assert result.stdout == "", result
    first, _, rest = result.stderr.partition("\n")
    assert first == refusal, result
    assert usage_text(rest), result
    # refused before any device was looked for
    for message in DEVICE_ACCESS:
        assert message not in result.stderr.lower(), (message, result)

print("%s: output streams and exit status follow the rule" % TOOL)
