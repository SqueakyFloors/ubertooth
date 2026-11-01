#!/usr/bin/env python3
"""ubertooth-rx says when its receiver is being driven past what it can measure.

The fixtures are recorded RSSI sequences, replayed through ``-i``: the dump
format is a big-endian 32-bit host time followed by one 64-byte usb_pkt_rx, so
a dump can carry any sequence of signal figures the bench produced.

What the bench read at 2441 MHz, on two lab boards, 2026-11-01:

* With nothing of ours transmitting, 26 of 19937 records reached the top of
  the radio's scale (raw 35, where the host's dBm figure stops rising), 0.13%,
  and the median record read raw -37. The channel carries Wi-Fi, so that is
  the ordinary loud-neighbour case the tool has to stay quiet about.
* With another board a few centimeters away transmitting a carrier, every
  record read raw 37, at every one of the eight transmit power levels.
* With that board about a meter away behind a 20 dB attenuator, the carrier
  read raw -11 at power level 0 rising to raw 16 at level 7, and no record
  reached the ceiling at any level -- so a link that works does not get near
  it even at full power.
"""

import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BINARY = sys.argv[1]

BR_PACKET = 0
SIGNAL_VALID = 0x80
CLK100NS_STEP = 4_000           # 0.4 ms of device clock between records
RUN_TIMEOUT_S = 60
WARNING = "warning: the signal is at the top of the receiver's range"

# What the measurement above gives the fixtures: the raw reading a pinned
# receiver sat at, the quiet channel's median, and its 26 records in 19937.
PINNED_RAW = 37
QUIET_RAW = -37
QUIET_RECORDS = 19937
QUIET_AT_CEILING = 26
# The strongest the attenuated link reached, at power level 7: loud, and
# nowhere near the top of the scale.
WORKING_LINK_RAW = 16


def record(host_time, clk100ns, rssi_avg):
    """One Basic Rate record stating a signal figure, carrying no packet.

    The Basic Rate stream sends one of these for every block of symbols it
    collects, whether or not a packet was in it, which is why a capture can
    see its receiver pinned while decoding nothing at all.
    """
    header = struct.pack("<BBBBIbbbB2x", BR_PACKET, SIGNAL_VALID, 39, 0,
                         clk100ns, rssi_avg, rssi_avg - 6, rssi_avg, 50)
    return struct.pack(">I", host_time) + header + bytes(50)


def dump(levels):
    """A dump of one record per raw reading in `levels`."""
    base = int(time.time()) - 3600
    return b"".join(
        record(base + i // 2500, CLK100NS_STEP * (i + 1), level)
        for i, level in enumerate(levels)
    )


def run(*arguments, directory):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_S,
        check=False,
        cwd=directory,
        env=dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1", LC_ALL="C"),
    )


with tempfile.TemporaryDirectory() as workdir:
    work = Path(workdir)

    # The quiet channel, with its ceiling records put next to each other --
    # the worst arrangement for a rule that judges a block of records at a
    # time. Nothing is said.
    quiet = [QUIET_RAW] * 1000 + [PINNED_RAW] * QUIET_AT_CEILING
    quiet += [QUIET_RAW] * (QUIET_RECORDS - len(quiet))
    (work / "quiet.bin").write_bytes(dump(quiet))
    result = run("-i", "quiet.bin", directory=work)
    assert result.returncode == 0, result
    assert WARNING not in result.stderr, result

    # The link that works, at the transmit power that drives it hardest.
    (work / "working.bin").write_bytes(dump([WORKING_LINK_RAW] * QUIET_RECORDS))
    result = run("-i", "working.bin", directory=work)
    assert result.returncode == 0, result
    assert WARNING not in result.stderr, result

    # The pinned receiver, said once however long it stays there.
    (work / "pinned.bin").write_bytes(dump([PINNED_RAW] * QUIET_RECORDS))
    result = run("-i", "pinned.bin", directory=work)
    assert result.returncode == 0, result
    assert result.stderr.count(WARNING) == 1, result
    # On standard error and not on standard output, which carries only what
    # was asked for.
    assert WARNING not in result.stdout, result

    # And with the dump on standard output, where standard output belongs to
    # the capture for the whole run: the warning must not be mixed into it.
    result = run("-i", "pinned.bin", "-d", "-", directory=work)
    assert result.returncode == 0, result
    assert result.stderr.count(WARNING) == 1, result
    assert WARNING not in result.stdout, result

    # Too short a capture to judge a block of records says nothing: the first
    # 999 records of the pinned fixture.
    (work / "short.bin").write_bytes(dump([PINNED_RAW] * 999))
    result = run("-i", "short.bin", directory=work)
    assert result.returncode == 0, result
    assert WARNING not in result.stderr, result

print("ALL TESTS PASSED")
