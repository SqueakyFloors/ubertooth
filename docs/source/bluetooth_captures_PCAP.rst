==========================
Bluetooth Captures in PCAP
==========================

This page describes the capture files the Ubertooth tools write today, as
libbtbb writes them. It is a reference for anyone reading such a file with
something other than Wireshark, or writing one from another tool.

Two link types carry Ubertooth captures: ``LINKTYPE_BLUETOOTH_BREDR_BB`` for
BR/EDR baseband packets and ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`` for LE link
layer packets. Each prefixes every packet with a pseudo-header holding the
receiver's own metadata: the channel, signal and noise levels, the reference
address the capture was listening for, and flags saying what the decoder
checked. libbtbb hardcodes the link type numbers, so it does not need a
libpcap that knows them: 255 for BR/EDR baseband, 256 for LE with
pseudo-header, 251 for bare LE link layer, and 192 for PPI.

The writers assume a little-endian host. Every multi-octet field in a
pseudo-header, and in a PCAPNG option's value, is little-endian; a PCAPNG
block header and option header are in the byte order of the file's section
header block, which these writers set from the host.


What each tool writes
~~~~~~~~~~~~~~~~~~~~~

.. list-table ::
  :header-rows: 1
  :widths: 2 1 3

  * - Tool and option
    - Format
    - Link type
  * - ``ubertooth-rx -r``, ``ubertooth-follow -r``
    - PCAPNG
    - ``LINKTYPE_BLUETOOTH_BREDR_BB``
  * - ``ubertooth-rx -q``, ``ubertooth-follow -q``
    - PCAP
    - ``LINKTYPE_BLUETOOTH_BREDR_BB``
  * - ``ubertooth-btle -r``
    - PCAPNG
    - ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR``
  * - ``ubertooth-btle -q``
    - PCAP
    - ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR``
  * - ``ubertooth-btle -c``
    - PCAP
    - ``DLT_PPI``, wrapping ``DLT_BLUETOOTH_LE_LL``
  * - ``btle-extcap.py``
    - PCAP into Wireshark's fifo
    - ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR``

The Wireshark extcap program runs ``ubertooth-btle -q`` on the fifo Wireshark
opened, so Wireshark sees exactly what a ``-q`` capture file holds.

A PCAP file is written with the nanosecond magic number ``0xa1b23c4d``, so a
record's second field counts nanoseconds rather than microseconds. The snaplen
is the longest record the writer can produce.

A PCAPNG file carries ``shb_userappl`` of ``libbtbb`` in its section header
block. Each tool passes ``Ubertooth`` as the interface description, and the
writer adds ``if_tsresol`` of 9, nanoseconds, when the caller did not supply
one.


LINKTYPE_BLUETOOTH_BREDR_BB
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Covers BR/EDR baseband packets (Bluetooth spec Vol 2, Part B). Each record is
the pseudo-header below, followed by the decoded payload when the decoder
produced one. The sync word is not stored; the Lower Address Part (LAP)
recovered from it is. The packet header is stored decoded, in
``bt_header``.

::

	typedef struct __attribute__((packed)) _pcap_bluetooth_bredr_bb_header {
	        uint8_t  rf_channel;
	        int8_t   signal_power;
	        int8_t   noise_power;
	        uint8_t  access_code_offenses;
	        uint8_t  payload_transport_rate;
	        uint8_t  corrected_header_bits;
	        int16_t  corrected_payload_bits;
	        uint32_t lap;
	        uint32_t ref_lap_uap;
	        uint32_t bt_header;
	        uint16_t flags;
	        uint8_t  bredr_payload[BREDR_MAX_PAYLOAD];
	} pcap_bluetooth_bredr_bb_header;

``BREDR_MAX_PAYLOAD`` is 400, which is room for the longest payload the
decoder produces, a DH5's 343 bytes. A record is only as long as the payload
it actually carries.

**rf_channel** ranges 0 to 78 (Vol 2, Part A, Section 2).

**signal_power** and **noise_power** are dBm.

**access_code_offenses** is how many bits of the access code deviated from the
expected one.

**payload_transport_rate** is two nibbles: the high nibble is the logical
transport (0 unknown, 1 SCO, 2 eSCO, 3 ACL, 4 CSB) and the low nibble the
modulation (0 for BR with GFSK, 1 for EDR with pi/2-DQPSK, 2 for EDR with
8DPSK). The format also defines the whole byte as 0xff for an ID packet, which
has neither a header nor a payload. The Ubertooth demodulates BR only and does
not work out the transport, so every record these tools write holds 0x00 here,
and a reader should not take it as evidence that the packet was ACL.

**corrected_header_bits** is how many of the header's 18 bits the
forward error correction corrected.

**corrected_payload_bits** is the decoder's count for the payload in this
record, and is zero when the record carries no payload. The format has no
validity flag for either corrected-bits field, so a reader takes both at face
value; the payload's count is only recorded alongside a payload.

**lap** is the 24-bit LAP recovered from the sync word (Vol 2, Part B, Section
6.3.3), with the top byte zero.

**ref_lap_uap** holds the reference LAP the capture was listening for in its
low 24 bits and the reference Upper Address Part (UAP) in its top byte. Each
half is only meaningful when its flag is set.

**bt_header** is the 18-bit packet header (Vol 2, Part B, Section 6.4), with
the top 14 bits zero.

**flags** is described below.

**bredr_payload** is the decoded payload (Vol 2, Part B, Section 6), present
only when flag 0x0020 is set.


BR/EDR record flags
^^^^^^^^^^^^^^^^^^^

Both BR/EDR writers build the same flag word; most of it comes from
``bredr_record_flags()``, which they share.

.. list-table ::
  :header-rows: 1
  :widths: 1 2 3

  * - Bit
    - Meaning
    - When it is set
  * - 0x0001
    - the header and payload are de-whitened
    - always: the decoder de-whitens before the record is built
  * - 0x0002
    - ``signal_power`` is valid
    - always
  * - 0x0004
    - ``noise_power`` is valid
    - when the noise level the caller passed is below the signal level
  * - 0x0010
    - the reference LAP is valid and led to this capture
    - when the caller passed a LAP rather than ``LAP_ANY``
  * - 0x0020
    - a payload is present and follows the flags
    - when the decoder produced a payload for this packet. A NULL or POLL
      packet, and a candidate the clock search rejected, leave it clear
  * - 0x0080
    - the reference UAP is valid for header check and CRC checking
    - when the caller passed a UAP rather than ``UAP_ANY``
  * - 0x0100
    - the header's Header Error Check was checked
    - whenever the library decoded a header
  * - 0x0200
    - the Header Error Check passed
    - when that check succeeded
  * - 0x0400
    - the payload's CRC was checked
    - whenever the packet type carries a CRC and the library checked it
  * - 0x0800
    - the CRC passed
    - when that check succeeded

The two check flags are independent of the reference UAP flag: a record can
report a check with 0x0080 clear, because the library may have settled the UAP
from the packet itself.

The remaining bits defined for this link type are reserved and these writers
never set them: 0x0008 payload decrypted, 0x0040 channel subject to aliasing,
0x1000 message integrity check checked, and 0x2000 that check passed. Neither
writer decrypts, so no MIC is ever verified.

``ubertooth-rx`` and ``ubertooth-follow`` pass the LAP they were given, and the
UAP the decoder has settled on by the time the packet is written. So a record
from a followed piconet has both reference flags set, and a survey, which has
no target LAP, can still carry a reference UAP once the decoder has recovered
one for that LAP.


LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Covers LE link layer packets (Bluetooth spec Vol 6, Part B). Each record is
the pseudo-header below, followed by the access address, the PDU, and the CRC,
without the preamble.

::

	typedef struct __attribute__((packed)) _pcap_bluetooth_le_ll_header {
	        uint8_t  rf_channel;
	        int8_t   signal_power;
	        int8_t   noise_power;
	        uint8_t  access_address_offenses;
	        uint32_t ref_access_address;
	        uint16_t flags;
	        uint8_t  le_packet[0];
	} pcap_bluetooth_le_ll_header;

**rf_channel** is the LE channel index, 0 to 39 (Vol 6, Part A, Section 2).

**signal_power** and **noise_power** are dBm.

**access_address_offenses** is how many bits of the access address deviated
from the expected one.

**ref_access_address** is the access address the capture was listening for.
``ubertooth-btle`` passes the advertising access address 0x8e89bed6 for a
packet on an advertising channel and zero for a data packet, so the flag below
is set for advertising packets only.

The captured frame is the decoded packet's length plus 9 framing bytes: 4 for
the access address, 2 for the PDU header, and 3 for the CRC. It is truncated
to 264 bytes, the size of the decoder's symbol buffer. In PCAPNG the block's
original length still carries the untruncated length.


LE record flags
^^^^^^^^^^^^^^^

Both LE writers build the same flag word.

.. list-table ::
  :header-rows: 1
  :widths: 1 2 3

  * - Bit
    - Meaning
    - When it is set
  * - 0x0001
    - the packet is de-whitened
    - always
  * - 0x0002
    - ``signal_power`` is valid
    - always
  * - 0x0004
    - ``noise_power`` is valid
    - when the noise level the caller passed is below the signal level
  * - 0x0010
    - ``ref_access_address`` is valid and led to this capture
    - when the caller passed a non-zero access address. Zero cannot be a real
      one, since Vol 6, Part B, Section 2.1.2 forbids four equal octets
  * - 0x0020
    - ``access_address_offenses`` is valid
    - always

The other bits defined for this link type are reserved and these writers never
set them: 0x0008 packet decrypted, 0x0040 channel subject to aliasing, 0x0400
CRC checked, 0x0800 CRC passed, 0x1000 message integrity check checked, and
0x2000 that check passed. A reader that wants a CRC verified has to do it
itself.


DLT_PPI captures
~~~~~~~~~~~~~~~~

``ubertooth-btle -c`` writes a ``DLT_PPI`` file instead, for tools that expect
the older layout. Each record is a PPI packet header naming
``DLT_BLUETOOTH_LE_LL``, one PPI field of type 30006, and then the LE frame.
The field holds a version byte, the channel as a frequency in MHz, the high
byte of the board's clock, the packet's 100 ns timestamp, and the maximum,
minimum, and mean RSSI with the number of samples they came from. This layout
has no flags and no reference access address, so it says nothing about what
was checked.


PCAPNG interface options
~~~~~~~~~~~~~~~~~~~~~~~~

A PCAPNG capture can carry session metadata that no single packet holds. It
goes in options of the interface description block, which the writer keeps
mapped so options can be added while the capture runs. Option codes have their
most significant bit set, which PCAPNG reserves for local use, and all of
these start with 0xd3.

An option's code and length are in the file's byte order, and the length
counts the value only, not the four bytes of the option header itself.

The room for these options is set aside when the file is created, rounded up
to whole memory pages. Adding an option that would not fit, together with the
end-of-options marker and the block's trailing length, leaves the file
unchanged and reports that it did not fit.


BD_ADDR, code 0xd340
^^^^^^^^^^^^^^^^^^^^

Records a Bluetooth device address the capture relates to, so a reader knows
whose packets these are. Its value is 8 bytes:

::

	typedef struct __attribute__((packed)) {
	        uint8_t bd_addr[6];
	        uint8_t uap_mask;
	        uint8_t nap_valid;
	} bredr_bd_addr_info;

**bd_addr** is the 48-bit address, little-endian: the LAP in the first three
bytes, the UAP in the fourth, and the Network Address Part in the last two.

**uap_mask** has a bit set for each UAP bit known with certainty, so 0xff when
the whole UAP is known and 0x00 when none of it is.

**nap_valid** is a flag in its least significant bit; the other bits are
reserved and zero.

``ubertooth-rx -r`` writes one of these when it was given a target address,
with ``uap_mask`` 0xff if the UAP was given too and 0x00 if it was not.
``ubertooth-follow -r`` writes one with ``uap_mask`` 0xff. Neither knows a
Network Address Part, so ``nap_valid`` is always zero. The library allows more
than one of these for the same address, as long as each later one is more
certain than the last; the tools write one per capture.


Clock, code 0xd341
^^^^^^^^^^^^^^^^^^

Records the piconet clock of a device, and the moment the value was true, so a
reader can line a capture up with the hop sequence. Its value is 20 bytes:

::

	typedef struct __attribute__((packed)) {
	        uint64_t ts;
	        uint32_t lap_uap;
	        uint32_t clk;
	        uint32_t clk_mask;
	} bredr_clk_info;

**ts** is the reference instant, in nanoseconds, on the same scale as the
packet timestamps.

**lap_uap** is the LAP in its low 24 bits and the UAP in its top byte.

**clk** is the device's native clock, and **clk_mask** has a bit set for each
clock bit known with certainty, which lets a capture record an estimate that
improves over the session.

libbtbb exposes this through ``btbb_pcapng_record_btclock()``. No Ubertooth
tool calls it today, so no capture the tools write carries this option.


LE connection info, code 0xd380
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Records the parameters of an LE connection, which is what a reader needs to
follow the connection's data packets: the access address to match, the CRC
initial value to verify them with, and the timing, channel map, and hop
increment to place them. Its value is 42 bytes, the timestamp followed by the
34 payload bytes of the ``CONNECT_IND`` PDU, unchanged:

::

	typedef struct __attribute__((packed)) {
	        uint64_t ns;
	        union {
	                struct {
	                        uint8_t InitA[6];
	                        uint8_t AdvA[6];
	                        uint8_t AA[4];
	                        uint8_t CRCInit[3];
	                        uint8_t WinSize;
	                        uint8_t WinOffset[2];
	                        uint8_t Interval[2];
	                        uint8_t Latency[2];
	                        uint8_t Timeout[2];
	                        uint8_t ChM[5];
	                        uint8_t HopSCA;
	                } fields;
	                uint8_t bytes[0];
	        } pdu;
	} le_ll_connection_info;

**ns** is when the option became true, which is the timestamp of the packet it
came from.

The PDU fields are defined in Vol 6, Part B, Sections 2.3.3.1 and 2.4.2.1.
``InitA`` is the initiator's address and ``AdvA`` the advertiser's, so the
connection's central and peripheral can both be named. ``HopSCA`` packs the
hop increment and the sleep clock accuracy into one byte.

The LE PCAPNG writer adds one of these by itself whenever the packet it has
just written is a ``CONNECT_REQ`` on an advertising channel whose payload is
at least 34 bytes long. Nothing else writes one, and the PCAP writers cannot:
a PCAP file has no place to put it.


Wideband RF info, code 0xd300
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

An option for capture tools that deliberately alias several Bluetooth channels
into one narrow baseband, so a reader can work out which channels a packet
might have come from. Its value is 16 bytes:

::

	typedef struct __attribute__((packed)) {
	        uint32_t centre_freq;
	        uint32_t analog_bandwidth;
	        int32_t  intermediate_freq;
	        uint32_t sampling_bandwidth;
	} bt_wideband_rf_info;

Every field is in Hz: the center of the capture, the passband width of the
analog section measured from center to band edge, the intermediate frequency
relative to the center, and the digital sampling bandwidth.

libbtbb defines the code and the structure and never writes one. It also has no
public function to write one, so no capture any Ubertooth tool produces carries
it.


Long LE captures run out of room for connections
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Each connection option takes 48 bytes of the room set aside in the interface
description block when the file was created, and that room cannot grow once
the first packet has been written. A capture that sees a few hundred
connections fills it.

Past that point the packets still reach the file and the connection
parameters do not. ``lell_pcapng_append_packet()`` says so by returning
``LELL_PCAPNG_CONNECT_INFO_NOT_RECORDED``, a positive value, which is not a
write failure: the packet was written. ``ubertooth-btle`` prints one warning on
standard error the first time it happens and carries on, since every later
connection is in the same position. Those connections cannot be followed in
Wireshark. Starting a new capture file gives the next ones room again.


Reading these captures in Wireshark
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Wireshark dissects ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`` itself, so an LE
capture needs no plugin.

BR/EDR needs one of the two plugins in libbtbb's ``wireshark/plugins``
directory: ``btbb`` for the baseband and Link Manager Protocol, and
``btbredr`` for the same with EDR. Both are for Wireshark 2.0 and later, and
each builds on its own with its own CMake project; the libbtbb build does not
build them. The README beside each one has the build and install instructions.
By default the build installs into a ``.wireshark`` directory in your home
directory; Wireshark 4 reads compiled plugins from a per-version subdirectory
of its plugin folder instead, so install it there. Help, About Wireshark,
Folders names the folders your build reads.

``wireshark/plugins-legacy`` holds the same two for Wireshark 1.x, along with
the ``btatt``, ``btle``, and ``btsm`` dissectors that Wireshark has carried
itself since 1.12. On Wireshark 1.12 or later, only ``btbb`` and ``btbredr``
are needed.

To capture into Wireshark with no file in between, install
``btle-extcap.py``; see its man page for where Wireshark looks for extcap
programs.
