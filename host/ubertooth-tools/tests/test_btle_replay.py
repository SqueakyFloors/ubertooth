#!/usr/bin/env python3
"""Verify ubertooth-btle -d writes a dump and -R replays it.

A dump holds the records the board sent, in the format ``ubertooth-rx -d``
writes: a big-endian 32-bit host time, then one 64-byte usb_pkt_rx. The
fixture here is built from scratch, so no board and no saved capture is
needed. It holds the five parameter records promiscuous mode sends while it
works out which connection it has found, and one advertising packet, which is
the pair a replay has to put back: the parameters say which connection the
packets belong to and nothing else in the file does.
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
DATA_LEN = 50                   # usb_pkt_rx.data
LE_PACKET = 1
LE_PROMISC = 5
# The record's channel byte, counted from 2402 MHz: 0 is advertising channel
# 37, which is what makes the decoder read the packet below as an
# advertisement rather than as connection data.
ADVERTISING_CHANNEL = 0
DATA_CHANNEL = 10
ADVERTISING_AA = 0x8E89BED6
AGE_S = 3600                    # the records were written this long ago
CLK100NS_STEP = 1_000_000       # 0.1 s of device clock between records
# How long one ubertooth-btle run is given. The whole test takes well under a
# second on an idle machine, over a handful of replays, so half a minute is
# many times what any of them needs. It is kept that wide on purpose: on a
# machine busy with parallel builds the cost that grows is starting a process,
# not the replay, and the CTest timeout on this test is larger again, so this
# deadline is the one that fires and says the run timed out.
RUN_TIMEOUT_S = 30

# The five parameters promiscuous mode recovers, as the state byte it sends
# each one with, the value, and the line the tool prints for it.
PROMISC_PARAMETERS = (
    (0, struct.pack("<I", ADVERTISING_AA), "Access Address: 8e89bed6"),
    (1, struct.pack("<I", 0x555555), "CRC Init: 555555"),
    (2, struct.pack("<H", 24), "Hop interval: 30 ms"),
    (3, struct.pack("<B", 7), "Hop increment: 7"),
    (4, struct.pack("<H", 1234), "Algorithm 2 event counter: 1234"),
)


def record(host_time, clk100ns, pkt_type, data, channel=DATA_CHANNEL):
    """One dump record: the host time it arrived, then the usb_pkt_rx."""
    assert len(data) <= DATA_LEN, len(data)
    header = struct.pack("<BBBBIbbbB2x", pkt_type, 0, channel, 0, clk100ns,
                         -40, -90, -70, 1)
    body = data + bytes(DATA_LEN - len(data))
    return struct.pack(">I", host_time) + header + body


def advertisement(access_address=ADVERTISING_AA):
    """An ADV_IND, as the board hands one over: the access address, the PDU
    header, the advertiser's address, its flags, and three CRC bytes."""
    payload = bytes.fromhex("665544332211") + bytes.fromhex("020106")
    pdu = bytes([0x00, len(payload)]) + payload
    return struct.pack("<I", access_address) + pdu + bytes(3)


def unreadable_field():
    """An ADV_IND whose first advertising field claims more bytes than the
    packet carries, which libbtbb decodes as an advertisement and whose
    advertising data it then refuses."""
    payload = bytes.fromhex("111111111111") + bytes([15, 0x09]) + b"a" * 9
    pdu = bytes([0x00, len(payload)]) + payload
    return struct.pack("<I", ADVERTISING_AA) + pdu


BASE_TIME = int(time.time()) - AGE_S
RECORDS = [
    record(BASE_TIME + index, CLK100NS_STEP * (index + 1), LE_PROMISC,
           bytes([state]) + value)
    for index, (state, value, _) in enumerate(PROMISC_PARAMETERS)
]
PACKET_TIME = BASE_TIME + len(RECORDS)
RECORDS.append(record(PACKET_TIME, CLK100NS_STEP * (len(RECORDS) + 1),
                      LE_PACKET, advertisement(), ADVERTISING_CHANNEL))
FIXTURE = b"".join(RECORDS)
PARAMETER_LINES = ["LE Promisc - " + line for _, _, line in PROMISC_PARAMETERS]


def run(*arguments, directory, text=True):
    return subprocess.run(
        [BINARY, *arguments],
        capture_output=True,
        text=text,
        timeout=RUN_TIMEOUT_S,
        check=False,
        cwd=directory,
        # A replay opens no device; the switch keeps it that way if one of
        # these runs ever reaches device selection. LC_ALL, because a case
        # below compares a strerror() string and glibc translates those.
        env=dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1", LC_ALL="C"),
    )


def packet_lines(output):
    return [line for line in output.splitlines() if line.startswith("systime=")]


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


def pcap_records(path):
    """(seconds, nanoseconds) of each record in a libbtbb nanosecond pcap."""
    data = path.read_bytes()
    assert len(data) > 24, f"pcap is only {len(data)} bytes"
    order = "<" if data[:4] == bytes.fromhex("4d3cb2a1") else ">"
    out = []
    offset = 24
    while offset + 16 <= len(data):
        seconds, nanoseconds, length, _ = struct.unpack_from(order + "4I", data,
                                                             offset)
        out.append((seconds, nanoseconds))
        offset += 16 + length
    return out


with tempfile.TemporaryDirectory() as workdir:
    work = Path(workdir)
    (work / "full.bin").write_bytes(FIXTURE)

    # The whole fixture, replayed as the promiscuous capture that recorded it:
    # every parameter record is printed again, the packet carries the host
    # time of its own record rather than the time of the replay, and the
    # replay ends cleanly.
    result = run("-p", "-R", "full.bin", directory=work)
    assert result.returncode == 0, result
    for line in PARAMETER_LINES:
        assert line in result.stdout, (line, result)
    assert len(packet_lines(result.stdout)) == 1, result
    assert f"systime={PACKET_TIME} " in result.stdout, result
    assert f"addr={ADVERTISING_AA:08x}" in result.stdout, result
    # Read as an advertisement, which is what channel 37 and that access
    # address make it, with the advertiser's own address in it.
    assert "ADV_IND" in result.stdout, result
    assert "11:22:33:44:55:66" in result.stdout, result
    assert result.stderr == "", result

    # -d during a replay rewrites the records it read, byte for byte: that is
    # what makes a dump of a promiscuous capture worth keeping, since the
    # parameter records are the only note of which connection it found.
    result = run("-p", "-R", "full.bin", "-d", "again.bin", directory=work)
    assert result.returncode == 0, result
    assert (work / "again.bin").read_bytes() == FIXTURE, result

    # The decoding options work on a replay as they do on live data, and each
    # case needs a record the option changes the outcome for.
    #
    # -x is the access address filter. The advertisement below carries an
    # address two bits from the advertising one, so the default of 32 allowed
    # offenses decodes it and -x 0 drops it.
    (work / "offenses.bin").write_bytes(
        record(BASE_TIME, CLK100NS_STEP, LE_PACKET,
               advertisement(access_address=ADVERTISING_AA ^ 0x00000003),
               ADVERTISING_CHANNEL))
    result = run("-p", "-R", "offenses.bin", directory=work)
    assert result.returncode == 0, result
    assert len(packet_lines(result.stdout)) == 1, result
    result = run("-p", "-R", "offenses.bin", "-x", "0", directory=work)
    assert result.returncode == 0, result
    assert packet_lines(result.stdout) == [], result

    # -v asks the decoder to describe what it could not read rather than only
    # counting it. The advertisement below claims 15 bytes of local name and
    # carries 9, which is what noise decoded as an advertisement mostly looks
    # like, so the decoder refuses that field.
    (work / "malformed.bin").write_bytes(
        record(BASE_TIME, CLK100NS_STEP, LE_PACKET, unreadable_field(),
               ADVERTISING_CHANNEL))
    quiet = run("-p", "-R", "malformed.bin", directory=work)
    assert quiet.returncode == 0, quiet
    # Without -v the run only counts it, in one line when it ends, on the
    # stream it prints the packets on.
    assert quiet.stdout.endswith("note: 1 packet held data that could not be"
                                 " decoded; -v describes each one\n"), quiet
    assert quiet.stderr == "", quiet
    loud = run("-p", "-R", "malformed.bin", "-v", directory=work)
    assert loud.returncode == 0, loud
    # With it, the decoder describes the field on standard error, and the note
    # no longer has to point at -v.
    assert loud.stdout.endswith("note: 1 packet held data that could not be"
                                " decoded\n"), loud
    assert loud.stderr != "", loud

    # A PcapNG capture written during a replay carries the recorded time too,
    # in nanoseconds, as the PCAP one does.
    result = run("-p", "-R", "full.bin", "-r", "cap.pcapng", directory=work)
    assert result.returncode == 0, result
    assert pcapng_times(work / "cap.pcapng") == [
        PACKET_TIME * 1_000_000_000], result

    # No output file may be the dump being replayed: each is opened for
    # writing, which truncates it, so the dump would be emptied before a
    # record of it was read. It is the file that is compared and not the name,
    # so the other ways of naming one file are refused too.
    (work / "same.bin").write_bytes(FIXTURE)
    (work / "link.bin").symlink_to("same.bin")
    os.link(work / "same.bin", work / "hard.bin")
    # Every way of naming the one file against -d, which pins that it is the
    # file being compared, and one naming against each of the other three,
    # which pins that every writer is checked.
    cases = [("-d", "d", written) for written in
             ("same.bin", "./same.bin", "link.bin", "hard.bin")]
    cases += [("-q", "q", "same.bin"), ("-c", "c", "same.bin"),
              ("-r", "r", "same.bin")]
    for option, letter, written in cases:
        result = run("-p", "-R", "same.bin", option, written, directory=work)
        assert result.returncode == 1, result
        assert result.stdout == "", result
        assert result.stderr == (
            f"error: -{letter} would write over the dump -R is replaying"
            f" ({written})\n"), result
        assert (work / "same.bin").read_bytes() == FIXTURE, \
            (option, written, "truncated")

    # A file that is not the input is written as usual, which is what keeps
    # the check above from refusing an ordinary run.
    result = run("-p", "-R", "same.bin", "-d", "other.bin", directory=work)
    assert result.returncode == 0, result
    assert (work / "other.bin").read_bytes() == FIXTURE, result

    # The same read as the follow mode's dump instead. The two modes stamp
    # their packets with different clocks, which is why the mode has to be
    # named, but the host time a record carries is the same either way.
    result = run("-f", "-R", "full.bin", directory=work)
    assert result.returncode == 0, result
    assert f"systime={PACKET_TIME} " in result.stdout, result

    # A capture written during a replay is stamped from the recorded times,
    # not from the wall clock: its first packet lands on the host time of that
    # packet's own record. The parameter records carry no packet, so the
    # capture holds the one advertisement.
    result = run("-p", "-R", "full.bin", "-q", "cap.pcap", directory=work)
    assert result.returncode == 0, result
    assert pcap_records(work / "cap.pcap") == [(PACKET_TIME, 0)], result

    # "-d -" hands the dump this process's standard output, so the records go
    # there whole and the lines the tool prints move to standard error.
    result = run("-p", "-R", "full.bin", "-d", "-", directory=work, text=False)
    assert result.returncode == 0, result
    assert result.stdout == FIXTURE, len(result.stdout)
    for line in PARAMETER_LINES:
        assert line.encode() in result.stderr, (line, result.stderr[:400])

    # A run whose dump took standard output counts nothing: the decode stops
    # before the field that would be counted, so there is no note at all and
    # none that could point at -v, which such a run is refused.
    result = run("-p", "-R", "malformed.bin", "-d", "-", directory=work,
                 text=False)
    assert result.returncode == 0, result
    assert result.stderr == b"", result.stderr[-200:]

    # Records this tool does not recognize are read to the end and decoded as
    # nothing: a run that prints nothing may be the wrong file rather than a
    # quiet capture.
    (work / "other.dump").write_bytes(
        record(BASE_TIME, CLK100NS_STEP, 0, b"", DATA_CHANNEL))
    result = run("-p", "-R", "other.dump", directory=work)
    assert result.returncode == 0, result
    assert result.stdout == "" and result.stderr == "", result

    # Standard output carries one stream, so a dump and a PCAP capture cannot
    # both have it: the one that asked second would write to /dev/null without
    # a word. The message names the option the user wrote.
    for option, letter in (("-q", "q"), ("-c", "c")):
        result = run("-p", "-R", "full.bin", option, "-", "-d", "-",
                     directory=work)
        assert result.returncode == 1, result
        assert result.stdout == "", result
        assert result.stderr == (f"error: -{letter} and -d cannot both write"
                                 " to standard output\n"), result

    # A last record cut short: the whole records before it are replayed, the
    # leftover bytes are named, and the command ends with an error.
    (work / "cut.bin").write_bytes(FIXTURE[:-10])
    result = run("-p", "-R", "cut.bin", directory=work)
    assert result.returncode == 1, result
    assert packet_lines(result.stdout) == [], result
    assert PARAMETER_LINES[-1] in result.stdout, result
    assert (f"error: cut.bin ends in a partial record: {RECORD_LEN - 10} of "
            f"{RECORD_LEN} bytes, ignored" in result.stderr), result

    # A replay that could not read one whole record leaves no capture behind,
    # as a refused run does: a PcapNG holding only its 64 KB header looks like
    # a capture that caught something. The input stays where it is.
    (work / "stub.bin").write_bytes(FIXTURE[:20])
    result = run("-p", "-R", "stub.bin", "-q", "left.pcap", "-r",
                 "left.pcapng", "-d", "left.bin", directory=work)
    assert result.returncode == 1, result
    for name in ("left.pcap", "left.pcapng", "left.bin"):
        assert not (work / name).exists(), (name, sorted(os.listdir(work)))
    assert (work / "stub.bin").read_bytes() == FIXTURE[:20], result

    # From the first whole record on the files stay, however little of the
    # input came after it: the replay ran, and what it decoded is the result.
    (work / "cut2.bin").write_bytes(FIXTURE[:RECORD_LEN + 20])
    result = run("-p", "-R", "cut2.bin", "-d", "kept.bin", directory=work)
    assert result.returncode == 1, result
    assert (work / "kept.bin").read_bytes() == RECORDS[0], result

    # An empty file is a clean end of file, not an error.
    (work / "empty.bin").write_bytes(b"")
    result = run("-p", "-R", "empty.bin", directory=work)
    assert result.returncode == 0, result
    assert result.stdout == "" and result.stderr == "", result

    # A file that cannot be read: reported, and exit 1 rather than looking
    # like a capture that caught nothing. fopen() succeeds on a directory, the
    # first read then fails; where it does not, the open error is reported.
    (work / "adir").mkdir()
    result = run("-p", "-R", "adir", directory=work)
    assert result.returncode == 1, result
    assert ("error: reading adir failed:" in result.stderr
            or "error: adir: " in result.stderr), result
    assert packet_lines(result.stdout) == [], result

    # A missing file is reported on standard error, with standard output left
    # empty for whatever is reading the replay.
    result = run("-p", "-R", "missing.bin", directory=work)
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr.startswith("error: missing.bin: "), result

    # A replay is read with the clock of the mode that recorded the dump, and
    # a dump does not record which mode that was, so a replay without one is
    # refused rather than read with a guess.
    result = run("-R", "full.bin", directory=work)
    assert result.returncode == 1, result
    assert result.stdout == "", result
    assert result.stderr == (
        "error: -R needs -f, -n or -p: a dump does not record which mode"
        " wrote it, and the modes stamp their packets with different"
        " clocks\n"), result
