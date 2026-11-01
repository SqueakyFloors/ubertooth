# ubertooth-tx(1) -- transmit from an Ubertooth

## SYNOPSIS

    ubertooth-tx -g PAYLOAD -a SYNCWORD [-c MHZ] [-n COUNT] [-U <index|serial>] [-V]
    ubertooth-tx -l LAP -u UAP [-t SECONDS] [-U <index|serial>] [-V]

## DESCRIPTION

Transmits from an Ubertooth. There are two transmits, and a command line asks
for one of them.

`-g` sends one generic packet: a payload you give in hex, framed by the radio
with the sync word `-a` gives it. It is the transmitting half of the generic
radio mode, so what it puts on the air is not Bluetooth, and another Ubertooth
reads it with `ubertooth-dump -g`. **It is experimental**; see THE GENERIC
PACKET below.

`-l` and `-u` are meant to retransmit symbols captured with ubertooth-dump
toward the piconet that address names. **That half sends no dump.** Handing
the dumped symbols to the board is not written yet, and the usage text says
so. The board does transmit meanwhile: the firmware sends one fixed 16-byte
packet per pair of slots, holding the target's access code, a trailer byte,
and seven bytes of filler, and hops along the target piconet's sequence until
the run is interrupted or its `-t` timeout passes. So the command puts a
signal on the air, carrying the address it was given and nothing from a dump.
Do not rely on it to retransmit a capture.

## OPTIONS

 - `-g <payload>` :
   transmit one generic packet carrying these bytes, written as hex digits,
   two per byte and no separators: `-g 0102ff` is three bytes. 1 to 32 bytes,
   which is one load of the radio's FIFO and all a receiving board reports.
   Experimental.
 - `-a <syncword>` :
   the 32-bit sync word the board frames the generic packet with, 8 hex
   digits; required with `-g`. The whole of it is written out, because the
   radio sends all four bytes.
 - `-c <2402-2480>` :
   the channel the generic packet goes out on, in MHz (default: 2402). That
   range is the Bluetooth band, which is what every tool here tunes, and it is
   narrower than the radio: the synthesizer reaches 2268 to 2794 MHz and the
   board will take any of it, so a receiving board can be set outside the band
   with `ubertooth-debug -w %fsdiv=`. Transmitting outside the band is a
   question about what you are licensed to radiate rather than about the
   radio, so this option does not offer it.
 - `-n <1-65535>` :
   transmit the generic packet this many times (default: 1). One request
   carries the whole burst, so the board sends them back to back; the ceiling
   is what that request can hold.
 - `-l <LAP>` :
   target LAP (3 bytes / 6 hex digits) for the dump retransmit.
 - `-u <UAP>` :
   target UAP (1 byte / 2 hex digits) for the dump retransmit.
 - `-t <seconds>` :
   timeout for the dump retransmit; 0 means no timeout (default: 0).
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-V` :
   print the version: the host tools, then the firmware version and build of
   each attached board; see ubertooth(7), COMMON OPTIONS
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value, a stray word or
   an option given twice is still reported.

## THE GENERIC PACKET

Experimental: expect the options and what goes on the air to change.

The board hands the framing of a generic packet to the radio, so the sync word
`-a` gives it, plus the framing the firmware programs, is the whole description
of what goes out: 6 preamble bytes of `01010101`, the whole 32-bit sync word,
and the radio's CRC, which is GRMDM `0x0f71`. A board reading the packet has
to be set to the same thing, and with the CRC bit set it can tell a packet
with a bit error in it from a good one -- this framing has no other check. A
32-byte payload is the length that verdict has been measured at: the receive
reads a fixed 32 bytes and the radio answers the CRC question wherever the
read stopped. ubertooth-dump(1), THE GENERIC RECEIVE has the detail. ubertooth(7), GENERIC RADIO MODE lists the registers both
ends have to agree on and walks a packet from one board to another; this page
covers only what the options do.

Two things come from the command line and reach the radio for this one packet:
the sync word and the channel. The power is the board's own, as TRANSMIT POWER
below says. Everything else is the configuration already in the radio, which
`ubertooth-debug -w` writes.

`-g` stops at 32 bytes, which is one load of the radio's FIFO. The request
itself carries up to 50, but a longer payload is written to the radio a FIFO
at a time, each piece after the radio has sent the last, and what that leaves
on the air has not been observed: an Ubertooth receiver reads one FIFO's worth
per sync word, so it cannot tell one packet from two. 32 bytes is also all a
receiving board reports, and a shorter payload is followed in its record by
whatever was next in the radio's FIFO.

`-n` is carried in the one request, so the board sends the burst back to back
without the host saying anything more. That is not only faster -- about 1300
packets a second against 440 for a request per packet -- it is what makes a
burst arrive at all: the board answers no USB request while a packet is going
out, so a host sending a request per packet as fast as it can loses the ones
that land in those windows. Measured on two boards, a burst of 30 sent that
way had 15 come back as transfer errors and 15 packets reached the air; one
request for 2000 packets put 2000 on the air in 1.6 seconds and a board set to
the same framing read all 2000.

The command waits for the burst to finish, so it ends when the air is clear.
What it waits on is the board leaving the generic transmit mode, which it
cannot tell from another command stopping the burst: a `ubertooth-util -S` from
elsewhere ends this command as a finished burst would.
While it runs the board reports the generic transmit as its mode, which is
what `ubertooth-util -w` shows and what this waits on. Ctrl-C stops the burst:
the board takes a stop between packets, and what has not gone out is not
sent.

## TRANSMIT POWER

Both transmits use the CC2400 power level set with `ubertooth-util -a` and
leave it unchanged. A board that has not been given a level transmits at 7
(0 dBm), the CC2400's reset value. The generic packet carries that level in
its request, because the level belongs to the one packet, and the board puts
its own back once the packet is out. These levels are at the CC2400 radio; the
Ubertooth One's RF amplifier changes the actual output after that.

Two boards a few centimeters apart at level 7 are too loud for each other: a
receiver reads nothing from them. For a test over a short path, turn the
transmitting board down to 0 or 1, or put an attenuator in front of the
receiving one.

## ARGUMENTS

`ubertooth-tx` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: sync word must be 8 hex digits (got cb87)` or
`error: a generic payload must be hex digits, two per byte, 1 to 50 bytes
(got 010)`.

A command line has to name one of the two transmits, and a command line that
names neither is refused before any device is opened, so the message never
depends on which boards are attached: `error: nothing to do: give -g to
transmit a generic packet, or -l and -u to retransmit to a piconet`.

The two cannot be combined, since a generic packet is framed by the sync word
`-a` gives it and carries no piconet address, and it ends when the packets
`-n` asked for have gone out rather than after a timeout:
`error: -g cannot be combined with -l; a generic packet carries no piconet
address`, and the same for `-u` and for `-t`.

Each half needs its own options, and nothing else:

 - `-g` needs `-a`, because the board has no sync word of its own and the sync
   word is the whole of what a receiving board has to be set to:
   `error: -g needs -a <syncword>: the board frames the generic packet with
   it`.
 - `-a`, `-c` and `-n` belong to the generic packet, so without `-g` they ask
   for nothing: `error: -a, -c and -n are only used with -g, which transmits a
   generic packet`.
 - the dump retransmit needs both halves of the target address:
   `error: -l <LAP> and -u <UAP> are both required`. Half an address reads the
   same whether or not one of the generic packet's options was written beside
   it.

## DIAGNOSTICS

What `ubertooth-tx` does happens over the air, so it writes no result:
everything it prints goes to standard error, and standard output stays empty.
A generic transmit prints nothing at all when it works. Each error is one line
starting `error: `.

A command line the tool will not take is reported as that one `error: ` line
on standard error, with no usage text after it: the line describes the
problem, and `-h` prints the usage text for a reader who wants it. An option
the tool does not know reads `error: unrecognized option '-Z'`; one whose
value is missing reads `error: option '-l' requires a value`.

`-h` is what the user asked for, so the usage text goes to standard output
and nothing goes to standard error. A bare `ubertooth-tx`, with no arguments
at all, is the one refusal the usage text does follow, on standard error with
the `error: ` line.

## EXIT STATUS

0 when the command did what was asked, including `-h`, after a generic
transmit has handed over every packet it was asked for, and after a dump
retransmit is interrupted (Ctrl-C) or its `-t` timeout passes. 1 for a command
line the tool will not take and when the selected Ubertooth is not found or
cannot be opened.

A run stopped by Ctrl-C before it had opened a board exits 0 as well: see
ubertooth(7), SELECTING A DEVICE. Ctrl-C during a generic transmit exits 0
too, with the packets not yet handed over not sent.

1 as well when the board refuses a request the transmit needs: the power level
a generic packet carries, or a generic packet itself, or the Bluetooth address
the dump retransmit sends. libubertooth reports the refusal and says which
request it was. A `-n` run that fails part-way also says how far it got, so a
reader can tell one packet sent from ninety-nine:
`error: the board took 7 of 100 packets`.

1 as well when the board refuses the request that ends the dump retransmit: it
is left sending, and no other tool can open it until it is reset or
re-plugged. libubertooth names the refused request on standard error.

1 as well when a board never reports its burst finished, which reads
`error: the board was still transmitting after 100 packets' worth of time`:
the packets may or may not have gone out, and that board is still in the
generic transmit mode.

A generic transmit stopped by Ctrl-C exits 0 and says
`note: the transmit was stopped part way through 100 packets`.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## SEE ALSO

ubertooth-dump(1): the receiving half of the generic radio mode (`-g`)

ubertooth-debug(1): writes the CC2400 registers a generic receive needs
(`-w`)

ubertooth-util(1): sets the power level a transmit uses (`-a`)

ubertooth(7): overview of Project Ubertooth; GENERIC RADIO MODE is the worked
example of a generic packet sent from one board and read on another, and
OPTION VALUES describes how option values are written. The registers both
boards have to agree on are listed there.

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
