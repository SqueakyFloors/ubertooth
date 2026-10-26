# ubertooth-ego(1) -- Yuneec E-Go sniffing

## SYNOPSIS

    ubertooth-ego -f

## DESCRIPTION

The Yuneec E-Go is an electric skateboard controlled by a wireless
handheld remote. This handheld remote uses a custom protocol implemented
in 2.4 GHz RF. ubertooth-ego(1) can sniff this protocol and interfere
with its operation.

## EXAMPLES

Sniff the connection between an E-Go and its remote:

    ubertooth-ego -f

Receive all E-Go packets on 2408 MHz:

    ubertooth-ego -r -c 2408

Interfere with a connection:

    ubertooth-ego -i

## OPTIONS

Major modes:

 - `-f` :
   Sniff a connection by hopping along channels
 - `-r` :
   Continuously receive packets on a single channel (see `-c`)
 - `-i` :
   Interfere with a connection
 - `-j` :
   Original "jam" spelling of `-i`; does exactly what `-i` does

Options:

 - `-c <2402-2480>` :
   Channel in MHz to use for continuous receive (suggested: 2408, 2418,
   2423, 2469; default: 2418). Only `-r` uses it: `-f`, `-i` and `-j` hop
   across the E-Go's own channel list, so `-c` with one of those is refused
   rather than ignored. The default is sent like any other channel, so
   `ubertooth-ego -r` on its own receives on 2418 MHz rather than on whatever
   channel the board was left on.
 - `-l <1-48>` :
   Packet length to use. The value must be a complete decimal integer from 1
   through 48; invalid values fail before the Ubertooth is opened.
 - `-a <access_code>` :
   Access code to use, given as 2, 4, 6 or 8 hex digits (1 to 4 whole
   bytes). A code shorter than four bytes becomes the most significant bytes
   of the radio's 32-bit sync word and the rest are zero, so `-a 1122`
   matches `11220000`, not a shorter pattern. See ACCESS CODE below for what
   a run without `-a` uses.
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE

## ACCESS CODE

`-a` sets the access code on the board, which keeps it until the board is
reset or reflashed. A run without `-a` sends no access code at all, so it uses
whichever one the board was last given, not a default of its own:

    ubertooth-ego -r -c 2418 -a 11223344   # sets the board's access code
    ubertooth-ego -r -c 2418               # still receives with 11223344

`630f9ffe` is the firmware's value after a reset, so it is what a run without
`-a` uses only on a board that has not been given another. Pass `-a` when a
run has to be reproducible, or reset the board (`ubertooth-util -r`) first.
All three modes use the access code.

## TRANSMIT POWER

Interference (`-i`, `-j`) transmits at the CC2400 power level set with
`ubertooth-util -a` and leaves it unchanged. A board that has not been given a
level jams at 7 (0 dBm, the maximum), the CC2400's reset value and the level
jamming always used before. Sniffing does not transmit and does not change the
level. These levels are at the CC2400 radio; the Ubertooth One's RF amplifier
changes the actual output after that.

## ARGUMENTS

`ubertooth-ego` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: channel must be 2402-2480 MHz (got 2481)`.

A major mode is the work, so a command line without `-f`, `-r` or `-i` is
refused before any device is opened: `error: nothing to do: choose a major
mode (-f, -r or -i)`. It used to open the selected board, check its firmware,
do nothing and exit 0.

`-c` outside continuous receive is refused before any device is opened too,
naming the mode as it was spelled: `error: -c cannot be combined with -f; only
continuous receive (-r) uses a channel`, with no usage text after it (see
DIAGNOSTICS). It used to be accepted and then dropped silently in those modes.

## DIAGNOSTICS

Standard output carries only the captured packets. Errors, warnings and
progress go to standard error, each error as one line starting `error: `, for
example `error: E-GO not supported by this firmware`.

A request that sets the capture up and is refused by the board ends the
command there, named in one line, for example
`error: could not set the channel`; libubertooth has already said why. A
refused request is not proof that the board never got it, so the message does
not claim one way or the other.

A command line the tool will not take is refused before any device is opened,
with exit status 1 and one `error: ` line on standard error and nothing after
it: an option the tool does not know (`error: unrecognized option '-Z'`), one
whose value is missing (`error: option '-c' requires a value`), a bad option
value, a stray word, `-c` outside continuous receive, and a command line that
said something but has nothing to do. The line describes the problem, so the
usage text is left to `-h`. A bare `ubertooth-ego`, with no arguments at all,
is the one refusal the usage text does follow, on standard error with the
`error: ` line.

`-h` is what the user asked for, so the usage text goes to standard output and
nothing goes to standard error.

## EXIT STATUS

0 when the command did what was asked, including `-h`, and after a capture is
interrupted (Ctrl-C). 1 for a command line the tool will not take or with
nothing to do, when the selected Ubertooth is not found or cannot be opened,
when the board refuses one of the requests that set the capture up, when the
firmware does not support the requested mode, and when the capture stops short
because a poll failed.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-ego(1) is Copyright (c) 2015 Mike Ryan. This tool is released
under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
