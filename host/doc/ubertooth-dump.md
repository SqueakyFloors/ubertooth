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
   Dump to file instead of stdout
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE

## ARGUMENTS

`ubertooth-dump` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

A refused command line creates and truncates nothing: `-d` names its file while
the command line is read, and it is opened only once every check has passed.

## DIAGNOSTICS

Without `-d`, standard output carries the binary symbol stream and nothing
else. Errors, warnings and the per-block timestamp go to standard error, each
error as one line starting `error: `, for example
`error: /no/such/dir/capture.bin: No such file or directory`.

A command line the tool will not take is reported as that one `error: ` line
on standard error, with no usage text after it: the line describes the
problem, and `-h` prints the usage text for a reader who wants it. An option
the tool does not know reads `error: unrecognized option '-Z'`; one whose
value is missing reads `error: option '-d' requires a value`.

`-h` is what the user asked for, so the usage text goes to standard output and
nothing goes to standard error.

## EXIT STATUS

0 when the command did what was asked, including `-h`, and after the dump is
interrupted (Ctrl-C). 1 for a command line the tool will not take, for a `-d`
file that cannot be opened, and when the selected Ubertooth is not found or
cannot be opened.

Also 1 when the dump's USB stream fails part-way: the board stopped answering
or dropped off the bus, so the dump stops there and holds only what arrived
before it. The failure is reported on standard error as it happens.

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
