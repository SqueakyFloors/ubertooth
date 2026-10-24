# ubertooth-debug(1) -- read and write CC2400 radio registers

## SYNOPSIS

    ubertooth-debug -r <number>[,<number>[,...]]

## DESCRIPTION

ubertooth-debug(1) is a tool for reading register values from the CC2400 chip
on Ubertooth One. It is primarily designed to assist firmware development but
may be of interest to curious hackers.

## OPTIONS

Options:

 - `-r <reg>[,<reg> [,[...]]]` :
   Read one or more registers from the CC2400 on Ubertooth
 - `-r <start>-<end>` :
   Read a consecutive set of registers from the CC2400 on Ubertooth
 - `-v <0-2>` :
   Sets the level of verbosity, default is 1.
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE

## EXAMPLES

To read the MANOR register, use:

    ubertooth-debug -r 19

Registers can also be specified by name, the following will
also read the MANOR register:

    ubertooth-debug -r %manor

Read a range of registers:

    ubertooth-debug -r 19-22

Dump all registers with:

    ubertooth-debug -r 0x00-0x70

## EXIT STATUS

`ubertooth-debug` exits with 0 when every requested register was read, and 1
otherwise. A failed read prints `error: register 0x<reg> read failed` on
standard error, after libubertooth's own line for a USB error such as a
timeout. A short reply prints the single line `error: register 0x<reg> read
failed: board returned <n> of 2 bytes`. The remaining registers are still read
and decoded.

## ARGUMENTS

`ubertooth-debug` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: verbosity must be 0-2 (got 3)`.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Dominic Spill.

## COPYRIGHT

ubertooth-debug(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
