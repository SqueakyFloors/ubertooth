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


def run(*arguments, directory):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_S,
        check=False,
        cwd=directory,
        # A replay opens no device; the switch keeps it that way if one of
        # these runs ever reaches device selection.
        env=dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1"),
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
    # nothing on standard output, nothing decoded, and no file left behind.
    #
    # The reason is the errno of the writer's own failed open, so this covers
    # both halves of what a user sees. A writer that printed a line of its own
    # would make the stream two lines, and a reason read after the close and
    # the free that follow the create would be those calls' errno instead of
    # this one's. Both of those are what a libbtbb older than the commit
    # FindBTBB.cmake pins does, so a failure here with "PCAP error:" in it is
    # an installed libbtbb behind the pin rather than a change to this tool.
    for option, name in (("-q", "cap.pcap"), ("-r", "cap.pcapng")):
        inside_missing_dir = Path("nodir") / name
        result = run("-i", "full.bin", option, str(inside_missing_dir),
                     directory=work)
        assert result.returncode == 1, result
        assert result.stdout == "", result
        assert result.stderr == (
            f"error: {inside_missing_dir}: No such file or directory\n"), result
        assert not (work / inside_missing_dir).exists(), result

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
