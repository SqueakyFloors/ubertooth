# ubertooth-scan(1) -- active (BlueZ) device scan and inquiry supported by Ubertooth

## SYNOPSIS

ubertooth-scan [-s] [-x]

## DESCRIPTION

This tool uses a normal Bluetooth dongle to perform Inquiry Scans and
Extended Inquiry scans of Bluetooth devices. It uses Ubertooth to
discover undiscoverable devices and can use BlueZ to scan for
discoverable devices.


## EXAMPLES

Use Ubertooth to discover devices and perform Inquiry Scan with BlueZ:

    ubertooth-scan

Use BlueZ and Ubertooth to discover devices and perform Inquiry Scan and
Extended Inquiry Scan:

    ubertooth-scan -s -x

## OPTIONS

 - `-s` :
   hci Scan - use BlueZ to scan for discoverable devices
 - `-x` :
   eXtended scan - retrieve additional information about target devices
 - `-t <seconds>` :
   scan Time - length of time to sniff packets. [Default: 20s]
 - `-e <0-5>` :
    Maximum Access Code Errors (default: 2)
 - `-b hciN` :
    Bluetooth device (default: hci0)
 - `-U<0-7>` :
    which Ubertooth device to use

## ARGUMENTS

`ubertooth-scan` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: maximum access code errors must be 0-5 (got 6)`.

## SEE ALSO

ubertooth-rx(1): Discover devices passively with Ubertooth

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-scan(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
