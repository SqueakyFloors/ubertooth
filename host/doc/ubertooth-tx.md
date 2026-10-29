# ubertooth-tx(1) -- retransmit symbols dumped with ubertooth-dump

## SYNOPSIS

    ubertooth-tx -l LAP -u UAP [-t SECONDS] [-U <index|serial>] [-V]

## DESCRIPTION

Intended to retransmit symbols captured with ubertooth-dump toward a target
piconet identified by LAP/UAP.

**Status: non-functional.** The tool transmits nothing today, and says so in
its own usage output ("This tool currently does nothing"). It is kept as the
entry point for that work; do not rely on it to transmit.

## OPTIONS

 - `-l <LAP>` :
   target LAP (3 bytes / 6 hex digits); required.
 - `-u <UAP>` :
   target UAP (1 byte / 2 hex digits); required.
 - `-t <seconds>` :
   timeout; 0 means no timeout (default: 0).
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-V` :
   print the host tools version, as `Host tools version: libubertooth
   <version> (<revision>), libbtbb <version> (<revision>)`, followed by
   `libbtbb loaded from: <path>` naming the file this run reads libbtbb from.
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value, a stray word or
   an option given twice is still reported.

## TRANSMIT POWER

Transmitting uses the CC2400 power level set with `ubertooth-util -a` and leaves
it unchanged. A board that has not been given a level transmits at 7 (0 dBm),
the CC2400's reset value. These levels are at the CC2400 radio; the Ubertooth
One's RF amplifier changes the actual output after that.

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
`error: LAP must be 6 hex digits (got 4466)`.

Both halves of the target address are required, and a command line missing
either is refused there too, before any device is opened, so the message never
depends on which boards are attached:
`error: -l <LAP> and -u <UAP> are both required`.

## DIAGNOSTICS

What `ubertooth-tx` does happens over the air, so it writes no result:
everything it prints, including the `Syncword: 0x...` line, goes to standard
error, and standard output stays empty. Each error is one line starting
`error: `.

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

0 when the command did what was asked, including `-h`, and after the run is
interrupted (Ctrl-C) or its `-t` timeout passes. 1 for a command line the tool
will not take and when the selected Ubertooth is not found or cannot be
opened.

1 as well when the board refuses the Bluetooth address the transmit needs, or
takes only part of it: nothing is transmitted, since the address decides what
the board sends. libubertooth reports the refusal.

## SEE ALSO

ubertooth-dump(1)

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
