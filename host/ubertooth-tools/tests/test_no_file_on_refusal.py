#!/usr/bin/env python3
"""A refused command line creates and truncates nothing.

Run as: test_no_file_on_refusal.py <path to one ubertooth-* binary>

The file options used to open their file inside the getopt loop, so a command
line the argument rule then refused had already written a PCAP header or
truncated an existing capture. Every file option is now named during parsing
and opened only once the whole command line has been accepted, so each case
here checks two things after a refused run:

  * the named file was not created, and
  * a file that was already there still has its old contents.

Three ways to be refused, all of which used to come too late:
  * a stray word, caught by opt_check_no_operands();
  * a bad value for a later option;
  * -h, which ends the command only once the command line has parsed.

For ubertooth-btle there is a fourth: a capture file with no mode to capture
in, which is a command line with no work in it.

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
# .bin to sign and writes the .dfu beside it. ubertooth-dfu -u writes the name
# it is given, through a temporary file beside it that must not be left behind
# either.
FILE_OPTIONS = {
    "ubertooth-btle": (("-r", "out.pcapng", None),
                       ("-q", "out.pcap", None),
                       ("-c", "out.pcap", None)),
    "ubertooth-dfu": (("-s", "firmware.bin", "firmware.dfu"),
                      ("-u", "out.bin", None)),
    "ubertooth-dump": (("-d", "out.bin", None),),
    "ubertooth-follow": (("-r", "out.pcapng", None),
                         ("-q", "out.pcap", None),
                         ("-d", "out.bin", None)),
    "ubertooth-rx": (("-r", "out.pcapng", None),
                     ("-q", "out.pcap", None),
                     ("-d", "out.bin", None)),
    "ubertooth-specan": (("-d", "out.bin", None),),
}

# Further arguments every one of these tools refuses, so the file option is
# followed by a check that fails. Empty where the tool has none of its own.
BAD_VALUE = {
    "ubertooth-btle": (("-A", "40"),),
    "ubertooth-dfu": (),
    "ubertooth-dump": (),
    # -e 6 is out of range; -a without -l and -u cannot be carried out. Both
    # are refused while the command line is read, so neither reaches the file.
    "ubertooth-follow": (("-e", "6"), ("-a",)),
    "ubertooth-rx": (("-e", "6"),),
    "ubertooth-specan": (("-l", "bad"),),
}

if TOOL not in FILE_OPTIONS:
    print("%s: no file options" % TOOL)
    sys.exit(0)


def refused(arguments, directory):
    result = subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        errors="replace",
        timeout=10,
        check=False,
        env=ENVIRONMENT,
        cwd=directory,
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
    for bad in BAD_VALUE[TOOL]:
        refusals.append((option, value, *bad))
    if TOOL == "ubertooth-btle":
        # a capture file with no mode to capture in is a fourth refusal, and
        # the file it names must not be created either
        refusals.append((option, value))

    for arguments in refusals:
        # nothing there yet: the file must not appear, and neither must any
        # temporary file beside it
        with tempfile.TemporaryDirectory() as directory:
            if TOOL == "ubertooth-dfu" and option == "-s":
                with open(os.path.join(directory, value), "wb") as image:
                    image.write(b"\x00" * 2048)
            before = sorted(os.listdir(directory))
            refused(arguments, directory)
            assert sorted(os.listdir(directory)) == before, \
                (arguments, os.listdir(directory))

        # already there: the contents must survive
        with tempfile.TemporaryDirectory() as directory:
            if TOOL == "ubertooth-dfu" and option == "-s":
                with open(os.path.join(directory, value), "wb") as image:
                    image.write(b"\x00" * 2048)
            target = os.path.join(directory, written)
            with open(target, "w") as existing:
                existing.write(KEEP)
            before = sorted(os.listdir(directory))
            refused(arguments, directory)
            with open(target) as existing:
                assert existing.read() == KEEP, (arguments, written)
            assert sorted(os.listdir(directory)) == before, \
                (arguments, os.listdir(directory))
        cases += 1

print("%s: %d refused command lines left the filesystem alone" % (TOOL, cases))
