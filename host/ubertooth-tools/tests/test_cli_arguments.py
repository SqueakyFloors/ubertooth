#!/usr/bin/env python3
"""The one argument rule, per tool, with no Ubertooth attached.

Run as: test_cli_arguments.py <path to one ubertooth-* binary>

Every case here must be refused while the command line is being read, before
the tool looks for a device, so each run is checked for the library's
device-access messages as well as for its own diagnostic.

The tools run with UBERTOOTH_TEST_NO_DEVICES=1, which hides attached boards
from device selection. A case that slipped past argument checking then reports
that it found no device, on any machine, instead of opening whatever happens to
be attached (possibly a board another process is using) and printing whichever
message that produced.

Four kinds of case:
  * a stray word, including a bare "-": "error: unexpected argument '<word>'";
  * a bad option value: the option's own "error: ... must be ..." line;
  * an optional value written as the next word: the value is consumed, which
    a trailing "extra" proves by being the word the tool complains about;
  * an option combination the tool cannot carry out (a missing companion
    option, or two major modes at once): refused here too, so the diagnostic
    does not depend on which boards are attached.
"""

import os
import subprocess
import sys


BINARY = sys.argv[1]
TOOL = os.path.basename(BINARY)
# A serial no board has, for the cases that prove an argument error does not
# depend on which boards are attached.
NO_SUCH_SERIAL = "ffffffffffffffffffffffffffffffff"
# Hide attached boards from the tool: see the module docstring.
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")

# Anything libubertooth prints once a tool starts looking for a device.
DEVICE_ACCESS = (
    "could not open ubertooth device",
    "no ubertooth found",
    "multiple ubertooth devices found",
    "usb descriptor",
)

STRAY = (
    (("extra",), "error: unexpected argument 'extra'"),
    (("-",), "error: unexpected argument '-'"),
    # -h does not excuse a stray word either: it ends the command only after
    # the whole command line has parsed
    (("-h", "extra"), "error: unexpected argument 'extra'"),
)

VERSION_STRAY = (
    (("-V", "extra"), "error: unexpected argument 'extra'"),
)

LAP_UAP = (
    (("-l", "zz3344"), "error: lap must be 6 hex digits (got zz3344)"),
    (("-l", "44668"), "error: lap must be 6 hex digits (got 44668)"),
    (("-l", "4466889"), "error: lap must be 6 hex digits (got 4466889)"),
    (("-u", "2"), "error: uap must be 2 hex digits (got 2)"),
    (("-u", "2g"), "error: uap must be 2 hex digits (got 2g)"),
)

AC_ERRORS = (
    (("-e", "6"), "error: maximum access code errors must be 0-5 (got 6)"),
    (("-e", "abc"), "error: maximum access code errors must be 0-5 (got abc)"),
)

TIMEOUT_SECONDS = (
    (("-t", "abc"), "error: timeout must be a whole number of seconds, 0 for none (got abc)"),
    (("-t", "-1"), "error: timeout must be a whole number of seconds, 0 for none (got -1)"),
)

ADV_INDEX = (
    (("-A", "40"), "error: advertising channel index must be 37-39 (got 40)"),
    (("-A", "abc"), "error: advertising channel index must be 37-39 (got abc)"),
)

CASES = {
    "ubertooth-afh": STRAY + VERSION_STRAY + LAP_UAP + AC_ERRORS + TIMEOUT_SECONDS + (
        (("-m", "0"), "error: channel removal threshold must be a whole number of packets, 1 or more (got 0)"),
        (("-m", "5x"), "error: channel removal threshold must be a whole number of packets, 1 or more (got 5x)"),
        # a valid piconet address still reports the stray word
        (("-l", "446688", "-u", "22", "extra"),
         "error: unexpected argument 'extra'"),
    ),
    "ubertooth-btle": STRAY + ADV_INDEX + (
        # one major mode per command, refused before a device is opened
        (("-f", "-n"), "error: choose one of -f, -n or -p"),
        (("-n", "-p"), "error: choose one of -f, -n or -p"),
        (("-f", "-n", "-p"), "error: choose one of -f, -n or -p"),
        (("-v", "2"), "error: crc verify mode must be 0-1 (got 2)"),
        (("-vabc",), "error: crc verify mode must be 0-1 (got abc)"),
        # a word that is not a value for -v is not taken as one
        (("-v", "abc"), "error: unexpected argument 'abc'"),
        (("-x", "33"), "error: access address offenses must be 0-32 (got 33)"),
        (("-x", "abc"), "error: access address offenses must be 0-32 (got abc)"),
        (("-azzzz",), "error: access address must be 1-8 hex digits (got zzzz)"),
        (("-a", "zzzz"), "error: access address must be 1-8 hex digits (got zzzz)"),
        (("-s", "11:22:33:44:55:GG"), "mac address contains invalid character"),
        (("-t", "11:22:33:44:55:66/49"), "mac mask must be between 0 and 48"),
        (("-t", "11:22:33:44:55:66/abc"), "mac mask must be between 0 and 48"),
        # optional values as the next word: consumed, so "extra" is the stray
        (("-a", "8e89bed6", "extra"), "error: unexpected argument 'extra'"),
        (("-v", "1", "extra"), "error: unexpected argument 'extra'"),
        # "-" is still the stdout value of the capture options
        (("-q", "-", "extra"), "error: unexpected argument 'extra'"),
        (("-c", "-", "extra"), "error: unexpected argument 'extra'"),
    ),
    "ubertooth-debug": STRAY + (
        (("-v", "3"), "error: verbosity must be 0-2 (got 3)"),
        (("-v", "abc"), "error: verbosity must be 0-2 (got abc)"),
        # the message states the range -r accepts: 0x00 and 0xff are both in
        # what -r accepts and refuses is covered in full by test_debug_cli.py
        (("-r", "0x100"), "error: register address must be 0x00-0xff (got 0x100)"),
    ),
    "ubertooth-dfu": STRAY,
    "ubertooth-ducky": STRAY + ADV_INDEX + (
        (("-a", "11:22:33:44:55"),
         "error: bluetooth address must be xx:xx:xx:xx:xx:xx (got 11:22:33:44:55)"),
        (("-a", "11:22:33:44:55:GG"), "error: bluetooth address must be"),
        (("-q", "nonsense"), "error parsing uuid 'nonsense'"),
        (("-q", "fd123ff9-9e30-45b2-af0d-b85b7d2dc80c", "extra"),
         "error: unexpected argument 'extra'"),
    ),
    "ubertooth-dump": STRAY,
    "ubertooth-ego": STRAY + (
        (("-c", "2481"), "error: channel must be 2402-2480 mhz (got 2481)"),
        (("-c", "abc"), "error: channel must be 2402-2480 mhz (got abc)"),
        (("-a", "zzzzzzzz"), "error: access code must be 2, 4, 6 or 8 hex digits"),
        (("-a", "630f9ff"), "error: access code must be 2, 4, 6 or 8 hex digits"),
        (("-a", "630f9ffe9"), "error: access code must be 2, 4, 6 or 8 hex digits"),
        (("-l", "49"), "error: capture length must be 1-48 (got 49)"),
    ),
    "ubertooth-follow": STRAY + LAP_UAP + AC_ERRORS + (
        (("-w", "abc"), "error: usb delay must be a whole number of timeslots, 0 or more (got abc)"),
    ),
    "ubertooth-rx": STRAY + VERSION_STRAY + LAP_UAP + AC_ERRORS + TIMEOUT_SECONDS + (
        (("-c", "79"), "error: channel must be 0-78 (got 79)"),
        (("-c", "abc"), "error: channel must be 0-78 (got abc)"),
        # a UAP needs the LAP it belongs to, refused before a device is opened
        (("-u", "22"), "error: -u <uap> requires -l <lap>"),
        (("-u", "22", "-U", NO_SUCH_SERIAL), "error: -u <uap> requires -l <lap>"),
    ),
    "ubertooth-scan": STRAY + AC_ERRORS + TIMEOUT_SECONDS,
    "ubertooth-specan": STRAY + (
        (("-l", "bad"), "error: lower frequency must be 2402-2480 mhz (got bad)"),
        (("-u", "2481"), "error: upper frequency must be 2402-2480 mhz (got 2481)"),
    ),
    "ubertooth-tx": STRAY + VERSION_STRAY + LAP_UAP + TIMEOUT_SECONDS + (
        # both halves of the address are needed, before a device is opened
        ((), "error: -l <lap> and -u <uap> are both required"),
        (("-l", "446688"), "error: -l <lap> and -u <uap> are both required"),
        (("-u", "22"), "error: -l <lap> and -u <uap> are both required"),
        (("-u", "22", "-U", NO_SUCH_SERIAL),
         "error: -l <lap> and -u <uap> are both required"),
    ),
    "ubertooth-util": STRAY + (
        (("-c", "2500"), "error: frequency must be 2402-2480 mhz (got 2500)"),
        (("-c", "24x"), "error: unexpected argument '24x'"),
    ),
}

if TOOL not in CASES:
    sys.exit("no argument cases for %s" % TOOL)


def run(arguments):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        # "-q -" writes a binary PCAP header to stdout
        errors="replace",
        timeout=10,
        check=False,
        env=ENVIRONMENT,
    )


for arguments, expected in CASES[TOOL]:
    result = run(arguments)
    output = (result.stdout + result.stderr).lower()
    assert result.returncode == 1, (arguments, result)
    assert expected in output, (arguments, expected, result)
    for message in DEVICE_ACCESS:
        assert message not in output, (arguments, message, result)

# -h on its own still prints the tool's help and reaches no device. The exit
# status is the tool's own (0 for ubertooth-debug and ubertooth-specan, 1 for
# the rest); only the refusal is new.
help_result = run(("-h",))
help_output = (help_result.stdout + help_result.stderr).lower()
assert help_result.returncode in (0, 1), help_result
assert TOOL in help_output, help_result
assert "unexpected argument" not in help_output, help_result
# every tool prints the same -U line (OPT_USAGE_SELECT_DEVICE in opt_parse.h)
assert ("\t-u <index|serial> select ubertooth by index or serial; see ubertooth(7)\n"
        in help_output), help_result
for message in DEVICE_ACCESS:
    assert message not in help_output, (message, help_result)

print("%s: %d argument cases" % (TOOL, len(CASES[TOOL])))
