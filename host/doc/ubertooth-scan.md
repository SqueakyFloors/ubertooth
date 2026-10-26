# ubertooth-scan(1) -- active (BlueZ) device scan and inquiry supported by Ubertooth

## SYNOPSIS

ubertooth-scan [-s] [-x]

## DESCRIPTION

This tool uses a normal Bluetooth dongle to perform Inquiry Scans and
Extended Inquiry scans of Bluetooth devices. It uses Ubertooth to
discover undiscoverable devices and can use BlueZ to scan for
discoverable devices.

The Ubertooth capture is gated by the board's squelch, which this tool does
not set: it is the default -120 dBm unless `ubertooth-util -z` set another
value while the board was idle, and the firmware resets it on every mode
change, so it never carries over from an earlier capture. See
ubertooth-util(1), SQUELCH.


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
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE

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

## EXIT STATUS

A scan that ran to its `-t` timeout, or that was interrupted (Ctrl-C), exits 0
with whatever it found, and so does `-h`. A refused command line exits 1 (see
ARGUMENTS).

A scan whose USB stream fails part-way also exits 1: the board stopped
answering, or dropped off the bus, so the Ubertooth half of the scan stops
there. The results it did get are still printed, and the failure is reported
on standard error as it happens, as `error: receive transfer failed:
<reason>`.

A board that refuses the request that puts it in sweep mode exits 1 as well.
Standard output stays empty and standard error carries the reason from the
library followed by the tool naming the request:

    error: control message unsupported
    error: could not set sweep mode

The AFH map this tool prints is only meaningful while the board sweeps the
whole band, so neither the BlueZ inquiry nor the Ubertooth scan starts after
that.

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts

ubertooth-rx(1): Discover devices passively with Ubertooth

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-scan(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
