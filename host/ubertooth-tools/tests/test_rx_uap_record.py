#!/usr/bin/env python3
"""Verify the capture record of the packet that settles the UAP carries it.

Following a piconet means learning its upper address part (UAP) from the
packets themselves: the decoder tries every clock value until one of them
unwhitens a packet into a payload whose CRC is correct, and that packet is
what settles the UAP. ubertooth-rx writes the UAP into every capture record
as the reference UAP, which is what a reader checks the HEC and the CRC
against. The settling packet's own record used to go out with no reference
UAP at all -- the one record a reader most needs it on, because it is the
evidence the UAP was derived from.

The fixture is a replay of three records built from scratch here, so no board
and no saved capture is needed: an ID packet with no header, then a DH1 whose
payload CRC settles the UAP, then another ID packet. Everything a real board
would send is computed from the Bluetooth Core specification: the access code
(Vol 2, Part B 6.3.3), the header's HEC and the payload CRC (7.1 and 7.2),
the header's 1/3 rate repetition code (7.4) and the payload whitening (7.3).
"""

import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BINARY = sys.argv[1]

SYMBOLS = 400                   # the 50 data bytes, unpacked most significant bit first
PREAMBLE_AT = 16                # where the access code starts in those symbols
BR_PACKET = 0
LAP = 0x5A3C96                  # the LAP every record here carries
UAP = 0x5B                      # the UAP the DH1 below settles
AGE_S = 3600                    # the records were written this long ago

# The 64-bit scrambling sequence applied to the sync word, the (64,30)
# expurgated BCH generator (octal 260534236651), and the Barker sequence
# appended to the LAP, chosen by the LAP's top bit.
PN = 0x83848D96BBCC54FC
BCH_GENERATOR = 0o260534236651
BARKER = {0: 0b101100, 1: 0b010011}

# The 127-bit whitening sequence, generated from its own recurrence, and
# where in it a packet whitened at CLK6-1 = 0 begins. Every record here is
# whitened at that clock value, which is also the first one the decoder tries.
WHITENING = [1, 1, 1, 0, 0, 0, 1]
while len(WHITENING) < 127:
    WHITENING.append(WHITENING[-7] ^ WHITENING[-3])
WHITENING_AT_CLOCK_0 = 99
# The 18 header bits sit ahead of the payload in the sequence, so a payload
# starts that much further along than the header does.
HEADER_BITS = 18

# The HEC generator polynomial, reflected: x^8 + x^7 + x^5 + x^2 + x + 1.
HEC_POLYNOMIAL = 0x65

DH1 = 4                         # the packet type of the settling packet
LT_ADDR = 3                     # which member of the piconet sent it
PAYLOAD_BODY = b"\x01\x02\x03\x04"

# The clock the settling record is captured at, chosen so that the packet's
# own CLK1-6 is 0: the clock it is whitened at, so the fixture is the capture
# a real board would have made. ubertooth-rx derives CLKN from the reading and
# the access code's position, which is why the number is not a round one.
SETTLING_CLK100NS = 3800 + 1280 * 3125
ID_CLK100NS = (1_000_000, 9_000_000)

# Where the reference LAP and UAP, and the flags, sit in a
# LINKTYPE_BLUETOOTH_BREDR_BB record, and the flag that says the reference
# UAP in it is there to be used.
REF_LAP_UAP_AT = 12
FLAGS_AT = 20
BREDR_REFUAP_VALID = 0x0080
UAP_ANY = 0xFF                  # what a record with no reference UAP carries


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
    """The preamble, sync word and trailer of a LAP, as symbols."""
    bits = [(sync_word(lap) >> i) & 1 for i in range(64)]
    preamble = [1, 0, 1, 0] if bits[0] else [0, 1, 0, 1]
    last = bits[63]
    trailer = [1 - last, last, 1 - last, last]
    return preamble + bits + trailer


def whitened(bits, skip):
    """bits whitened at CLK6-1 = 0, starting skip bits into the packet."""
    index = (WHITENING_AT_CLOCK_0 + skip) % 127
    out = []
    for bit in bits:
        out.append(bit ^ WHITENING[index])
        index = (index + 1) % 127
    return out


def air_order(value, count):
    """A field as symbols: least significant bit first, as it goes on air."""
    return [(value >> i) & 1 for i in range(count)]


def reversed_bits(value):
    out = 0
    for i in range(8):
        out |= ((value >> i) & 1) << (7 - i)
    return out


def hec(header_data, uap):
    """The header-error check of a 10-bit header, seeded with the UAP.

    The check is a shift register seeded with the UAP and clocked by the ten
    header bits, so a receiver that runs it backwards from the check recovers
    the UAP the sender used. That is how the decoder learns a candidate UAP
    from every clock value it tries, and running it forwards here is what
    makes one of those candidates the UAP this fixture means.
    """
    register = reversed_bits(uap)
    for i in range(10):
        feedback = (register & 1) ^ ((header_data >> i) & 1)
        shifted = ((feedback << 7) | (register >> 1)) & 0xFF
        register = shifted ^ (HEC_POLYNOMIAL if feedback else 0)
    return register


def payload_crc(bits, uap):
    """The 16-bit payload CRC of these payload symbols, seeded with the UAP."""
    register = (reversed_bits(uap) << 8) & 0xFF00
    for bit in bits:
        register = (register >> 1) | (((register & 1) ^ bit) << 15)
        register ^= (register & 0x8000) >> 5
        register ^= (register & 0x8000) >> 12
    return register


def dh1_symbols(lap, uap, body):
    """A DH1 whose payload CRC is correct for this UAP at CLK6-1 = 0."""
    header = air_order(LT_ADDR, 3) + air_order(DH1, 4) + [1, 1, 0]
    header += air_order(hec(sum(bit << i for i, bit in enumerate(header)),
                            uap), 8)
    assert len(header) == HEADER_BITS, header
    # Whitened first, then sent at a third of the rate: every whitened bit
    # three times.
    coded = [bit for bit in whitened(header, 0) for _ in range(3)]

    # A one-byte payload header: the length of the body, no flow stop, and
    # the start of an L2CAP message.
    payload = air_order((len(body) << 3) | 0b010, 8)
    for byte in body:
        payload += air_order(byte, 8)
    payload += air_order(payload_crc(payload, uap), 16)

    return access_code_symbols(lap) + coded + whitened(payload, HEADER_BITS)


def id_symbols(lap):
    """An ID packet: an access code, and header symbols that disagree.

    A board sees these whenever a device is being found or paged. The decoder
    reads no header from one, so it learns nothing about the UAP, which is
    what makes these the records on either side of the settling packet.
    """
    return access_code_symbols(lap) + [1, 0, 0] * HEADER_BITS


def record(host_time, clk100ns, channel, symbols):
    """One replay record: a big-endian host time, then a 64-byte usb_pkt_rx."""
    symbols = [0] * PREAMBLE_AT + list(symbols)
    symbols += [0] * (SYMBOLS - len(symbols))
    assert len(symbols) == SYMBOLS, len(symbols)
    data = bytes(
        sum(bit << (7 - j) for j, bit in enumerate(symbols[i:i + 8]))
        for i in range(0, SYMBOLS, 8)
    )
    header = struct.pack("<BBBBIbbbB2x", BR_PACKET, 0, channel, 0, clk100ns,
                         -40, -90, -70, 1)
    return struct.pack(">I", host_time) + header + data


def capture_records(path):
    """(reference LAP, reference UAP, flags) of each record of a capture."""
    data = path.read_bytes()
    assert len(data) > 24, f"capture is only {len(data)} bytes"
    out = []
    offset = 24
    while offset + 16 <= len(data):
        length = struct.unpack_from("<I", data, offset + 8)[0]
        body = data[offset + 16:offset + 16 + length]
        reference = struct.unpack_from("<I", body, REF_LAP_UAP_AT)[0]
        flags = struct.unpack_from("<H", body, FLAGS_AT)[0]
        out.append((reference & 0xFFFFFF, reference >> 24, flags))
        offset += 16 + length
    return out


BASE_TIME = int(time.time()) - AGE_S
FIXTURE = b"".join([
    record(BASE_TIME, ID_CLK100NS[0], 3, id_symbols(LAP)),
    record(BASE_TIME + 1, SETTLING_CLK100NS, 17,
           dh1_symbols(LAP, UAP, PAYLOAD_BODY)),
    record(BASE_TIME + 2, ID_CLK100NS[1], 40, id_symbols(LAP)),
])

with tempfile.TemporaryDirectory() as workdir:
    work = Path(workdir)
    (work / "piconet.bin").write_bytes(FIXTURE)
    result = subprocess.run(
        [BINARY, "-i", "piconet.bin", "-l", "%06x" % LAP, "-q", "capture.pcap"],
        capture_output=True, text=True, timeout=30, check=False, cwd=work,
        # A replay opens no device; the switch keeps it that way if this run
        # ever reaches device selection.
        env=dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1"),
    )
    assert result.returncode == 0, result
    assert "error" not in result.stderr, result
    # The decoder says which packet settled the UAP, so a fixture that stopped
    # settling it would be a plain failure rather than a quiet one.
    assert "UAP = 0x%x found" % UAP in result.stdout, result

    records = capture_records(work / "capture.pcap")
    assert len(records) == 3, records
    # What each record says, so that running this by hand shows the capture
    # the assertions below are about.
    for index, (lap, uap, flags) in enumerate(records):
        print("record %d: reference LAP=%06x UAP=%02x flags=%#06x"
              % (index, lap, uap, flags))

    # Before the UAP is known there is nothing to record, so the first record
    # carries no reference UAP and does not claim to.
    assert records[0] == (LAP, UAP_ANY, records[0][2]), records
    assert not records[0][2] & BREDR_REFUAP_VALID, records

    # The packet that settles the UAP carries it, flag and all: this record is
    # the evidence the UAP came from, and it used to go out saying nothing.
    assert records[1][:2] == (LAP, UAP), records
    assert records[1][2] & BREDR_REFUAP_VALID, records

    # So does every record after it, which is all that ever worked.
    assert records[2][:2] == (LAP, UAP), records
    assert records[2][2] & BREDR_REFUAP_VALID, records
