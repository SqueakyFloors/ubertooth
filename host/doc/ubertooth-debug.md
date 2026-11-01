# ubertooth-debug(1) -- read and write CC2400 radio registers

## SYNOPSIS

    ubertooth-debug -r <number>[,<number>[,...]]
    ubertooth-debug -w <reg>=<value>[,<reg>=<value>[,...]]

## DESCRIPTION

ubertooth-debug(1) is a tool for reading register values from the CC2400 chip
on Ubertooth One. It is primarily designed to assist firmware development but
may be of interest to curious hackers.

`-w` is experimental, as the generic radio mode it exists for is: see
ubertooth(7), GENERIC RADIO MODE for what that mode does and what it asks of
a configuration. Expect the option and its output to change.

`-w` writes registers as well as `-r` reading them. Writing the radio's
registers by hand is how a generic capture is set up: `ubertooth-dump -g`
demodulates with whatever configuration is in the CC2400, so `-w` puts the
channel, data rate, sync word and framing there first. ubertooth(7), GENERIC
RADIO MODE is the worked example.

Nothing here knows which values a register accepts: these are the radio's own
registers, and the value is written as given. A configuration that does not
make sense gives a board that receives nothing, not an error.

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
 - `-w <reg>=<value>[,<reg>=<value>[,...]]` :
   Write one or more registers of the CC2400 on Ubertooth. Each register is
   an address `0x00`-`0xff` or a `%name`, as `-r` accepts, and each value is
   `0x0000`-`0xffff`. Hex needs the `0x`; every other number is decimal, so
   `010` is ten and not eight (experimental)
 - one `-w` takes the whole list, as one `-r` does, so a second `-w` is
   refused with `error: option '-w' given more than once`
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

Write a register, then read it back in the same command: the writes happen
first, so this reports what the board took.

    ubertooth-debug -w %grmdm=0x0f61 -r %grmdm

Several registers in one command, mixing names and addresses:

    ubertooth-debug -w %mdmctrl=0x0029,%grmdm=0x0f61,0x0f=0x0997

## EXIT STATUS

`ubertooth-debug` exits with 0 when every requested register was read, and 1
otherwise. A failed read prints `error: register 0x<reg> read failed` on
standard error, after libubertooth's own line for a USB error such as a
timeout. A short reply prints the single line `error: register 0x<reg> read
failed: board returned <n> of 2 bytes`. The remaining registers are still read
and decoded.

A failed write prints `error: register 0x<reg> write failed`, again after
libubertooth's own line, and the remaining registers are still written: one
register the board refused does not hide the rest. A command that asked for
both writes and reads does both, and exits 1 if either had a failure.

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

`-w` takes a list of `<reg>=<value>` pairs. A number with a leading `0x` is
hex and every other number is decimal, so `-w 010=1` writes register ten.
(`-r` reads a leading zero as octal, which it has always done.) The register
is refused the way `-r`'s is, and a value outside 16 bits the same way:

    $ ubertooth-debug -w 0x100=1
    error: register address must be 0x00-0xff (got 0x100 in 0x100=1)
    $ ubertooth-debug -w 0x20=0x10000
    error: register value must be 0x0000-0xffff (got 0x10000 in 0x20=0x10000)

An item that is not a pair at all, a missing value, an empty item between two
commas and a trailing comma are all refused in one line as
`error: register write must be <reg>=<value> (got ...)`, and a `%name` the
table does not have as
`error: no CC2400 register is called %nosuch (in %nosuch=1)`. Nothing is
written when any item is refused, so a configuration cannot reach the radio
half-applied.

Either `-r` or `-w` is needed: a command line with neither is refused with
`error: nothing to do: give -r to read registers, or -w to write them`,
on standard error and with exit status 1. A bare `ubertooth-debug`, with no
arguments at all, prints the usage text after that line; every other refused
command line is the `error: ` line alone, and `-h` prints the usage text on
standard output.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; GENERIC RADIO MODE describes how
`-w` and `ubertooth-dump -g` are used together, and OPTION VALUES describes
how option values are written

ubertooth-dump(1): the generic receive, which reads with the configuration
`-w` leaves in the radio

ubertooth-tx(1): the generic transmit (`-g`), which puts a packet on the air
for such a capture to read

## AUTHOR

This manual page was written by Dominic Spill.

## COPYRIGHT

ubertooth-debug(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
