#!/usr/bin/env python3
"""Verify ubertooth-rx -i replays the recorded times and reports bad files.

A replay reads the record format ``ubertooth-rx -d`` and ``ubertooth-dump``
write: a big-endian 32-bit host time, then one 64-byte usb_pkt_rx. The
fixtures here are built from scratch, so no board and no saved capture is
needed: the access code of each record is computed from the Bluetooth Core
specification (Vol 2, Part B 6.3.3) in the bit order libbtbb decodes.
"""

import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BINARY = sys.argv[1]

RECORD_LEN = 68                 # host time + usb_pkt_rx
SYMBOLS = 400                   # the 50 data bytes, unpacked most significant bit first
PREAMBLE_AT = 16                # where the access code starts in those symbols
BR_PACKET = 0
LAP = 0x5A3C96                  # the LAP every packet record carries
AGE_S = 3600                    # the records were written this long ago
CLK100NS_STEP = 1_000_000       # 0.1 s of device clock between records
# How long one ubertooth-rx run is given. The whole test takes about 0.4 s on
# an idle machine, spread over a handful of replays, so half a minute is far
# more than any of them needs. It is kept that wide on purpose: on a machine
# busy with parallel builds the cost that grows is starting a process, not the
# replay itself, and the CTest timeout on this test is larger again, so this
# deadline is the one that fires and says the run timed out.
RUN_TIMEOUT_S = 30

# The 64-bit scrambling sequence applied to the sync word, the (64,30)
# expurgated BCH generator (octal 260534236651), and the Barker sequence
# appended to the LAP, chosen by the LAP's top bit.
PN = 0x83848D96BBCC54FC
BCH_GENERATOR = 0o260534236651
BARKER = {0: 0b101100, 1: 0b010011}


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


def access_code_symbols(lap):
    """The preamble and sync word, as symbols."""
    bits = [(sync_word(lap) >> i) & 1 for i in range(64)]
    preamble = [1, 0, 1, 0] if bits[0] else [0, 1, 0, 1]
    return preamble + bits


def record(host_time, clk100ns, channel, with_packet=True):
    symbols = [0] * PREAMBLE_AT
    if with_packet:
        symbols += access_code_symbols(LAP)
    symbols += [0] * (SYMBOLS - len(symbols))
    data = bytes(
        sum(bit << (7 - j) for j, bit in enumerate(symbols[i:i + 8]))
        for i in range(0, SYMBOLS, 8)
    )
    header = struct.pack("<BBBBIbbbB2x", BR_PACKET, 0, channel, 0, clk100ns,
                         -40, -90, -70, 1)
    return struct.pack(">I", host_time) + header + data


# Five records, one second apart, the fourth without an access code so that a
# record the replay skips is covered too.
BASE_TIME = int(time.time()) - AGE_S
CHANNELS = (3, 17, 40, 30, 62)
HAS_PACKET = (True, True, True, False, True)
CLOCKS = tuple(CLK100NS_STEP * (i + 1) for i in range(len(CHANNELS)))
RECORDS = [
    record(BASE_TIME + i, clock, channel, has_packet)
    for i, (clock, channel, has_packet) in enumerate(zip(CLOCKS, CHANNELS, HAS_PACKET))
]
FIXTURE = b"".join(RECORDS)
# The host times of the records that decode into a packet line, in order.
PACKET_TIMES = [BASE_TIME + i for i, has in enumerate(HAS_PACKET) if has]
PACKET_RECORDS = [r for r, has in zip(RECORDS, HAS_PACKET) if has]
# A capture written while replaying starts at the host time of the first
# record and advances with the Ubertooth clock (100 ns per tick) from there.
EXPECTED_NS = [
    BASE_TIME * 1_000_000_000 + 100 * (clock - CLOCKS[0])
    for clock, has in zip(CLOCKS, HAS_PACKET) if has
]

# The same, with the first record at clock 0: a valid start, not a missing one.
ZERO_CLOCKS = tuple(CLK100NS_STEP * i for i in range(3))
ZERO_FIXTURE = b"".join(
    record(BASE_TIME + i, clock, CHANNELS[i])
    for i, clock in enumerate(ZERO_CLOCKS)
)
ZERO_EXPECTED_NS = [
    BASE_TIME * 1_000_000_000 + 100 * clock for clock in ZERO_CLOCKS
]


def run(*arguments, directory, text=True, stdout=subprocess.PIPE,
        close_stdout=False):
    """One ubertooth-rx run. `stdout` takes an open file for the cases that
    need this process's standard output pointed somewhere in particular, and
    `close_stdout` runs it with descriptor 1 closed, as `1>&-` does."""
    return subprocess.run(
        [BINARY, *arguments],
        stdout=stdout,
        stderr=subprocess.PIPE,
        text=text,
        timeout=RUN_TIMEOUT_S,
        check=False,
        cwd=directory,
        preexec_fn=(lambda: os.close(1)) if close_stdout else None,
        # A replay opens no device; the switch keeps it that way if one of
        # these runs ever reaches device selection. LC_ALL, because a case
        # below compares a strerror() string and glibc translates those.
        env=dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1", LC_ALL="C"),
    )


def systimes(output):
    """The systime of every packet line, in order."""
    return [
        int(line.split()[0].split("=")[1])
        for line in output.splitlines() if line.startswith("systime=")
    ]


def pcap_times(path):
    """(seconds, nanoseconds) of each record in a libbtbb nanosecond pcap."""
    data = path.read_bytes()
    assert len(data) > 24, f"pcap is only {len(data)} bytes"
    order = "<" if data[:4] == bytes.fromhex("4d3cb2a1") else ">"
    out = []
    offset = 24
    while offset + 16 <= len(data):
        seconds, nanoseconds, length, _ = struct.unpack_from(order + "4I", data, offset)
        out.append((seconds, nanoseconds))
        offset += 16 + length
    return out


def pcapng_times(path):
    """Nanoseconds of each enhanced packet block in a pcapng."""
    data = path.read_bytes()
    order = "<" if data[8:12] == bytes.fromhex("4d3c2b1a") else ">"
    out = []
    offset = 0
    while offset + 12 <= len(data):
        block_type, length = struct.unpack_from(order + "2I", data, offset)
        if length < 12 or offset + length > len(data):
            break
        if block_type == 6:
            high, low = struct.unpack_from(order + "2I", data, offset + 12)
            out.append((high << 32) | low)
        offset += length
    return out


with tempfile.TemporaryDirectory() as workdir:
    work = Path(workdir)

    # A whole fixture: every packet line carries the host time of its record,
    # not the time of the replay, and the replay ends cleanly.
    (work / "full.bin").write_bytes(FIXTURE)
    result = run("-i", "full.bin", "-d", "dump.bin", directory=work)
    assert result.returncode == 0, result
    assert systimes(result.stdout) == PACKET_TIMES, result
    assert "error" not in result.stderr, result
    # -d during a replay rewrites the records it decoded, times and all.
    assert (work / "dump.bin").read_bytes() == b"".join(PACKET_RECORDS), result

    # -s asks for the channel sweep that is already the default, so the same
    # command with it decodes the same records the same way, says nothing
    # extra, and is listed in the usage text as the default it states.
    plain = run("-i", "full.bin", directory=work)
    swept = run("-i", "full.bin", "-s", directory=work)
    assert swept.returncode == plain.returncode == 0, (plain, swept)
    assert swept.stdout == plain.stdout, (plain, swept)
    assert swept.stderr == plain.stderr == "", (plain, swept)
    listed = run("-h", directory=work)
    assert "-s sweep all channels" in listed.stdout, listed

    # The signal figure beside a packet, at the two readings where the dBm
    # scale ends. The board reports power on a raw scale the tool turns into
    # dBm, and that conversion stops rising at raw 35: one below it still has
    # a figure of its own, and at it and above the tool can only say 0 dBm.
    # The boundary is where the overload warning is taken from as well, so a
    # change to either has to be deliberate.
    for raw, dbm in ((34, -5), (35, 0), (36, 0)):
        header = struct.pack("<BBBBIbbbB2x", BR_PACKET, 0x80, CHANNELS[0], 0,
                             CLOCKS[0], raw, raw - 6, raw, 50)
        symbols = [0] * PREAMBLE_AT + access_code_symbols(LAP)
        symbols += [0] * (SYMBOLS - len(symbols))
        data = bytes(
            sum(bit << (7 - j) for j, bit in enumerate(symbols[i:i + 8]))
            for i in range(0, SYMBOLS, 8)
        )
        (work / "level.bin").write_bytes(
            struct.pack(">I", BASE_TIME) + header + data)
        result = run("-i", "level.bin", directory=work)
        assert result.returncode == 0, result
        assert f" s={dbm} " in result.stdout, (raw, dbm, result)

    # A capture written during the replay is stamped from the first record's
    # host time, not the wall clock: the first packet, which is that record,
    # lands on it exactly, and the rest follow the device clock from there.
    result = run("-i", "full.bin", "-q", "cap.pcap", "-r", "cap.pcapng",
                 directory=work)
    assert result.returncode == 0, result
    expected = [(ns // 1_000_000_000, ns % 1_000_000_000) for ns in EXPECTED_NS]
    assert pcap_times(work / "cap.pcap") == expected, result
    assert pcapng_times(work / "cap.pcapng") == EXPECTED_NS, result

    # The same for a capture whose first record sits at clock 0.
    (work / "zero.bin").write_bytes(ZERO_FIXTURE)
    result = run("-i", "zero.bin", "-q", "zero.pcap", directory=work)
    assert result.returncode == 0, result
    expected = [(ns // 1_000_000_000, ns % 1_000_000_000) for ns in ZERO_EXPECTED_NS]
    assert pcap_times(work / "zero.pcap") == expected, result

    # A capture file that cannot be created: one line on standard error, which
    # names the file the user asked for and the reason the create failed,
    # nothing decoded, and nothing on standard output.
    #
    # The reason is the errno of the writer's own failed open. A writer that
    # printed a line of its own, which is what a libbtbb older than the commit
    # FindBTBB.cmake pins does, would make the stream two lines: a failure
    # here with "PCAP error:" in it is an installed libbtbb behind the pin
    # rather than a change to this tool.
    for option, name in (("-q", "cap.pcap"), ("-r", "cap.pcapng")):
        inside_missing_dir = Path("nodir") / name
        result = run("-i", "full.bin", option, str(inside_missing_dir),
                     directory=work)
        assert result.returncode == 1, result
        assert result.stdout == "", result
        assert result.stderr == (
            f"error: {inside_missing_dir}: No such file or directory\n"), result

    # A file that cannot be read: reported, and exit 1 rather than looking
    # like an empty capture. fopen() succeeds on a directory, the first read
    # then fails; where it does not, the open error is reported instead.
    (work / "adir").mkdir()
    result = run("-i", "adir", directory=work)
    assert result.returncode == 1, result
    assert ("error: reading adir failed:" in result.stderr
            or "error: adir: " in result.stderr), result
    assert systimes(result.stdout) == [], result

    # A last record cut short: the whole records before it are decoded, the
    # leftover bytes are named, and the command ends with an error.
    (work / "cut.bin").write_bytes(FIXTURE[:-10])
    result = run("-i", "cut.bin", directory=work)
    assert result.returncode == 1, result
    assert systimes(result.stdout) == PACKET_TIMES[:-1], result
    assert (f"error: cut.bin ends in a partial record: {RECORD_LEN - 10} of "
            f"{RECORD_LEN} bytes, ignored" in result.stderr), result

    # An empty file is a clean end of file, not an error.
    (work / "empty.bin").write_bytes(b"")
    result = run("-i", "empty.bin", directory=work)
    assert result.returncode == 0, result
    assert result.stdout == "" and result.stderr == "", result

    # A missing file is reported on standard error, with standard output
    # left empty for whatever is reading the replay.
    result = run("-i", "missing.bin", directory=work)
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr.startswith("error: missing.bin: "), result

    # No output file may be the dump being replayed: each is opened for
    # writing, which truncates it, so the dump would be emptied before a
    # record of it was read. -d used to leave it 0 bytes long and exit 0 with
    # nothing said, and -q used to report a partial record of the file it had
    # just emptied. It is the file that is compared and not the name, so the
    # other ways of naming one file are refused too, in the same words
    # ubertooth-btle uses for its own replay.
    (work / "same.bin").write_bytes(FIXTURE)
    (work / "link.bin").symlink_to("same.bin")
    os.link(work / "same.bin", work / "hard.bin")
    # Every way of naming the one file against -d, which pins that it is the
    # file being compared, and one naming against each of the other two, which
    # pins that every writer is checked.
    cases = [("-d", written) for written in
             ("same.bin", "./same.bin", "link.bin", "hard.bin")]
    cases += [("-q", "same.bin"), ("-r", "same.bin")]
    for option, written in cases:
        result = run("-i", "same.bin", option, written, directory=work)
        assert result.returncode == 1, result
        assert result.stdout == "", result
        assert result.stderr == (
            f"error: {option} would write over the dump -i is replaying"
            f" ({written})\n"), result
        assert (work / "same.bin").read_bytes() == FIXTURE, \
            (option, written, "truncated")

    # A file that is not the input is written as usual, which is what keeps
    # the check above from refusing an ordinary run.
    result = run("-i", "same.bin", "-d", "other.bin", directory=work)
    assert result.returncode == 0, result
    assert (work / "other.bin").read_bytes() == b"".join(PACKET_RECORDS), result

    # A capture on "-" whose standard output the shell has pointed at the dump
    # is the one case a name cannot catch: the run would read the records it
    # was appending and never end. "-" is this tool's own notation for
    # standard output, so descriptor 1 is what is compared for it.
    for option in ("-d", "-q"):
        with open(work / "same.bin", "ab") as appended:
            result = run("-i", "same.bin", option, "-", directory=work,
                         stdout=appended)
        assert result.returncode == 1, (option, result)
        assert result.stderr == (
            f"error: {option} would write over the dump -i is replaying"
            " (standard output)\n"), (option, result)
        assert (work / "same.bin").read_bytes() == FIXTURE, (option, result)

    # Standard output closed before the run starts. The input's own open then
    # takes descriptor 1, so standard output cannot be identified by it: a
    # named destination must still be refused, and "-" must be left to the
    # writer, which says it cannot write there.
    result = run("-i", "same.bin", "-d", "same.bin", directory=work,
                 stdout=None, close_stdout=True)
    assert result.returncode == 1, result
    assert result.stderr == ("error: -d would write over the dump -i is"
                             " replaying (same.bin)\n"), result
    assert (work / "same.bin").read_bytes() == FIXTURE, result

    result = run("-i", "same.bin", "-d", "-", directory=work,
                 stdout=None, close_stdout=True)
    assert result.returncode == 1, result
    assert "cannot write binary dump data to stdout" in result.stderr, result
    assert (work / "same.bin").read_bytes() == FIXTURE, result

    # Identifying a destination must not touch it. A named pipe with a reader
    # is where that shows: a tool that opened the pipe to look at it and closed
    # it again would end the stream for the reader, and the write that followed
    # would then block forever. So the dump arrives and the run ends.
    os.mkfifo(work / "pipe")
    collected = work / "collected.bin"
    with open(collected, "wb") as sink:
        reader = subprocess.Popen(["cat", "pipe"], cwd=work, stdout=sink)
        result = run("-i", "same.bin", "-d", "pipe", directory=work)
        reader.wait(timeout=RUN_TIMEOUT_S)
    assert result.returncode == 0, result
    assert reader.returncode == 0, reader.returncode
    assert collected.read_bytes() == b"".join(PACKET_RECORDS), result

    # Whatever kind of file the two sides share, they are one thing and the
    # command line says two. A replay fed its own dump through a named pipe
    # would never end, and one from and to /dev/null loses nothing but is
    # still asking for both.
    os.mkfifo(work / "fed")
    feeder = subprocess.Popen(["sh", "-c", "cat same.bin > fed"], cwd=work)
    result = run("-i", "fed", "-d", "fed", directory=work)
    feeder.wait(timeout=RUN_TIMEOUT_S)
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr == ("error: -d would write over the dump -i is"
                             " replaying (fed)\n"), result
    result = run("-i", "/dev/null", "-d", "/dev/null", directory=work)
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr == ("error: -d would write over the dump -i is"
                             " replaying (/dev/null)\n"), result

    # Standard output somewhere else is an ordinary dump to it, not a refusal.
    result = run("-i", "same.bin", "-d", "-", directory=work, text=False)
    assert result.returncode == 0, result
    assert result.stdout == b"".join(PACKET_RECORDS), len(result.stdout)

    # A capture on "-" is standard output rather than a file of that name, so
    # it is not refused for naming the input. This run fails on the input
    # instead, which is a file called "-" that is not there.
    result = run("-i", "-", "-d", "-", directory=work)
    assert result.returncode == 1, result
    assert result.stderr == "error: -: No such file or directory\n", result
