# ubertooth-dump(1) -- output a continuous stream of received bits

## SYNOPSIS

    ubertooth-dump [-b] [-c | -l | -g] [-d <filename.bin>|-] [-U <index|serial>]
    ubertooth-dump [-U <index|serial>] -V
    ubertooth-dump -h

## DESCRIPTION

This tool dumps the raw bitstream received from the CC2400 radio to
STDOUT or a file (if specified with `-d`). It can configure the radio to
demodulate Classic Bluetooth with `-c` (default), or Bluetooth Low
Energy (BLE) with `-l`. Optionally it can decode the binary data and
print ASCII 0 and 1 using `-b`.

`-g` runs the board's generic receive instead. That mode programs no
demodulator of its own: it receives with whatever CC2400 configuration is
already in the radio, which `ubertooth-debug -w` is how you put there. It is
what the board has for a 2.4 GHz protocol that is not Bluetooth, and
ubertooth(7), GENERIC RADIO MODE is the worked example.

The dump is one record per block of symbols the board sends. Without `-b` a
record is 68 bytes: the host time the block arrived, as Unix epoch seconds in 4
bytes big-endian, then the whole 64-byte USB receive packet, 50 bytes of which
are the symbols packed 8 per byte. With `-b` a record is one 401-byte line: the
block's 400 symbols as ASCII `0` and `1`, then a newline. The records are back
to back with nothing between them, so a reader counts from the start of the
dump. A keep-alive, which a quiet capture sends and which carries no symbols,
writes no record either way.

The capture is gated by the board's squelch, which this tool does not set: it
is the default -120 dBm unless `ubertooth-util -z` set another value while the
board was idle, and the firmware resets it on every mode change, so it never
carries over from an earlier capture. See ubertooth-util(1), SQUELCH.

## OPTIONS

 - `-b` :
   Print ASCII 0 and 1 instead of raw data
 - `-c` :
   Classic Bluetooth modulation (default)
 - `-l` :
   Bluetooth Low Energy (BLE) modulation
 - `-g` :
   Generic receive: demodulate with the CC2400 configuration already in the
   radio rather than programming one. Cannot be given with `-c` or `-l`
 - `-d <filename.bin>` :
   Dump to file instead of stdout. A lone `-` is standard output, where the
   stream goes anyway without `-d`
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

## ARGUMENTS

`ubertooth-dump` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

The board demodulates one modulation at a time, so `-c` and `-l` in one
command are refused before any device is opened:
`error: -l cannot be combined with -c; a capture uses one modulation`. The
second one used to be the one used, and the other was dropped without a word.

`-g` and a modulation ask for two different things, so they are refused
together as well: `error: -g cannot be combined with -c; the generic receive
uses the modulation already in the radio`. `-c` and `-l` name a demodulator
for the board to program, and `-g` says to use the one already configured, so
a dump that took both would overwrite part of the configuration it was asked
to read with.

A refused command line creates and truncates nothing: `-d` names its file
while the command line is read, and it is opened only once every check has
passed. The board is opened and its firmware is checked before that, so a run
refused for either reason leaves no empty file behind either.

## DIAGNOSTICS

Without `-d`, standard output carries the symbol stream and nothing else, and
so does `-d -`. Errors, warnings and the per-block timestamp go to
standard error, each error as one line starting `error: `, for example
`error: /no/such/dir/capture.bin: No such file or directory`. That line comes
after the board has been selected and its firmware accepted, because that is
when the file is opened.

A terminal is refused either way, before any device is opened:
`error: stdout is a tty, refusing to write binary dump data to it`, exit 1.
A dump is written for a file or another program, not for a screen. `-b` is
refused too although it writes text: that line is the one every tool shares for
a capture stream it will not put on a terminal. Redirect or pipe standard
output instead, or give `-d` a file.

A command line the tool will not take is reported as that one `error: ` line
on standard error, with no usage text after it: the line describes the
problem, and `-h` prints the usage text for a reader who wants it. An option
the tool does not know reads `error: unrecognized option '-Z'`; one whose
value is missing reads `error: option '-d' requires a value`.

`-h` is what the user asked for, so the usage text goes to standard output and
nothing goes to standard error.

## GAPS IN A CAPTURE

A dump that is losing records says so. The board holds captured records in a
queue until this tool reads them, and a queue with no room left loses the next
one. The first time it happens the dump warns `warning: the board's packet
queue filled up and it dropped at least one packet; this capture has a gap`,
and when the dump ends it says how many such gaps there were: `warning: the
capture has 3 gaps where the board's queue filled up and it dropped packets`.
The count is of gaps, not of lost records: one full queue can swallow several,
and the board sends no number. Nothing in the dump marks where a gap is, so a
dump with gaps is continuous only between them.

A capture transfer error is reported the same way. The chip's transfer
controller can report an error on the channel the board receives into: the
record it was filling had arrived, so it is kept and marked, while some of its
bytes may be wrong and whatever the fault cut short before it is lost. The
first mark warns `warning: the board had a transfer error while capturing;
what it was receiving was kept, but some of those bytes may be wrong and the
capture may be missing packets around it`, and when the dump ends it says how
many there were: `warning: the capture has 3 transfer errors where a packet
may be wrong or missing`. ubertooth-util(1) `-w` reports the board's own count
of them since power-on, over every capture the board has run.

Symbols the board wrote over are reported the same way. The board receives
into one block of symbols while its capture loop reads the one before it, and
when a block finishes arriving before the loop has read that one, the new
symbols go over the old ones. The block is gone before anything looked in it
for a packet, so this loss has no packet to point at and nothing in the output
marks where it happened. The first one warns `warning: the board fell behind
while capturing and wrote new symbols over a block it had not read yet;
whatever was in the air then is missing from this capture`, and when the
dump ends it says how many such places there were: `warning: the capture has
3 places where the board wrote new symbols over a block it had not read`. The
count is of those places, not of the blocks lost at them: the board has one
flag for this and no field to send a number in, so one place can stand for
several blocks.

## EXIT STATUS

0 when the command did what was asked, including `-h`, and after the dump is
interrupted (Ctrl-C). 1 for a command line the tool will not take, for a `-d`
file that cannot be opened, and when the selected Ubertooth is not found or
cannot be opened.

A run stopped by Ctrl-C before it had opened a board exits 0 as well: see
ubertooth(7), SELECTING A DEVICE.

Also 1 when the dump's USB stream fails part-way: the board stopped answering
or dropped off the bus, so the dump stops there and holds only what arrived
before it. The failure is reported on standard error as it happens. A board
that never hands the transfer back at all exits 1 too, reported by the
`warning:` line ubertooth(7) describes rather than an error.

Also 1 when the board refuses the request that ends the dump: it is left in
the capture mode, and no other tool can open it until it is reset or
re-plugged. What was dumped stays written out, and libubertooth names the
refused request on standard error.

Also 1 when the board refuses the modulation `-c` or `-l` asks for, with
libubertooth's own line for the refused request on standard error and nothing
of the dump on standard output: a dump that went ahead would hold whatever
modulation the board had been left in, and nothing in its records says which
that was.

The dump states it on either side of the request that starts its capture, for
the reason ubertooth-util(1), CARRY-OVER BETWEEN RUNS gives; that page also
says what `-l` can hold at the very start of a dump on a board another run
left in a mode. Nothing is asked of the radio until then, which is after a
`-d` file has been opened, so a refusal leaves that file there and empty, as a
board that will not start the capture does, and a `-d` the tool cannot open
leaves the board as it was found.

A `-d` file is opened once the board has been opened and its firmware
accepted, not before, so a run refused for either reason leaves no empty file
behind. A terminal on standard output is still refused
while the command line is read, with or without `-d -`, because that is a
problem with the command rather than with the board.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## THE GENERIC RECEIVE

With `-g` the board does one of two things, and which one it does comes from
GRMDM's packet-mode bit, so the configuration decides it:

 - with packet mode set, the board waits for the sync word in SYNCH and SYNCL
   and sends one record per packet: the four sync-word bytes, then 32 bytes
   read out of the radio's FIFO.
 - with packet mode clear, the board streams demodulated symbols as the
   Bluetooth captures do, so `-b` prints them as `0` and `1`.

Packet mode leaves the framing to the radio, so it produces records only when
GRMDM's preamble length, how much of the sync word it matches and its CRC
choice all match what is on the air. Getting that right for a protocol is part
of the setup and nothing reports a mismatch: a wrong choice gives a capture
with no records in it. The symbol stream asks less of the configuration, which
is why the worked example in ubertooth(7) uses it.

Either way the records are the 68-byte and 401-byte shapes described above:
the board sends what it received, and this tool writes it out without looking
in it. Nothing decodes a generic capture, because nothing here knows what
protocol it is.

The configuration has to be in the radio before the capture starts, and it
survives only while the board is idle: a mode ending puts the radio back
through its own reset. So write the registers and start the dump as two
commands with nothing in between, as ubertooth(7), GENERIC RADIO MODE shows.

## SEE ALSO

ubertooth-debug(1): reads and writes the CC2400 registers a generic capture
needs (`-w`)

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts

ubertooth(7): overview of Project Ubertooth; GENERIC RADIO MODE is the worked
example of `-g`, and OPTION VALUES describes how option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-dump(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
