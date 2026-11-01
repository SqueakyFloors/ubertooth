# ubertooth-debug(1) -- read CC2400 radio registers

## SYNOPSIS

    ubertooth-debug -r <number>[,<number>[,...]]

## DESCRIPTION

ubertooth-debug(1) is a tool for reading register values from the CC2400 chip
on Ubertooth One. It is primarily designed to assist firmware development but
may be of interest to curious hackers.

It reads and does not write: there is no option for setting a register, and no
other tool has one either.

Register reads are debugging work, so this tool drives whatever firmware
revision a board is carrying and makes no version check. It does check which
image the board runs: `btbr`, the other image this tree builds, answers no
register read, so a board carrying it is refused by name before any register
is read, with the line that flashes `bluetooth_rxtx`; see ubertooth(7),
FIRMWARE OF ANOTHER API VERSION.

## OPTIONS

Options:

 - `-r <reg>[,<reg> [,[...]]]` :
   Read one or more registers from the CC2400 on Ubertooth. Each item is an
   address `0x00`-`0xff` or a `%name`, in any mixture and in any position
 - `-r <start>-<end>` :
   Read a consecutive set of registers from the CC2400 on Ubertooth. Either
   end may be a `%name` (`-r %main-%fsdiv`)
 - one `-r` takes the whole list, so give it once: a second `-r` is refused
   with `error: option '-r' given more than once`, because it used to replace
   the first one's registers without a word. Write them together instead,
   `-r %main,0x02-0x05` (ubertooth(7), OPTION VALUES)
 - `-v <0-2>` :
   Sets the level of verbosity, default is 1.
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

## EXAMPLES

To read the MANOR register, use:

    ubertooth-debug -r 19

Registers can also be specified by name, the following will
also read the MANOR register:

    ubertooth-debug -r %manor

Names and addresses mix freely, in a list or as the ends of a range:

    ubertooth-debug -r %main,%fsdiv,0x1b
    ubertooth-debug -r %main-%fsdiv

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

A board running the other firmware image exits 1 as well, with no register
read and the two lines that name the image and flash the right one:

    error: the Ubertooth is running btbr; this tool needs bluetooth_rxtx
    note: flash it with ubertooth-dfu -U aa11 -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## ARGUMENTS

`ubertooth-debug` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

An option the tool does not know is refused the same way, as one line on
standard error with no usage text after it: `error: unrecognized option '-Z'`.
An option whose value is missing reads `error: option '-r' requires a value`.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: verbosity must be 0-2 (got 3)`.

`-r` takes register addresses `0x00` to `0xff`, both ends included: `0x00` is
MAIN and `0xff` is the highest address the tool accepts. An address outside
that is refused, quoted as it was written, and with the whole value when it is
one item of several:

    $ ubertooth-debug -r 300
    error: register address must be 0x00-0xff (got 300)
    $ ubertooth-debug -r 0x02,300
    error: register address must be 0x00-0xff (got 300 in 0x02,300)

A value that cannot be read as a list at all gets one line in the same shape,
with the reason: `error: register list must be names or addresses, single or as
a range: unknown name (got %nosuchregister)`. The other reasons are
`empty value`, `range with no start`, `not an octal digit` and
`not a name, address or range`.

`-r` is required: without it the command is refused with
`error: -r <reg>[,<reg>[,...]] is required: at least one register to read`,
on standard error and with exit status 1. A bare `ubertooth-debug`, with no
arguments at all, prints the usage text after that line; every other refused
command line is the `error: ` line alone, and `-h` prints the usage text on
standard output.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Dominic Spill.

## COPYRIGHT

ubertooth-debug(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
