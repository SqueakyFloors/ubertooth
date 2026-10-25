#!/usr/bin/env python3
"""Verify a capture written by ubertooth-rx keeps time across the clock wrap.

The board's 100 ns capture clock wraps every 3125 * 2^20 ticks (327.68 s), so
any capture longer than that crosses a wrap. Extending the clock as a 32-bit
counter put a 101.8 s step into every PCAP and PcapNG timestamp after the first
wrap.

A wrap is also lost when more than a whole period passes between two packets,
because the clock then reads higher than it did and there is nothing to infer
one from. A Classic packet carries the period count the firmware keeps, which
covers such a gap; a capture whose packets carry none has the loss reported
against the host time of each record instead. Both are replayed here.

The fixtures are built here, so no board and no saved capture is needed: a
replay reads the record format ``ubertooth-rx -d`` writes (a big-endian 32-bit
host time, then one 64-byte usb_pkt_rx), and the access code of each record is
computed from the Bluetooth Core specification (Vol 2, Part B 6.3.3) in the bit
order libbtbb decodes. Only the times relative to the first packet are checked,
since where a capture is anchored is a separate question.
"""

import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BINARY = sys.argv[1]

SYMBOLS = 400                       # the 50 data bytes, most significant bit first
PREAMBLE_AT = 16                    # where the access code starts in those symbols
BR_PACKET = 0
LAP = 0x5A3C96                      # the LAP every record carries
CLK100NS_PERIOD = 3125 * 2 ** 20    # the clock's period, in 100 ns ticks
STEP_TICKS = 1_000_000_000          # 100 s of device clock between records
FIRST_TICKS = 100_000_000           # the first record's clock, away from the wrap
RECORDS = 8                         # 700 s of clock: two wraps
AGE_S = 7200                        # the first record was written this long ago

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


def record(host_time, clk100ns, channel, clkn_high=0):
    """One replay record of a decodable packet at this clock value.

    The clock is packed little-endian, which is how the host reads it, and
    clkn_high is the period count a board sends with a Classic packet.
    """
    bits = [(sync_word(LAP) >> i) & 1 for i in range(64)]
    symbols = [0] * PREAMBLE_AT
    symbols += [1, 0, 1, 0] if bits[0] else [0, 1, 0, 1]
    symbols += bits
    symbols += [0] * (SYMBOLS - len(symbols))
    data = bytes(
        sum(bit << (7 - j) for j, bit in enumerate(symbols[i:i + 8]))
        for i in range(0, SYMBOLS, 8)
    )
    header = struct.pack("<BBBBIbbbB2x", BR_PACKET, 0, channel, clkn_high,
                         clk100ns, -40, -90, -70, 1)
    return struct.pack(">I", host_time) + header + data


BASE_TIME = int(time.time()) - AGE_S


def fixture(ticks, counted=True):
    """A replay of decodable packets at these clock values.

    The host time of each record follows the device clock, as a real capture's
    does. counted is what a board sends with a Classic packet: the number of
    times the capture clock has started over. A mode that sends no count
    (ubertooth-btle -f and -n) leaves the field zero.
    """
    return b"".join(
        record(BASE_TIME + (t - ticks[0]) // 10_000_000,
               t % CLK100NS_PERIOD, 3 + 7 * i,
               (t // CLK100NS_PERIOD) & 0xFF if counted else 0)
        for i, t in enumerate(ticks)
    )


def offsets_ns(ticks):
    """The times each packet should carry, relative to the first."""
    return [100 * (t - ticks[0]) for t in ticks]


# Records 100 s of device clock apart, so the sequence crosses the wrap twice.
TICKS = [FIRST_TICKS + i * STEP_TICKS for i in range(RECORDS)]
assert max(TICKS) // CLK100NS_PERIOD == 2, "the fixture must cross two wraps"

# Four records 100 s apart, a gap of two whole periods and 10 s, then four
# more. The gap leaves the clock reading a little higher than it did, so
# nothing in the sequence of clock values shows that it started over twice;
# only the count each record carries does.
GAP = 2 * CLK100NS_PERIOD + 100_000_000
LOST_PERIODS = 2
GAP_TICKS = [FIRST_TICKS + i * STEP_TICKS for i in range(4)]
GAP_TICKS += [GAP_TICKS[3] + GAP + i * STEP_TICKS for i in range(4)]
assert len(GAP_TICKS) == RECORDS
assert GAP_TICKS[4] % CLK100NS_PERIOD > GAP_TICKS[3] % CLK100NS_PERIOD, \
    "the gap must leave the clock reading higher, not lower"
assert GAP_TICKS[4] // CLK100NS_PERIOD - GAP_TICKS[3] // CLK100NS_PERIOD \
    == LOST_PERIODS, "the gap must cross two whole periods"


def pcap_times(path):
    """Nanoseconds of each record in a nanosecond-resolution pcap."""
    data = path.read_bytes()
    assert len(data) > 24, f"pcap is only {len(data)} bytes"
    magic = {bytes.fromhex("4d3cb2a1"): "<", bytes.fromhex("a1b23c4d"): ">"}
    order = magic.get(data[:4])
    assert order, f"not a nanosecond pcap: magic {data[:4].hex()}"
    out = []
    offset = 24
    while offset + 16 <= len(data):
        seconds, nanoseconds, length, _ = struct.unpack_from(order + "4I", data, offset)
        out.append(seconds * 1_000_000_000 + nanoseconds)
        offset += 16 + length
    return out


def pcapng_times(path):
    """Nanoseconds of each enhanced packet block in a pcapng.

    The interface block states the resolution, so a file written with another
    one fails as a resolution mismatch rather than as odd times.
    """
    data = path.read_bytes()
    order = "<" if data[8:12] == bytes.fromhex("4d3c2b1a") else ">"
    out = []
    offset = 0
    while offset + 12 <= len(data):
        block_type, length = struct.unpack_from(order + "2I", data, offset)
        if length < 12 or offset + length > len(data):
            break
        if block_type == 1:                     # interface description
            options = offset + 12 + 8
            while options + 4 <= offset + length - 4:
                code, size = struct.unpack_from(order + "2H", data, options)
                if code == 0:
                    break
                if code == 9:                   # if_tsresol
                    assert data[options + 4] == 9, \
                        f"timestamps are 10^-{data[options + 4]} s, not ns"
                options += 4 + (size + 3) // 4 * 4
        if block_type == 6:                     # enhanced packet
            high, low = struct.unpack_from(order + "2I", data, offset + 12)
            out.append((high << 32) | low)
        offset += length
    return out


def offsets(times):
    """The times relative to the first one."""
    return [t - times[0] for t in times]


def replay(data, work):
    """Replays a fixture through ubertooth-rx and returns the run and the
    PCAP and PcapNG times it wrote, each relative to its first packet."""
    (work / "wrap.bin").write_bytes(data)
    # libbtbb refuses to overwrite a capture file, so a second replay in the
    # same directory starts from nothing.
    for name in ("wrap.pcap", "wrap.pcapng"):
        (work / name).unlink(missing_ok=True)
    result = subprocess.run(
        [BINARY, "-i", "wrap.bin", "-q", "wrap.pcap", "-r", "wrap.pcapng"],
        capture_output=True, text=True, timeout=30, check=False, cwd=work,
        # A replay opens no device; the switch keeps it that way if this run
        # ever reaches device selection.
        env=dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1"))
    assert result.returncode == 0, result
    pcap = pcap_times(work / "wrap.pcap")
    pcapng = pcapng_times(work / "wrap.pcapng")
    assert len(pcap) == RECORDS, (pcap, result)
    assert len(pcapng) == RECORDS, (pcapng, result)
    return result, offsets(pcap), offsets(pcapng)


with tempfile.TemporaryDirectory() as workdir:
    work = Path(workdir)

    # A wrap the records cross with no gap: seen either from the count each
    # one carries or, where a mode sends none, from the clock reading lower
    # than it did.
    for counted in (True, False):
        result, pcap, pcapng = replay(fixture(TICKS, counted), work)
        assert pcap == offsets_ns(TICKS), (counted, pcap, result)
        assert pcapng == offsets_ns(TICKS), (counted, pcapng, result)

        # Said plainly: no packet is further from the one before it than the
        # step between the records, so no wrap adds 101.8 s of its own.
        for times, name in ((pcap, "pcap"), (pcapng, "pcapng")):
            steps = [b - a for a, b in zip(times, times[1:])]
            assert steps == [100 * STEP_TICKS] * (RECORDS - 1), \
                (counted, name, steps, result)
        assert "warning:" not in result.stderr, result.stderr

    # A gap of more than a whole period in the middle. The count the records
    # carry puts every later packet where it belongs and nothing is said.
    result, pcap, pcapng = replay(fixture(GAP_TICKS), work)
    assert pcap == offsets_ns(GAP_TICKS), (pcap, result)
    assert pcapng == offsets_ns(GAP_TICKS), (pcapng, result)
    assert "warning:" not in result.stderr, result.stderr

    # The same gap in records carrying no count: the wrap cannot be seen, so
    # the later packets come out two periods early, and that is reported
    # against the host time each record carries rather than passed in silence.
    result, pcap, pcapng = replay(fixture(GAP_TICKS, counted=False), work)
    lost_ns = 100 * LOST_PERIODS * CLK100NS_PERIOD
    expected = [t - (lost_ns if i >= 4 else 0)
                for i, t in enumerate(offsets_ns(GAP_TICKS))]
    assert pcap == expected, (pcap, result)
    assert pcapng == expected, (pcapng, result)
    assert "warning: packet times are more than the capture clock's 327.68 s "\
        "period behind the host clock" in result.stderr, result.stderr
    # Said once, not once a packet.
    assert result.stderr.count("warning:") == 1, result.stderr

