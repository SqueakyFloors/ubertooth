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
 - `-V` :
   print the version: the host tools, then the firmware version and build of
   each attached board; see ubertooth(7), COMMON OPTIONS
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value, a stray word or
   an option given twice is still reported.

## ARGUMENTS

`ubertooth-scan` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

An option the tool does not know is refused the same way, as one line on
standard error with no usage text after it: `error: unrecognized option '-Z'`.
An option whose value is missing reads `error: option '-t' requires a value`.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: maximum access code errors must be 0-5 (got 6)`.

## OUTPUT

Standard output carries what the scan found. Nothing the tool itself prints
there is anything else, so a run collected in a file or read by a script no
longer picks up its headings, its progress or its failures — see the end of
this section for the two lines libbtbb still adds.

Each device the BlueZ inquiry of `-s` found is its address and its name, one to
a line. While the Ubertooth half runs, each packet it decodes is a line of its
own, as `ubertooth-rx` reports one. Then each piconet that half found is its
address, with the NAP unknown, and the channel map the scan built for it:

    systime=1762000000 ch=39 LAP=9e8b33 err=0 clk100ns=1234567 clk1=12 s=-55 n=-90 snr=35
    ??:??:??:9E:8B:33
    AFH map: 0x0123456789abcdef1234

`s=` is the packet's own power and `n=` the board's estimate of the background
on that channel, both in dBm, with `snr=` the difference. A channel the board
never heard both loud and quiet in one receive window leaves it with no
estimate, and the line reads `n=none snr=none`; ubertooth-rx(1) describes both
figures and what the estimate rests on.

`-x` adds the extended report of a device under its line, indented, as
`hcitool info` shows one: its address and name, the LMP version and
manufacturer, its features, its clock offset, and its channel map as an `AFH
Map:` line, the field name `hcitool info` gives it. A device that reports AFH
off has no map to report, and standard error carries a `note:` line saying so.
A survey result's map is the `AFH map:` line above, the spelling it has always
had.

A device that does not answer one of those reads has no line for it in the
report, and the read that failed is named on standard error: the name, the LMP
version, the features and each feature page are left out rather than reported
as unknown or, in the features' case, as zero. Earlier releases left the line
out in silence, and printed zeroed features as the device's own. The device's
own line above the report still reads `[unknown]` when no name comes back,
since that line is a result either way.

Everything else this tool prints goes to standard error: the three headings
that say which half of the scan is running (`HCI scan`, `Ubertooth scan` and
`Scan results:`), the `Requesting information ...` line of `-x`, and every
warning and failure (see EXIT STATUS). Earlier releases mixed those in with
the results, so a scan piped into another program carried them along.

Earlier releases also had lines from libbtbb among the results, because the
library wrote them to standard output: what it says while it recovers a
piconet's clock, such as `12 initial CLK1-27 candidates`, `got CLK1-27` and
`clock offset = 3.`. The library reports those on standard error now, so a
scan's standard output holds the results and nothing else.

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

A capture transfer error is reported the same way. The chip's transfer
controller can report an error on the channel a capture receives into: what
the board was receiving at the time had arrived, so it is kept and marked,
while some of its bytes may be wrong and whatever the fault cut short before
it is lost. The first mark warns `warning: the board had a transfer error
while capturing; what it was receiving was kept, but some of those bytes may
be wrong and the capture may be missing packets around it`, and when the
capture ends it says how many there were: `warning: the capture has 3 transfer
errors where a packet may be wrong or missing`. ubertooth-util(1) `-w` reports
the board's own count of them since power-on, over every capture the board has
run.

Symbols the board wrote over are reported the same way. The board receives
into one block of symbols while its capture loop reads the one before it, and
when a block finishes arriving before the loop has read that one, the new
symbols go over the old ones. The block is gone before anything looked in it
for a packet, so this loss has no packet to point at and nothing in the output
marks where it happened. The first one warns `warning: the board fell behind
while capturing and wrote new symbols over a block it had not read yet;
whatever was in the air then is missing from this capture`, and when the
capture ends it says how many such places there were: `warning: the capture has
3 places where the board wrote new symbols over a block it had not read`. The
count is of those places, not of the blocks lost at them: the board has one
flag for this and no field to send a number in, so one place can stand for
several blocks.

## EXIT STATUS

A scan that ran to its `-t` timeout, or that was interrupted (Ctrl-C), exits 0
with whatever it found, and so does `-h`. A refused command line exits 1 (see
ARGUMENTS).

A run stopped by Ctrl-C before it had opened a board exits 0 as well: see
ubertooth(7), SELECTING A DEVICE.

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

An inquiry asked for with `-s` that the adapter refused exits 1 as well, with
`error: the BlueZ inquiry failed: <reason>` on standard error: half of what
`-s` asked for did not happen. The Ubertooth half still runs — it needs no
adapter — and its results are printed.

A device the inquiry found that will not take the connection `-x` needs, or
will not answer one of the reads that fill the report in — its name, its LMP
version, its features, a features page, its clock offset or its channel map —
is named the same way and that much of its report is missing. The scan carries on and prints the
rest, and exits 1 because part of what `-x` asked for did not happen. Three
failures of that report — reading the local adapter's settings, finding memory
for the request it makes, and reading the clock offset — used to end the whole
run instead, with the board still open in whatever mode it was in.

A piconet the Ubertooth half found is different: a sniffed piconet gives no
NAP, so the address `-x` connects to has its two most significant bytes guessed
as `00:00`. Whether that guess stops the connection to a device whose address
begins otherwise is not known
([issue #699](https://github.com/SqueakyFloors/ubertooth/issues/699)). Either
way, a connection that cannot be made to such an address is a `note:` and the
run still exits 0. Give such a device to `ubertooth-scan -s -x`, or to
`ubertooth-follow -t`, by its whole address.

A device that reports AFH off is not such a failure: it answered, and `note:
AFH disabled` is the answer. A device that reports an AFH mode byte which is
neither off nor on is one, and says what the byte was; it used to be taken for
AFH off.

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
    note: if a mode is running, stop it with ubertooth-util -S; a board that does not answer at all usually resets itself within a few seconds, so run the command again, and unplug it or power-cycle its hub port if it stays silent

That pair is the report. A sweep the board's firmware has not got is
stalled, which reads `error: could not set sweep mode: the board's firmware
does not support it (stalled, libusb -9)`, with a note naming the board's
firmware API version and the one these tools need (see ubertooth(7), FIRMWARE
OF ANOTHER API VERSION). The AFH map this tool prints is only meaningful while
the board sweeps the whole band, so no scan results are printed after that.
The sweep is refused with the capture already running, so the mode is stopped
on the way out, and a board that has stopped answering altogether can add a
line of its own to the report as that is done.

The sweep is stated to the board after the capture has been asked for, because
a mode start resets the board's hop mode and would throw away a sweep stated
before it: without that, a scan listened to one channel and printed a map that
meant nothing. See ubertooth-util(1), CARRY-OVER BETWEEN RUNS. The board keeps
a sweep stated after the capture request whatever it was doing before, so
nothing has to be stopped first; a board left in a mode that captures nothing,
a killed spectrum scan say, used to swallow it and the map was built from one
channel.

A BlueZ inquiry asked for with `-s` runs before the sweep is stated, so its
results are printed even when the sweep is then refused: that half of the scan
needs no Ubertooth, and what it reported is what it saw.

A scan the board would not stop exits 1 as well: the request that takes the
board out of its capture mode was stalled, so the board is left in that mode
and no other tool can open it until it is reset or re-plugged. The results the
scan found are still printed, and libubertooth names the refused request on
standard error.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

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
