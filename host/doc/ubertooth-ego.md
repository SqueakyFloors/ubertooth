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
   Interfere with a connection (alias of `-i`)

Options:

 - `-c <2402-2480>` :
   Channel in MHz to use for continuous receive (suggested: 2408, 2418,
   2423, 2469)
 - `-l <1-48>` :
   Packet length to use. The value must be a complete decimal integer from 1
   through 48; invalid values fail before the Ubertooth is opened.
 - `-a <access_code>` :
   Access code to use, given as 2, 4, 6 or 8 hex digits (1 to 4 whole
   bytes, default: 630f9ffe)
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE

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

## SEE ALSO

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-ego(1) is Copyright (c) 2015 Mike Ryan. This tool is released
under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
