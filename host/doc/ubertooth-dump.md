# ubertooth-dump(1) -- output a continuous stream of received bits

## SYNOPSIS

    ubertooth-dump [-b] [-c | -l] [-d <filename.bin>]

## DESCRIPTION

This tool dumps the raw bitstream received from the CC2400 radio to
STDOUT or a file (if specified with `-d`). It can configure the radio to
demodulate Classic Bluetooth with `-c` (default), or Bluetooth Low
Energy (BLE) with `-l`. Optionally it can decode the binary data and
print ASCII 0 and 1 using `-b`.

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

## SEE ALSO

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-dump(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
