# How Bluetooth sniffing works

Ubertooth receives traffic it was never invited to, and the protocol gives it
no help: the parameters a receiver needs are either agreed before the traffic
starts or never sent at all. Everything Ubertooth and libbtbb do is a way of
recovering those parameters from the air. This page explains the ideas that
work is built on, for a reader who knows radio basics but not Bluetooth
internals.

Citations are to the Bluetooth Core Specification in its own form: `[Vol 2]
Part B, 7.2` means that volume, part and section. Volume 2 is the Classic
(BR/EDR) controller, volume 6 the Low Energy controller.

## The problem

**A sniffer is not a member.** A device that joins a piconet is told its
parameters: the Central's address and clock arrive in a Frequency Hop
Synchronization (FHS) packet during paging ([Vol 2] Part B, 6.5.1.4), and a
Low Energy peripheral gets them in the connection request. A sniffer is never
addressed, receives neither packet unless it happens to overhear one, and has
nobody to ask.

**One channel at a time.** Ubertooth has a single radio, 1 MHz wide. Classic
Bluetooth spreads traffic over 79 channels 1 MHz apart (f = 2402 + k MHz,
k = 0..78; [Vol 2] Part A, 2) and changes channel up to 1600 times a second
([Vol 2] Part B, 2.1), once per 625 µs slot ([Vol 2] Part B, 2.2.3). A single
radio hears roughly one slot in 79 — sweeping the band slowly, which is what
`ubertooth-rx` does by default, catches no more of them, because the hops it
is looking for are not where it happens to be. Either a sniffer predicts the
hop sequence, or it settles for scraps. Low Energy is kinder: 40
channels 2 MHz apart ([Vol 6] Part A, 2), and a connection changes channel
once per connection event, 7.5 ms to 4 s apart ([Vol 6] Part B, 4.5.1).

## Classic Bluetooth (BR/EDR)

**Piconet, Central and Peripheral.** One Central and up to seven active
Peripherals share a physical channel, and the Central controls access to it
([Vol 2] Part B, 1). The hop sequence comes from the Central's address and
clock, so there is only one set of parameters to find per piconet, and they
belong to the Central.

**BD_ADDR: LAP, UAP and NAP.** The 48-bit Bluetooth Device Address splits into
a 24-bit Lower Address Part (LAP), an 8-bit Upper Address Part (UAP) and a
16-bit Non-significant Address Part (NAP) ([Vol 2] Part B, 1.2). Only the LAP
goes on air in the clear, in every packet's access code; the UAP appears
indirectly, as the seed of two checksums; the NAP is absent from normal
traffic. That is the order a sniffer learns them in — LAP at once, UAP after
some arithmetic, NAP not at all without an FHS packet or outside knowledge.

**Access code.** Every packet starts with an access code: a 4-bit preamble, a
64-bit sync word derived from the Central's LAP, and, when a packet header
follows, a 4-bit trailer — 72 bits in all ([Vol 2] Part B, 6.3, 6.3.3).
Receivers find packets by correlating against it. Because the sync word is a
function of the LAP alone and has a known block structure, libbtbb can
recognize a valid one without knowing the LAP in advance and read the LAP back
out of it. That is what "seeing a piconet" means.

**Packet header and HEC.** The 18-bit header carries the logical transport
address, packet type, flow, acknowledge and sequence bits, and an 8-bit header
error check (HEC); rate 1/3 FEC sends it as 54 bits ([Vol 2] Part B, 6.4). The
HEC register is preloaded with the 8-bit UAP before the header is clocked
through it ([Vol 2] Part B, 7.1.1). The register is invertible, so a sniffer
runs it backwards: for a received header, exactly one UAP makes the check come
out right.

**Payload CRC.** Many payload types end in a 16-bit CRC-CCITT whose register is
loaded with the UAP in its top eight bits ([Vol 2] Part B, 7.1.2). This is a
second, independent test of the same guess, and libbtbb uses the pair
together: the HEC proposes a UAP, the CRC confirms it.

**Whitening.** Before transmission, the header and payload are XORed with the
output of a 7-bit shift register (x⁷ + x⁴ + 1) initialized with the Central's
clock bits CLK6-1 plus a leading one; this happens before FEC encoding, so a
receiver de-whitens after FEC decoding ([Vol 2] Part B, 7.2). A receiver
therefore needs those six clock bits to read anything. It cuts both ways:
there are only 64 of them, so a sniffer tries all 64, and the one whose
de-whitened header and payload satisfy the HEC and CRC hands it CLK1-6 along
with the UAP.

**FEC 1/3 and 2/3.** Rate 1/3 is plain 3-times bit repetition, used for the
whole header and for the HV1 payload ([Vol 2] Part B, 7.4). Rate 2/3 is a
(15,10) shortened Hamming code — 10 data bits, 5 parity bits, one correctable
bit error per block — used on several payload types ([Vol 2] Part B, 7.5).
Both work for a sniffer as well as for a member: they are how libbtbb reads
headers and payloads that arrived with a few bit errors, which near the edge
of range is most of them.

**CLKN, the 625 µs slot and CLK1-27.** Every device has a 28-bit native clock,
CLKN, whose lowest bit ticks every 312.5 µs — half a slot — giving a cycle of
about a day ([Vol 2] Part B, 1.1). Slots are 625 µs long and numbered by the
Central's CLK27-1 ([Vol 2] Part B, 2.2.3). Clock bits are what move the hop
sequence along, so following a piconet means knowing the Central's CLK1-27 and
keeping it.

**Hop selection.** A kernel defined in [Vol 2] Part B, 2.6 turns 28 address
bits — the LAP and the four low bits of the UAP (Table 2.3) — and the clock
into a channel number, with a period of 2²⁷ slots. It is deterministic, which
is the whole opening: given the address and the clock, a sniffer computes the
same sequence the piconet uses and arrives on each channel at the right time.

**AFH and the channel map.** Devices using adaptive frequency hopping (AFH)
drop channels that are busy or noisy, keeping at least 20 of the 79 ([Vol 2]
Part B, 2.3.1); the kernel remaps a hop that lands on an unused channel into
the used set ([Vol 2] Part B, 2.6.3). The map is negotiated over the Link
Manager Protocol ([Vol 2] Part C), which a sniffer does not see, so an unknown
map makes the predicted sequence wrong wherever remapping applies.
`ubertooth-afh` recovers it by hopping with the piconet and noting which
channels actually carry traffic.

**Clock recovery by hop reversal.** Whitening yields CLK1-6; the remaining 21
bits come from the hop sequence itself. libbtbb walks every CLK1-27 value
whose low six bits match the ones it already has, keeps those that would have
put the piconet on the channel where the first packet appeared, then discards
the candidates that disagree with each later packet's time and channel until
one survives. Each packet eliminates most of what is left, so a handful of
packets is enough — once they start arriving.

**Why discovery takes seconds.** Catching about one slot in 79, a sniffer
needs several packets with readable headers to pin the UAP and CLK1-6, then
several more to winnow CLK1-27. An idle piconet sends almost
nothing. In practice that means seconds of busy traffic — play audio to a
headset — and nothing at all from a device that is merely connected.

## Bluetooth Low Energy

**Access address.** Every LE packet starts with a 32-bit access address: the
fixed 0x8E89BED6 on the advertising channels, and a random per-connection
value on the data channels ([Vol 6] Part B, 2.1.2). Ubertooth's radio matches
it in hardware, so once it is known the board delivers framed packets instead
of raw symbols. Not knowing it is what makes an already-running connection
hard to join; `ubertooth-btle -p` tries to infer one from data-channel
traffic.

**CRC and CRCInit.** LE uses a 24-bit CRC, preset to 0x555555 on the
advertising channels and to a random per-connection CRCInit on the data
channels ([Vol 6] Part B, 3.1.1). Without the right CRCInit a sniffer cannot
tell a good packet from a corrupt one, so the init value matters as much as
the access address.

**The connection request.** When a central connects it sends `CONNECT_IND` on
an advertising channel, in the clear, and that single packet carries
everything a follower needs: the connection's access address, the CRCInit, the
transmit window, the connection interval (Interval × 1.25 ms), the 37-bit
used-channel map and the hop increment ([Vol 6] Part B, 2.3.3.1, 4.5.1).

**Why following LE is easier.** Channel selection algorithm 1 is one line:
the next unmapped channel is (last + hopIncrement) mod 37, with the hop
increment a value from 5 to 16, and unused channels remapped into the used set
([Vol 6] Part B, 4.5.8.2). Nothing has to be recovered — catch the
`CONNECT_IND` and the rest follows. The cost is that you must be listening on
the right advertising channel at the right moment. Bluetooth 5 devices often
negotiate channel selection algorithm 2 ([Vol 6] Part B, 4.5.8.3), which picks
each connection event's channel from the access address, the event counter, and
the channel map; follow mode hops with whichever algorithm the connection chose.

**Recovering a connection already in progress.** `ubertooth-btle -p` works the
four values out from the connection's own traffic instead of from
`CONNECT_IND`: the access address and CRCInit from the packets, then the
interval from the spacings between the connection's visits to one channel,
whose pattern repeats every 37 events under algorithm 1, and then the hop
increment. An algorithm-2 connection gives neither figure: its visits do not
repeat on that cycle, and it has no increment at all. Recovering one would mean
working from its arithmetic instead — searching for the event counter and
channel map whose computed channel sequence matches the channels the packets
appear on.

**Whitening by channel, not clock.** LE whitening uses the same kind of 7-bit
register (x⁷ + x⁴ + 1), but seeds it from the index of the channel the packet
is sent on ([Vol 6] Part B, 3.2). A receiver always knows which channel it is
listening to, so de-whitening needs no clock and no guessing. This is one
reason LE capture has nothing resembling Classic's clock recovery.

**PHYs.** LE defines LE 1M (mandatory), LE 2M and LE Coded ([Vol 6] Part A,
1). Ubertooth's CC2400 radio does 1 Msym/s GFSK, so it hears LE 1M only; a
connection that moves to 2M or Coded drops out of the capture.

## Which tool does which step

| Tool | Where it fits |
|---|---|
| [`ubertooth-rx -z`](../host/doc/ubertooth-rx.md) | Survey: sweep the band, or sit on one channel with `-c`, and report every LAP heard, with UAPs where it can calculate them |
| [`ubertooth-rx -l -u`](../host/doc/ubertooth-rx.md) | UAP and CLK1-6 from the HEC and CRC, CLK1-27 by hop reversal, then follow the piconet and decode it |
| [`ubertooth-afh`](../host/doc/ubertooth-afh.md) | Recover the AFH channel map of a piconet whose LAP and UAP are known |
| [`ubertooth-follow`](../host/doc/ubertooth-follow.md) | Skip the recovery: read the target's clock and AFH map through a local BlueZ adapter, then follow (Linux only) |
| [`ubertooth-btle`](../host/doc/ubertooth-btle.md) | LE: advertising only (`-n`), follow from a `CONNECT_IND` (`-f`), or infer a running connection (`-p`) |
| [`ubertooth-btbr`](../host/doc/ubertooth-btbr.md) | Take part in Classic exchanges — inquiry, paging — and capture a connection as it is set up, including the FHS packet |

**libbtbb or the firmware?** The board does what must happen in real time, the
host does what needs searching. For Classic the firmware demodulates and
streams symbols, with the radio matching only 8 bits of sync word, and libbtbb
finds the access codes, de-whitens, decodes the FEC, checks the HEC and CRC,
solves for the UAP and the clock, and writes the capture file. What the
firmware keeps is the hopping: once the host sends it the address and the
recovered clock offset, the board computes the hop sequence itself and stays
with the piconet, because there is no time for a USB round trip inside a
625 µs slot. For LE the split moves — the radio matches the full access
address, so the board delivers framed packets and hops on its own from the
`CONNECT_IND` parameters, and libbtbb's LE side mostly decodes and prints.

## Where to go next

- [Sniffing Classic Bluetooth](source/sniffing_BR_EDR.rst) — the practical
  workflow, including how to find the NAP.
- [Capturing BLE in Wireshark](source/capturing_BLE_Wireshark.rst) — follow
  mode in practice, and why you may see only advertisements.
- [Bluetooth captures in PCAP](source/bluetooth_captures_PCAP.rst) — what ends
  up in a capture file.
