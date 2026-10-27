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

An option the tool does not know is refused the same way, as one line on
standard error with no usage text after it: `error: unrecognized option '-Z'`.
An option whose value is missing reads `error: option '-t' requires a value`.

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
<reason>`. A board that never hands the transfer back at all exits 1 too,
reported by the `warning:` line ubertooth(7) describes rather than that error.

A BlueZ adapter that is not there, or that is there but will not open, exits 1
with one `error: ` line on standard error naming the adapter and why, for
example `error: Bluetooth device hci7 not found: No such device`. Nothing is
printed on standard output, so a caller collecting the scan results in a file
does not find the failure among them.

A capture that could not be started at all exits 1 as well, whichever step
failed: the receive transfer libusb would not take, the host thread that
collects the capture, or the request that puts the board in its capture mode.
Standard output holds whatever a `-s` inquiry printed before that and nothing
else; standard error carries the one `error: ` line for the step that failed.
The capture request is named the way the sweep request below is, with the
reason the board gave and the libusb code. The status is 1 in every case,
never the failing call's own return value: a negative libusb code handed to
the exit status is wrapped into a byte, so `libusb -9` reached a script as
247.

A board that refuses the request that puts it in sweep mode exits 1 as well.
Standard output holds whatever a `-s` inquiry printed before that and nothing
else, not even the heading the Ubertooth results would have gone under.
Standard error names the request, why the board refused it and the libusb code,
and adds a note where there is something to try:

    error: could not set sweep mode: the board did not answer in time (libusb -7)
    note: if a mode is running, stop it with ubertooth-util -S; a board that does not answer at all resets itself within a few seconds, so run the command again

That pair is the report. A request the board stalls (`libusb -9`) reads `the
board does not support this command (stalled)` and carries no note, there being
nothing to try. The AFH map this tool prints is only meaningful while the board
sweeps the whole band, so no scan results are printed after that. The sweep is
refused with the capture already running, so the mode is stopped on the way
out, and a board that has stopped answering altogether can add a line of its
own to the report as that is done.

The sweep is stated to the board after the capture has been asked for, because
a mode start resets the board's hop mode and would throw away a sweep stated
before it: without that, a scan listened to one channel and printed a map that
meant nothing. See ubertooth-util(1), CARRY-OVER BETWEEN RUNS. A board that was
already capturing — what a killed `ubertooth-afh` or `ubertooth-follow` leaves
behind — does that reset while it takes the capture request, so the sweep
stated after it survives. A board left in a mode that captures nothing, a
spectrum scan say, resets as it leaves that mode, a moment after the capture
request has been answered, and can still swallow the sweep: stop such a board
with `ubertooth-util -S` before scanning.

A BlueZ inquiry asked for with `-s` runs before the sweep is stated, so its
results are printed even when the sweep is then refused: that half of the scan
needs no Ubertooth, and what it reported is what it saw.

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
