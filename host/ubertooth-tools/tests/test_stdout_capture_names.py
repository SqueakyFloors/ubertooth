#!/usr/bin/env python3
"""Verify the capture destinations that name this process's standard output.

A capture destination of ``-`` hands descriptor 1 to the capture and keeps
everything else off that stream. ``/dev/stdout`` and ``/dev/fd/1`` reach the
same descriptor, with or without a redundant separator or ``.`` element, so
each has to behave the same way: the capture arrives on descriptor 1 on its
own, and a terminal there is refused.

A symbolic link whose chain ends at one of those names reaches the stream as
well, because a writer writes through a link, so it is treated the same way.

The names next to those stay ordinary file names, and that boundary is pinned
here too: ``/dev/fd/<n>`` for any other ``<n>`` reaches whatever the caller's
shell opened on that descriptor, a relative ``dev/stdout`` is a file in the
working directory, and a name ending in ``/.`` asks for something inside a
file that is not a directory.

Everything runs as a replay (``ubertooth-rx -i``) or as a refused command
line, so no board is involved. The replay fixture is built from scratch: the
access code of each record is computed from the Bluetooth Core specification
(Vol 2, Part B 6.3.3) in the bit order libbtbb decodes.
"""

import errno
import os
import pty
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RX, SPECAN, DUMP = sys.argv[1:4]
# ubertooth-follow is built only where BlueZ is, so its case is optional.
FOLLOW = sys.argv[4] if len(sys.argv) > 4 else None

SYMBOLS = 400                   # the 50 data bytes, most significant bit first
PREAMBLE_AT = 16                # where the access code starts in those symbols
BR_PACKET = 0
LAP = 0x5A3C96                  # the LAP every packet record carries
AGE_S = 3600                    # the records were written this long ago
CLK100NS_STEP = 1_000_000       # 0.1 s of device clock between records
RUN_TIMEOUT_S = 30

# The 64-bit scrambling sequence applied to the sync word, the (64,30)
# expurgated BCH generator (octal 260534236651), and the Barker sequence
# appended to the LAP, chosen by the LAP's top bit.
PN = 0x83848D96BBCC54FC
BCH_GENERATOR = 0o260534236651
BARKER = {0: 0b101100, 1: 0b010011}

# PCAP magic in either byte order, nanosecond resolution, as libbtbb writes it.
PCAP_MAGIC = (bytes.fromhex("4d3cb2a1"), bytes.fromhex("a1b23c4d"))


def sync_word(lap):
    """The 64-bit sync word of a LAP; bit i is the i-th symbol on air."""
    lap &= 0xFFFFFF
    info = lap | (BARKER[lap >> 23] << 24)
    scrambled = info ^ (PN >> 34)
    remainder = scrambled << 34
    for bit in range(63, 33, -1):
        if remainder >> bit & 1:
            remainder ^= BCH_GENERATOR << (bit - 34)
    return ((scrambled << 34) | remainder) ^ PN


def record(host_time, clk100ns, channel):
    """One replay record: a big-endian host time, then a 64-byte usb_pkt_rx."""
    bits = [(sync_word(LAP) >> i) & 1 for i in range(64)]
    symbols = [0] * PREAMBLE_AT
    symbols += [1, 0, 1, 0] if bits[0] else [0, 1, 0, 1]
    symbols += bits
    symbols += [0] * (SYMBOLS - len(symbols))
    data = bytes(
        sum(bit << (7 - j) for j, bit in enumerate(symbols[i:i + 8]))
        for i in range(0, SYMBOLS, 8)
    )
    header = struct.pack("<BBBBIbbbB2x", BR_PACKET, 0, channel, 0, clk100ns,
                         -40, -90, -70, 1)
    return struct.pack(">I", host_time) + header + data


BASE_TIME = int(time.time()) - AGE_S
RECORDS = [
    record(BASE_TIME + i, CLK100NS_STEP * (i + 1), channel)
    for i, channel in enumerate((3, 17, 40))
]
FIXTURE = b"".join(RECORDS)
EXPECTED_DUMP = b"".join(RECORDS)

# A replay opens no device; the switch keeps it that way if a run below ever
# reaches device selection. LC_ALL, because a refusal may quote strerror().
ENVIRONMENT = dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1", LC_ALL="C")

ON_LINUX = sys.platform.startswith("linux")

# Every spelling that names this process's descriptor 1. /proc/self/fd/1 is
# one on Linux and names nothing elsewhere, so it is checked on each platform
# for the answer that platform must give.
STDOUT_NAMES = [
    "-",
    "/dev/stdout",
    "/dev/./stdout",
    "/dev/fd/1",
    "/dev/fd/./1",
    "//dev//fd//1",
]
if ON_LINUX:
    STDOUT_NAMES += ["/proc/self/fd/1", "/proc/./self/fd/1"]

# A name ending in "/." asks for something inside the name before it, and
# neither of these is a directory, so it cannot be created on any platform and
# must not be read as standard output. The run is refused with the name
# quoted, which is what a file destination the writer could not open does.
NOT_STDOUT_NAMES = [
    "/dev/stdout/.",
    "/dev/fd/1/.",
]
if sys.platform == "darwin":
    # No /proc here, so this is a destination that cannot be created rather
    # than a name for descriptor 1.
    NOT_STDOUT_NAMES.append("/proc/self/fd/1")

failures = []


def check(condition, what, detail=""):
    if not condition:
        failures.append("FAIL %s%s" % (what, detail and ": " + detail))


def run_to_file(arguments, directory, target, extra_fds=()):
    """Run the tool with standard output on <target>; returns (status, stderr)."""
    with open(target, "wb") as stream:
        result = subprocess.run(
            [RX, *arguments], cwd=directory, env=ENVIRONMENT,
            stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.PIPE,
            timeout=RUN_TIMEOUT_S, check=False,
            pass_fds=tuple(extra_fds),
        )
    return result.returncode, result.stderr.decode()


def run_to_tty(binary, arguments, directory):
    """Run the tool with a terminal on standard output.

    Returns (status, stderr, bytes written to the terminal), or None where no
    pseudo-terminal can be allocated.
    """
    try:
        reader, writer = pty.openpty()
    except OSError:                                 # a sandbox with no pty
        return None
    try:
        result = subprocess.run(
            [binary, *arguments], cwd=directory, env=ENVIRONMENT,
            stdin=subprocess.DEVNULL, stdout=writer, stderr=subprocess.PIPE,
            timeout=RUN_TIMEOUT_S, check=False,
        )
        os.close(writer)
        writer = -1
        os.set_blocking(reader, False)
        try:
            written = os.read(reader, 4096)
        except OSError as problem:
            # Nothing to read: EAGAIN where the terminal is still open at the
            # other end, EIO on Linux once it is not. Any other reason is a
            # read that failed, and reporting it as "nothing was written"
            # would let the checks below pass without looking.
            if problem.errno not in (errno.EAGAIN, errno.EIO):
                raise
            written = b""
    finally:
        if writer >= 0:
            os.close(writer)
        os.close(reader)
    return result.returncode, result.stderr.decode(), written


with tempfile.TemporaryDirectory() as workdir:
    work = Path(workdir)
    (work / "full.bin").write_bytes(FIXTURE)

    # The baseline: a replay writing its dump to an ordinary file prints the
    # per-packet report on standard output, where it has always been.
    status, errors = run_to_file(["-i", "full.bin", "-d", "plain.bin"],
                                 work, work / "plain.out")
    report = (work / "plain.out").read_bytes()
    check(status == 0, "a replay to an ordinary file exits 0", str(status))
    check((work / "plain.bin").read_bytes() == EXPECTED_DUMP,
          "the dump of a replay to an ordinary file holds the records")
    check(b"systime=" in report,
          "the per-packet report is on standard output for a file destination")
    check("error" not in errors, "a replay to a file reports no error", errors)

    # Every spelling of standard output: the dump arrives on descriptor 1 byte
    # for byte, with none of the report text that the run above printed there.
    for name in STDOUT_NAMES:
        target = work / ("out-%d.bin" % STDOUT_NAMES.index(name))
        status, errors = run_to_file(["-i", "full.bin", "-d", name],
                                     work, target)
        captured = target.read_bytes()
        check(status == 0, "-d %s exits 0" % name, errors)
        check(captured == EXPECTED_DUMP,
              "-d %s writes the dump and nothing else to descriptor 1" % name,
              "%d bytes, expected %d" % (len(captured), len(EXPECTED_DUMP)))
        check(b"systime=" not in captured,
              "-d %s keeps the per-packet report out of the capture" % name)
        check("error" not in errors, "-d %s reports no error" % name, errors)

    # A PCAP capture on standard output, with a survey asked for: the capture
    # owns descriptor 1 and the survey results, which are what the user asked
    # the run for, come out on standard error instead.
    for name in STDOUT_NAMES:
        target = work / ("survey-%d.pcap" % STDOUT_NAMES.index(name))
        status, errors = run_to_file(["-i", "full.bin", "-q", name, "-z"],
                                     work, target)
        captured = target.read_bytes()
        check(status == 0, "-q %s -z exits 0" % name, errors)
        check(captured[:4] in PCAP_MAGIC,
              "-q %s -z writes a PCAP to descriptor 1" % name,
              captured[:4].hex())
        check(b"Survey Results" not in captured,
              "-q %s -z keeps the survey heading out of the capture" % name)
        check("Survey Results" in errors,
              "-q %s -z prints the survey heading on standard error" % name,
              errors)
        check("5A:3C:96" in errors.split("Survey Results")[-1],
              "-q %s -z prints the address it found on standard error" % name,
              errors)

    # A raw dump on standard output, with a survey asked for: the addresses
    # are what the user asked the run for, so they arrive on standard error
    # for every spelling rather than going to the null device the capture left
    # behind.
    for name in STDOUT_NAMES:
        target = work / ("dumpsurvey-%d.bin" % STDOUT_NAMES.index(name))
        status, errors = run_to_file(["-i", "full.bin", "-d", name, "-z"],
                                     work, target)
        captured = target.read_bytes()
        check(status == 0, "-d %s -z exits 0" % name, errors)
        check(captured == EXPECTED_DUMP,
              "-d %s -z writes the dump and nothing else to descriptor 1"
              % name, "%d bytes, expected %d" % (len(captured),
                                                 len(EXPECTED_DUMP)))
        check("Survey Results" in errors,
              "-d %s -z prints the survey heading on standard error" % name,
              errors)
        check("5A:3C:96" in errors.split("Survey Results")[-1],
              "-d %s -z prints the address it found on standard error" % name,
              errors)

    # A spelling of descriptor 1 that only the platform's own descriptor
    # filesystem answers: the BSDs read a leading zero as part of the number,
    # so "/dev/fd/01" is that stream there, while on Linux /dev/fd holds exact
    # names and the path leads nowhere. Whichever this platform does, the tool
    # has to follow it -- reading the number alone would leave a capture given
    # it with the report mixed in, and unrefused at a terminal.
    padded = "/dev/fd/01"
    try:
        reaches_stdout = os.stat(padded).st_ino == os.fstat(1).st_ino
    except OSError:
        reaches_stdout = False
    if not reaches_stdout:
        print("skipping %s, this platform does not answer it" % padded)
    else:
        status, errors = run_to_file(["-i", "full.bin", "-d", padded, "-z"],
                                     work, work / "padded.bin")
        captured = (work / "padded.bin").read_bytes()
        check(status == 0, "-d %s exits 0" % padded, errors)
        check(captured == EXPECTED_DUMP,
              "-d %s writes the dump and nothing else to descriptor 1"
              % padded, "%d bytes, expected %d" % (len(captured),
                                                   len(EXPECTED_DUMP)))
        check("5A:3C:96" in errors,
              "-d %s prints the address it found on standard error" % padded,
              errors)
        outcome = run_to_tty(RX, ["-i", "full.bin", "-d", padded], work)
        if outcome is None:
            print("skipping the %s terminal case, no pty available" % padded)
        else:
            status, errors, written = outcome
            check(status == 1, "-d %s at a terminal exits 1" % padded,
                  str(status))
            check("error: stdout is a tty, refusing to write binary dump data"
                  " to it" in errors,
                  "-d %s at a terminal says why" % padded, errors)
            check(written == b"",
                  "-d %s writes nothing to the terminal" % padded,
                  repr(written[:60]))

    # A symbolic link pointing at one of those names reaches the same stream,
    # because a writer writes through a link. So the capture owns descriptor 1
    # the same way: the dump arrives there whole, and the survey results, which
    # are what the user asked the run for, come out on standard error.
    paths = [name for name in STDOUT_NAMES if name != "-"]
    for name in paths:
        link = "link-%d" % paths.index(name)
        (work / link).symlink_to(name)
        target = work / ("linked-%d.bin" % paths.index(name))
        status, errors = run_to_file(["-i", "full.bin", "-d", link, "-z"],
                                     work, target)
        captured = target.read_bytes()
        check(status == 0, "-d <link to %s> exits 0" % name, errors)
        check(captured == EXPECTED_DUMP,
              "-d <link to %s> writes the dump and nothing else to"
              " descriptor 1" % name,
              "%d bytes, expected %d" % (len(captured), len(EXPECTED_DUMP)))
        check("5A:3C:96" in errors,
              "-d <link to %s> prints the address it found on standard error"
              % name, errors)

    # A chain of links reaches it too, since the writer's own open follows the
    # whole chain.
    (work / "chain-1").symlink_to("/dev/stdout")
    (work / "chain-2").symlink_to("chain-1")
    status, errors = run_to_file(["-i", "full.bin", "-d", "chain-2", "-z"],
                                 work, work / "chained.bin")
    check(status == 0, "-d <chain of links to the stream> exits 0", errors)
    check((work / "chained.bin").read_bytes() == EXPECTED_DUMP,
          "-d <chain of links> writes only the dump to descriptor 1")
    check("5A:3C:96" in errors,
          "-d <chain of links> prints the address on standard error", errors)

    # A link to the stream beside a "-" is the one pair the stream cannot
    # carry, so it gets the one-capture-on-standard-output refusal rather than
    # a line about two captures of one file.
    status, errors = run_to_file(
        ["-i", "full.bin", "-q", "link-0", "-d", "-"], work,
        work / "linkpair.out")
    check(status == 1, "-q <link to the stream> -d - exits 1", str(status))
    check(errors == "error: -q and -d cannot both write to standard output\n",
          "-q <link to the stream> -d - names the pair that clashed", errors)
    check((work / "linkpair.out").read_bytes() == b"",
          "-q <link to the stream> -d - writes nothing to descriptor 1")

    # -r through such a link is refused with the line that says to use -q,
    # rather than the writer reporting the stream's own device node as a file
    # that already exists.
    status, errors = run_to_file(["-i", "full.bin", "-r", "link-0"], work,
                                 work / "linkpcapng.out")
    check(status == 1, "-r <link to the stream> exits 1", str(status))
    check(errors == "error: PCAPNG capture to stdout is not supported;"
                    " use -q -\n",
          "-r <link to the stream> says what to use instead", errors)

    # A link pointing at an ordinary file is an ordinary destination: the
    # capture goes through it and the report stays on standard output.
    (work / "to-file").symlink_to("through-link.bin")
    status, errors = run_to_file(["-i", "full.bin", "-d", "to-file"], work,
                                 work / "tofile.out")
    check(status == 0, "-d <link to a file> exits 0", errors)
    check((work / "through-link.bin").read_bytes() == EXPECTED_DUMP,
          "-d <link to a file> writes the dump through the link")
    check(b"systime=" in (work / "tofile.out").read_bytes(),
          "-d <link to a file> leaves the report on standard output")

    # /dev/fd/<n> for any other <n> is an ordinary file name. The capture goes
    # to whatever that descriptor is, and standard output is left alone, so
    # the report is still printed there.
    with open(work / "byfd.bin", "wb") as inherited:
        number = inherited.fileno()
        check(number > 2, "the inherited descriptor is not one of the three",
              str(number))
        status, errors = run_to_file(
            ["-i", "full.bin", "-d", "/dev/fd/%d" % number],
            work, work / "byfd.out", extra_fds=(number,))
    captured = (work / "byfd.bin").read_bytes()
    report = (work / "byfd.out").read_bytes()
    check(status == 0, "-d /dev/fd/<n> exits 0", errors)
    check(captured == EXPECTED_DUMP,
          "-d /dev/fd/<n> writes the dump to that descriptor",
          "%d bytes" % len(captured))
    check(b"systime=" in report,
          "-d /dev/fd/<n> leaves the report on standard output")

    # "./-" is how a file named "-" is written, so it stays an ordinary file.
    status, errors = run_to_file(["-i", "full.bin", "-d", "./-"],
                                 work, work / "dash.out")
    check(status == 0, "-d ./- exits 0", errors)
    check((work / "-").read_bytes() == EXPECTED_DUMP,
          "-d ./- writes the file named -")
    check(b"systime=" in (work / "dash.out").read_bytes(),
          "-d ./- leaves the report on standard output")

    # A relative "dev/stdout" is a file in the working directory.
    (work / "dev").mkdir()
    status, errors = run_to_file(["-i", "full.bin", "-d", "dev/stdout"],
                                 work, work / "relative.out")
    check(status == 0, "-d dev/stdout exits 0", errors)
    check((work / "dev" / "stdout").read_bytes() == EXPECTED_DUMP,
          "-d dev/stdout writes the file in the working directory")
    check(b"systime=" in (work / "relative.out").read_bytes(),
          "-d dev/stdout leaves the report on standard output")

    # A name one "/." away from standard output is not standard output.
    for name in NOT_STDOUT_NAMES:
        target = work / ("not-%d.out" % NOT_STDOUT_NAMES.index(name))
        status, errors = run_to_file(["-i", "full.bin", "-d", name],
                                     work, target)
        check(status == 1, "-d %s exits 1" % name, str(status))
        check(errors.startswith("error: %s: " % name),
              "-d %s is refused with the name quoted" % name, errors)
        check(target.read_bytes() == b"",
              "-d %s writes nothing to standard output" % name)

    # Two captures cannot share the stream, and the pair is the same pair
    # however each of the two was spelled: it is refused while the command
    # line is read, so neither capture is created and nothing reaches
    # descriptor 1.
    for first, second in (("-", "-"), ("/dev/stdout", "-"),
                          ("-", "/dev/fd/1"),
                          ("/dev/fd/1", "/dev/./stdout")):
        target = work / "pair.out"
        status, errors = run_to_file(
            ["-i", "full.bin", "-q", first, "-d", second], work, target)
        check(status == 1, "-q %s -d %s exits 1" % (first, second), str(status))
        check(errors == "error: -q and -d cannot both write to standard"
                        " output\n",
              "-q %s -d %s is refused with the pair named" % (first, second),
              errors)
        written = target.read_bytes()
        check(written == b"",
              "-q %s -d %s writes nothing to descriptor 1" % (first, second),
              repr(written[:40]))

    # A PcapNG writer seeks back over what it has written, which the stream
    # does not allow, so -r refuses it by whichever name it was given rather
    # than leaving the writer to report the stream's own device node as a file
    # that is already there.
    for name in STDOUT_NAMES:
        target = work / "pcapng.out"
        status, errors = run_to_file(["-i", "full.bin", "-r", name], work,
                                     target)
        check(status == 1, "-r %s exits 1" % name, str(status))
        check(errors == "error: PCAPNG capture to stdout is not supported;"
                        " use -q -\n",
              "-r %s says what to use instead" % name, errors)
        check(target.read_bytes() == b"",
              "-r %s writes nothing to descriptor 1" % name)

    # ubertooth-follow asks the same helper about its own pair, so a pair
    # written two ways is refused there too, before any device is opened.
    if FOLLOW is None:
        print("skipping the ubertooth-follow case, the tool is not built here")
    else:
        result = subprocess.run(
            [FOLLOW, "-l", "5a3c96", "-u", "0f", "-q", "/dev/stdout",
             "-d", "-"],
            cwd=work, env=ENVIRONMENT, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=RUN_TIMEOUT_S, check=False)
        check(result.returncode == 1, "follow refuses -q /dev/stdout -d -",
              str(result.returncode))
        check(b"error: -q and -d cannot both write to standard output"
              in result.stderr,
              "follow names the pair that clashed", result.stderr.decode())
        check(result.stdout == b"",
              "follow writes nothing to standard output when it refuses")

    # A terminal is refused for every spelling, before anything is written to
    # it, and the refusal names what would have gone there.
    for name in STDOUT_NAMES:
        for option, what in (("-d", "binary dump data"),
                             ("-q", "binary capture data")):
            outcome = run_to_tty(RX, ["-i", "full.bin", option, name], work)
            if outcome is None:
                print("skipping the terminal cases, no pty available")
                break
            status, errors, written = outcome
            check(status == 1, "%s %s at a terminal exits 1" % (option, name),
                  str(status))
            check("error: stdout is a tty, refusing to write %s to it" % what
                  in errors,
                  "%s %s at a terminal says why" % (option, name), errors)
            check(written == b"",
                  "%s %s writes nothing to the terminal" % (option, name),
                  repr(written[:60]))
        else:
            continue
        break

    # A terminal behind such a link is refused too, which is what keeps a
    # binary capture off a user's screen when the stream was named indirectly.
    outcome = run_to_tty(RX, ["-i", "full.bin", "-d", "link-0"], work)
    if outcome is None:
        print("skipping the link-at-a-terminal case, no pty available")
    else:
        status, errors, written = outcome
        check(status == 1, "-d <link to the stream> at a terminal exits 1",
              str(status))
        check("error: stdout is a tty, refusing to write binary dump data"
              " to it" in errors,
              "-d <link to the stream> at a terminal says why", errors)
        check(written == b"",
              "-d <link to the stream> writes nothing to the terminal",
              repr(written[:60]))

    # The same refusal reaches the other tools that share these helpers, with
    # no board and no replay: it is a command-line check.
    for binary, arguments, what in (
            (SPECAN, ["-d", "/dev/stdout"], "binary samples"),
            (DUMP, ["-d", "/dev/fd/1"], "binary dump data")):
        outcome = run_to_tty(binary, arguments, work)
        if outcome is None:
            print("skipping the other tools' terminal cases, no pty available")
            break
        status, errors, written = outcome
        tool = os.path.basename(binary)
        check(status == 1, "%s at a terminal exits 1" % tool, str(status))
        check("error: stdout is a tty, refusing to write %s to it" % what
              in errors, "%s at a terminal says why" % tool, errors)
        check(written == b"", "%s writes nothing to the terminal" % tool,
              repr(written[:60]))

for line in failures:
    print(line)
if failures:
    sys.exit("%d checks failed" % len(failures))
print("stdout capture destination names: all checks passed")
