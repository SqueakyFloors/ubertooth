#!/usr/bin/env python3
"""Verify the capture record of the packet that settles the UAP carries it.

Following a piconet means learning its upper address part (UAP) from the
packets themselves: the decoder tries every clock value until one of them
unwhitens a packet into a payload whose CRC is correct, and that packet is
what settles the UAP. ubertooth-rx writes the UAP into every capture record
as the reference UAP, which is what a reader checks that record's CRC
against. The settling packet's own record used to go out with no reference
UAP at all -- the one record a reader most needs it on, because it is the
first evidence the UAP was derived from.

The fixture is a replay of four records built from scratch here, so no board
and no saved capture is needed: an ID packet with no header, a DH1 whose CRC
leaves every clock candidate alive, the DH1 whose CRC settles the UAP, then
another ID packet. Everything a real board would send is computed from the
Bluetooth Core specification: the access code (Vol 2, Part B 6.3.3), the
header's HEC and the payload CRC (7.1 and 7.2), the header's 1/3 rate
repetition code (7.4) and the payload whitening (7.3).
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

# The clocks the two DH1 records are captured at, chosen so that each
# packet's own CLK1-6 is 0: the clock they are whitened at, so the fixture is
# the capture a real board would have made. ubertooth-rx derives CLKN from the
# reading and the access code's position, which is why the numbers are not
# round ones. Both land on the same CLK1-6, which is what lets the decoder
# carry its clock candidates from the first packet to the second.
UNSETTLED_CLK100NS = 3800 + 1152 * 3125
SETTLING_CLK100NS = 3800 + 1280 * 3125
ID_CLK100NS = (1_000_000, 9_000_000)

# Where the reference LAP and UAP, and the flags, sit in a
# LINKTYPE_BLUETOOTH_BREDR_BB record, and the flag that says the reference
# UAP in it is there to be used.
DLT_BLUETOOTH_BREDR_BB = 255
REF_LAP_UAP_AT = 12
FLAGS_AT = 20
PAYLOAD_AT = 22
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


def dh1_symbols(lap, uap, body, settles=True):
    """A DH1 at CLK6-1 = 0, with a payload CRC correct for this UAP.

    settles=False spoils the CRC instead. The decoder cannot tell that from a
    packet it has the wrong clock for, or an encrypted one, so it keeps every
    clock candidate and the UAP stays unsettled: a header-bearing packet that
    leaves the piconet mid-acquisition.
    """
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
    check = payload_crc(payload, uap)
    payload += air_order(check if settles else check ^ 1, 16)

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
    """Each record of a capture: reference LAP, reference UAP, flags, payload.

    The field offsets below are those of the basic-rate baseband link type, so
    the link type in the file header is checked rather than assumed: another
    one would be read as this one and give neighbouring fields instead.
    """
    data = path.read_bytes()
    assert len(data) > 24, f"capture is only {len(data)} bytes"
    # libbtbb writes the header as a struct, so the file is in this machine's
    # own byte order; the magic says which that is.
    order = "<" if data[:4] == bytes.fromhex("4d3cb2a1") else ">"
    link_type = struct.unpack_from(order + "I", data, 20)[0]
    assert link_type == DLT_BLUETOOTH_BREDR_BB, link_type
    out = []
    offset = 24
    while offset + 16 <= len(data):
        length = struct.unpack_from(order + "I", data, offset + 8)[0]
        body = data[offset + 16:offset + 16 + length]
        reference = struct.unpack_from(order + "I", body, REF_LAP_UAP_AT)[0]
        flags = struct.unpack_from(order + "H", body, FLAGS_AT)[0]
        out.append((reference & 0xFFFFFF, reference >> 24, flags,
                    body[PAYLOAD_AT:]))
        offset += 16 + length
    return out


def pcapng_records(path):
    """The same, from the enhanced packet blocks of a pcapng."""
    data = path.read_bytes()
    # The byte order of the file, from the magic in its first block. Checked
    # rather than guessed: a file that holds neither way round would be read
    # as big-endian and every field would come out wrong.
    assert data[8:12] in (bytes.fromhex("4d3c2b1a"), bytes.fromhex("1a2b3c4d")), \
        data[:12].hex()
    order = "<" if data[8:12] == bytes.fromhex("4d3c2b1a") else ">"
    out = []
    offset = 0
    while offset + 12 <= len(data):
        block, length = struct.unpack_from(order + "2I", data, offset)
        if length < 12 or offset + length > len(data):
            break
        if block == 6:
            # The block holds the interface, two words of time, the captured
            # length, the length the packet had on air, and then that many
            # captured bytes. The block is padded to a multiple of four, so
            # the captured length is what bounds the record.
            captured = struct.unpack_from(order + "I", data, offset + 20)[0]
            body = data[offset + 28:offset + 28 + captured]
            reference = struct.unpack_from(order + "I", body, REF_LAP_UAP_AT)[0]
            flags = struct.unpack_from(order + "H", body, FLAGS_AT)[0]
            out.append((reference & 0xFFFFFF, reference >> 24, flags,
                        body[PAYLOAD_AT:]))
        offset += length
    return out


def crc_recomputes(payload, uap):
    """Whether a record's payload passes its own CRC under this UAP.

    The payload a record carries is dewhitened, so the CRC over all but its
    last two bytes has to come back as those two bytes. This is the check a
    reader runs with the reference UAP the record gives it, and it only comes
    out right for one UAP, which is what makes the recorded value worth
    anything.
    """
    bits = []
    for byte in payload:
        bits += air_order(byte, 8)
    body, check = bits[:-16], bits[-16:]
    return payload_crc(body, uap) == sum(bit << i
                                        for i, bit in enumerate(check))


BASE_TIME = int(time.time()) - AGE_S
FIXTURE = b"".join([
    record(BASE_TIME, ID_CLK100NS[0], 3, id_symbols(LAP)),
    record(BASE_TIME + 1, UNSETTLED_CLK100NS, 11,
           dh1_symbols(LAP, UAP, PAYLOAD_BODY, settles=False)),
    record(BASE_TIME + 2, SETTLING_CLK100NS, 17,
           dh1_symbols(LAP, UAP, PAYLOAD_BODY)),
    record(BASE_TIME + 3, ID_CLK100NS[1], 40, id_symbols(LAP)),
])

with tempfile.TemporaryDirectory() as workdir:
    work = Path(workdir)
    (work / "piconet.bin").write_bytes(FIXTURE)
    result = subprocess.run(
        [BINARY, "-i", "piconet.bin", "-l", "%06x" % LAP,
         "-q", "capture.pcap", "-r", "capture.pcapng"],
        capture_output=True, text=True, timeout=10, check=False, cwd=work,
        # A replay opens no device; the switch keeps it that way if this run
        # ever reaches device selection.
        env=dict(os.environ, UBERTOOTH_TEST_NO_DEVICES="1"),
    )
    assert result.returncode == 0, result
    assert "error" not in result.stderr, result
    # The decoder says which packet settled the UAP and how, so a fixture that
    # stopped settling it on a CRC -- by settling it some other way, or not at
    # all -- is a plain failure rather than a quiet one. It takes two packets:
    # the first DH1 leaves the candidates alive, the second settles it.
    assert ("Correct CRC! UAP = 0x%x found after 2 total packets" % UAP
            in result.stdout), result

    # Both capture formats are written from the same reference UAP, so both
    # are checked: libbtbb assembles the flags separately for each.
    for name, records in (("pcap", capture_records(work / "capture.pcap")),
                          ("pcapng", pcapng_records(work / "capture.pcapng"))):
        assert len(records) == 4, (name, records)
        # What each record says, so that running this by hand shows the
        # capture the assertions below are about.
        for index, (lap, uap, flags, payload) in enumerate(records):
            print("%s record %d: reference LAP=%06x UAP=%02x flags=0x%04x "
                  "payload=%s" % (name, index, lap, uap, flags,
                                  payload.hex() or "none"))

        # The first two records are written before the UAP is known: one with
        # no header to learn it from, one whose header the decoder has but
        # whose CRC left every clock candidate alive. Neither carries a
        # reference UAP, and neither claims to.
        for index in (0, 1):
            assert records[index][:2] == (LAP, UAP_ANY), (name, records)
            assert not records[index][2] & BREDR_REFUAP_VALID, (name, records)

        # The packet that settles the UAP carries it, flag and all: this
        # record is the first evidence the UAP came from, and it used to go
        # out saying nothing. Its payload passes its own CRC under the UAP the
        # record gives and under no other, which is the check the reference
        # UAP is recorded for and the one this record could not be put
        # through before.
        assert records[2][:2] == (LAP, UAP), (name, records)
        assert records[2][2] & BREDR_REFUAP_VALID, (name, records)
        assert crc_recomputes(records[2][3], records[2][1]), (name, records)
        assert not crc_recomputes(records[2][3], UAP ^ 1), (name, records)

        # So does every record after it, which is all that ever worked.
        assert records[3][:2] == (LAP, UAP), (name, records)
        assert records[3][2] & BREDR_REFUAP_VALID, (name, records)
