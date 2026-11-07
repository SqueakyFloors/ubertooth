#!/usr/bin/env python3
"""The two stream rules the command-line matrix cannot generate.

Run as: test_output_streams.py <path to one ubertooth-* binary>

Everything else this file used to check -- -h and -V on standard output, an
unknown option as one "error: " line, a bare command line, a command with
nothing to do, a removed flag, and that none of them reaches device lookup --
is generated for every tool from cli_spec.json by test_cli_matrix.py, as
universal rules 1 to 5. What is left here is the two things that generator
cannot reach:

  * a refusal whose whole line is pinned, with standard output empty. The
    matrix pins many refusals by a substring, and a line that moved to the
    other stream would still pass a check that reads the two together;
  * a capture sent to standard output while standard output is a terminal.
    That needs a pty, and every matrix run gives the tool a pipe, so no
    generated case can put a terminal there.

No device is opened in any case here; UBERTOOTH_TEST_NO_DEVICES=1 hides any
board that is attached, and a run that reached device selection is caught by
the message that reports it.
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
# How long one run of the tool is given. A handful of short runs per tool, so
# twenty seconds is many times what any single run needs.
RUN_TIMEOUT_S = 20

checked = []

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

if checked:
    print("%s: the rule holds for %s" % (TOOL, " and ".join(checked)))
else:
    print("%s: no named refusal and no capture on standard output" % TOOL)
