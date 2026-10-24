#!/usr/bin/env python3
"""A refused command line creates and truncates nothing.

Run as: test_no_file_on_refusal.py <path to one ubertooth-* binary>

The file options used to open their file inside the getopt loop, so a command
line the argument rule then refused had already written a PCAP header or
truncated an existing capture (#193). Every file option is now named during
parsing and opened only once the whole command line has been accepted, so each
case here checks two things after a refused run:

  * the named file was not created, and
  * a file that was already there still has its old contents.

Three ways to be refused, all of which used to come too late:
  * a stray word, caught by opt_check_no_operands();
  * a bad value for a later option;
  * -h, which ends the command only once the command line has parsed.

The tools run with UBERTOOTH_TEST_NO_DEVICES=1, so a case that got past
argument checking reports that it found no device rather than opening whatever
is attached. No case should reach that point.
"""

import os
import subprocess
import sys
import tempfile


BINARY = sys.argv[1]
TOOL = os.path.basename(BINARY)
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1")
KEEP = "do not touch this\n"

# Per tool: the file options, and the name each one writes. None as the written
# name means "the value given", which is all but ubertooth-dfu -s: that takes a
# .bin to sign and writes the .dfu beside it.
FILE_OPTIONS = {
    "ubertooth-btle": (("-r", "out.pcapng", None),
                       ("-q", "out.pcap", None),
                       ("-c", "out.pcap", None)),
    "ubertooth-dfu": (("-s", "firmware.bin", "firmware.dfu"),),
    "ubertooth-dump": (("-d", "out.bin", None),),
    "ubertooth-follow": (("-r", "out.pcapng", None),
                         ("-q", "out.pcap", None),
                         ("-d", "out.bin", None)),
    "ubertooth-rx": (("-r", "out.pcapng", None),
                     ("-q", "out.pcap", None),
                     ("-d", "out.bin", None)),
    "ubertooth-specan": (("-d", "out.bin", None),),
}

# A value every one of these tools refuses, so the file option is followed by a
# check that fails. None where the tool has no such option of its own.
BAD_VALUE = {
    "ubertooth-btle": ("-A", "40"),
    "ubertooth-dfu": None,
    "ubertooth-dump": None,
    "ubertooth-follow": ("-e", "6"),
    "ubertooth-rx": ("-e", "6"),
    "ubertooth-specan": ("-l", "bad"),
}

if TOOL not in FILE_OPTIONS:
    print("%s: no file options" % TOOL)
    sys.exit(0)


def refused(arguments):
    result = subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        errors="replace",
        timeout=10,
        check=False,
        env=ENVIRONMENT,
    )
    output = (result.stdout + result.stderr).lower()
    # -h ends the command with the tool's own status (0 for ubertooth-specan
    # and ubertooth-debug, 1 for the rest); the other cases are errors.
    expected = (0, 1) if "-h" in arguments else (1,)
    assert result.returncode in expected, (arguments, result)
    assert "no ubertooth" not in output, (arguments, result)
    assert "could not open ubertooth" not in output, (arguments, result)
    return result


cases = 0
for option, value, written in FILE_OPTIONS[TOOL]:
    written = written or value
    refusals = [(option, value, "extra"), (option, value, "-h")]
    if BAD_VALUE[TOOL] is not None:
        refusals.append((option, value, *BAD_VALUE[TOOL]))

    for arguments in refusals:
        # nothing there yet: the file must not appear
        with tempfile.TemporaryDirectory() as directory:
            os.chdir(directory)
            if TOOL == "ubertooth-dfu":
                with open(value, "wb") as image:
                    image.write(b"\x00" * 2048)
            before = sorted(os.listdir(directory))
            refused(arguments)
            assert sorted(os.listdir(directory)) == before, \
                (arguments, os.listdir(directory))

        # already there: the contents must survive
        with tempfile.TemporaryDirectory() as directory:
            os.chdir(directory)
            if TOOL == "ubertooth-dfu":
                with open(value, "wb") as image:
                    image.write(b"\x00" * 2048)
            with open(written, "w") as existing:
                existing.write(KEEP)
            refused(arguments)
            with open(written) as existing:
                assert existing.read() == KEEP, (arguments, written)
        cases += 1

os.chdir("/")
print("%s: %d refused command lines left the filesystem alone" % (TOOL, cases))
