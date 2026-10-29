# ubertooth-ego(1) -- Yuneec E-Go sniffing

## SYNOPSIS

    ubertooth-ego -f

## DESCRIPTION

The Yuneec E-Go is an electric skateboard controlled by a wireless
handheld remote. This handheld remote uses a custom protocol implemented
in 2.4 GHz RF. ubertooth-ego(1) can sniff this protocol and interfere
with its operation.

Each run captures in the mode it asks for, even on a board left in another
E-GO mode by a run that was killed rather than interrupted: the next run takes
that board over and starts its own mode, on the channel and with the access
code it states. See ubertooth-util(1), CARRY-OVER BETWEEN RUNS.

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
   How much of each packet to keep, in bytes (default: 18). The value must be
   a complete decimal integer from 1 through 48; invalid values fail before
   the Ubertooth is opened. Only the modes that read a packet use it, so `-l`
   with `-i` or `-j` is refused rather than ignored: interference waits for
   the sync word and transmits. The default is sent in every mode, so what a
   later receiving run inherits from the board is the same either way.
 - `-a <access_code>` :
   Access code to use, given as 2, 4, 6 or 8 hex digits (1 to 4 whole
   bytes). A code shorter than four bytes becomes the most significant bytes
   of the radio's 32-bit sync word and the rest are zero, so `-a 1122`
   matches `11220000`, not a shorter pattern. See ACCESS CODE below for what
   a run without `-a` uses.
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value, a stray word or
   an option given twice is still reported.

## ACCESS CODE

`-a` sets the access code for the run that asks for it. A run without `-a`
receives with `630f9ffe`: a mode start puts the code back to that default, so
one run's `-a` does not reach the next.

    ubertooth-ego -r -c 2418 -a 11223344   # receives with 11223344
    ubertooth-ego -r -c 2418               # receives with 630f9ffe

The code is stated just after the request that starts the mode, which is what
keeps a board left in another mode from throwing it away as it leaves it; see
ubertooth-util(1), CARRY-OVER BETWEEN RUNS. Continuous receive tunes once, so
a code that arrives after the capture has started costs its first
millisecond, while `-f`, `-i` and `-j` pick one up on their next hop. All
three modes use the access code.

A program driving the board by hand can still set a code of its own and have
the capture that follows use it, as it can with a channel: a mode started on
an idle board resets nothing, because no earlier mode's code can be there to
reset.

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

Each major mode runs until it is interrupted, so only one of them can run: two
in one command are refused before any device is opened, naming both as they
were spelled, `error: -r cannot be combined with -f; choose one of -f, -r or
-i`. `-i` and `-j` are two spellings of the jamming mode, so they clash too.
The last mode written used to be the one that ran, and the others were dropped
without a word.

`-c` outside continuous receive is refused before any device is opened too,
naming the mode as it was spelled: `error: -c cannot be combined with -f; only
continuous receive (-r) uses a channel`, with no usage text after it (see
DIAGNOSTICS). It used to be accepted and then dropped silently in those modes.

`-l` with interference is refused the same way: `error: -l cannot be combined
with -i; only the receiving modes (-f, -r) read a capture length`. It used to
set a firmware value the mode never reads.

## DIAGNOSTICS

Standard output carries only the captured packets. Errors, warnings and
progress go to standard error, each error as one line starting `error: `, for
example `error: E-GO not supported by this firmware`.

A capture that is losing packets says so. This tool reads its packets one at a
time, and a reply that held part of a record cannot be read as one, so it is
dropped and reported once per run: `warning: a poll reply arrived with 32 of a
packet's 64 bytes and was dropped; the capture is losing packets`. The capture
runs on and ends as it otherwise would, so the warning is the only sign.

A request that sets the capture up and is refused by the board ends the
command there, named in one line, for example
`error: could not set the channel`; libubertooth has already said why. A
refused request is not proof that the board never got it, so the message does
not claim one way or the other.

A command line the tool will not take is refused before any device is opened,
with exit status 1 and one `error: ` line on standard error and nothing after
it: an option the tool does not know (`error: unrecognized option '-Z'`), one
whose value is missing (`error: option '-c' requires a value`), a bad option
value, a stray word, `-c` outside continuous receive, `-l` with interference,
and a command line that said something but has nothing to do. The line
describes the problem, so the usage text is left to `-h`. A bare
`ubertooth-ego`, with no arguments at all, is the one refusal the usage text
does follow, on standard error with the `error: ` line.

`-h` is what the user asked for, so the usage text goes to standard output and
nothing goes to standard error.

## GAPS IN A CAPTURE

A capture that is losing packets says so. The board holds captured packets in a
queue until this tool reads them, and a queue with no room left loses the next
packet. The first time it happens the capture warns `warning: the board's
packet queue filled up and it dropped at least one packet; this capture has a
gap`, and when the capture ends it says how many such gaps there were:
`warning: the capture has 3 gaps where the board's queue filled up and it
dropped packets`. The count is of gaps, not of lost packets: one full queue can
swallow several packets, and the board sends no number. Nothing says which
packets are missing, so a capture with gaps is complete only between them. The
board reports a gap on the first packet that gets through after it, so a
capture that ends in the moment after a gap does not report that last one.

## EXIT STATUS

0 when the command did what was asked, including `-h`, and after a capture is
interrupted (Ctrl-C). 1 for a command line the tool will not take or with
nothing to do, when the selected Ubertooth is not found or cannot be opened,
when the board refuses one of the requests that set the capture up, when the
firmware does not support the requested mode, and when the capture stops short
because a poll failed.

1 as well when the board refuses the request that ends the capture: it is left
in the E-GO mode, and no other tool can open it until it is reset or
re-plugged. libubertooth names the refused request on standard error.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-ego(1) is Copyright (c) 2015 Mike Ryan. This tool is released
under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
