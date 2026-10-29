# ubertooth-dump(1) -- output a continuous stream of received bits

## SYNOPSIS

    ubertooth-dump [-b] [-c | -l] [-d <filename.bin>]

## DESCRIPTION

This tool dumps the raw bitstream received from the CC2400 radio to
STDOUT or a file (if specified with `-d`). It can configure the radio to
demodulate Classic Bluetooth with `-c` (default), or Bluetooth Low
Energy (BLE) with `-l`. Optionally it can decode the binary data and
print ASCII 0 and 1 using `-b`.

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

A refused command line creates and truncates nothing: `-d` names its file
while the command line is read, and it is opened only once every check has
passed. The board is opened and its firmware is checked before that, so a run
refused for either reason leaves no empty file behind either.

## DIAGNOSTICS

Without `-d`, standard output carries the binary symbol stream and nothing
else, and so does `-d -`. Errors, warnings and the per-block timestamp go to
standard error, each error as one line starting `error: `, for example
`error: /no/such/dir/capture.bin: No such file or directory`. That line comes
after the board has been selected and its firmware accepted, because that is
when the file is opened.

A terminal is refused either way, before any device is opened:
`error: stdout is a tty, refusing to write binary dump data to it`, exit 1.
The stream is binary, so a terminal fills with control characters and the
dump is lost. Redirect or pipe standard output instead.

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

A `-d` file is opened once the board has been opened and its firmware
accepted, not before, so a run refused for either reason leaves no empty file
behind. A terminal on standard output is still refused while the command line
is read, with or without `-d -`, because that is a problem with the command
rather than with the board.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-dump(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
