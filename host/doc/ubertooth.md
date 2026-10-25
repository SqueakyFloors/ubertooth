# ubertooth(7) -- Project Ubertooth

## OVERVIEW

Ubertooth is a platform for Bluetooth experimentation. It is able to
sniff Bluetooth Smart (BLE), discover undiscoverable classic Bluetooth
devices, and perform rudimentary sniffing of classic Bluetooth devices.

Ubertooth is **not** a Bluetooth dongle and it will not show up as one
when running `hciconfig`.

## COMMAND LINE TOOLS

The following command line tools included in this package are the
primary means of using Ubertooth. Refer to their manual pages for
example usage and comprehensive documentation.

Useful commands:

 - ubertooth-btle(1) : BLE sniffing and other fun
 - ubertooth-rx(1) : Classic Bluetooth device discovery and rudimentary sniffing
 - ubertooth-scan(1) : Active scanning of undiscoverable devices
 - ubertooth-afh(1) : Detecting the AFH map of a Classic Bluetooth piconet
 - ubertooth-ego(1) : Yuneec E-Go electric skateboard sniffing
 - ubertooth-follow(1) : Active (BlueZ) clock discovery and piconet following

Utility commands:

 - ubertooth-dfu(1) : Firmware update tool
 - ubertooth-dump(1) : Dumping raw RF symbols to disk
 - ubertooth-tx(1) : Retransmitting symbols dumped with ubertooth-dump
 - ubertooth-ducky(1) : Driving an Uberducky as a BLE USB Rubber Ducky
 - ubertooth-util(1) : "Everything else"

Less useful commands:

 - ubertooth-debug(1) : Peeking and poking registers on the CC2400
 - ubertooth-specan(1) : Raw RSSI values used by graphical specan

## OPTION VALUES

These pages write an option's value as a separate word: `ubertooth-util -c
2440`, `ubertooth-btle -a 8e89bed6`, `ubertooth-rx -l 446688 -u 22`. This
follows a standard, the POSIX Utility Syntax Guidelines (guideline 6: an option
and its value are separate arguments). The value placeholders are written the
same way (`-c <2402-2480>`). The older attached form is still accepted for
backward compatibility with existing scripts, so `-c 2440` and `-c2440` are the
same. Where the value is optional, the next word
is taken as the value only when it is a valid value, so an option never
swallows the option after it: `-c -b` reads the channel and then the board ID,
while `-z -90` sets the squelch to -90 dBm. A long option takes `--name value` or
`--name=value`, never attached without the `=` (no long option takes a value
today; this is for consistency with `getopt_long`). A lone `-` as the value of
a file option means standard input or standard output, so `ubertooth-btle -q -`
writes the capture to stdout.

## SELECTING A DEVICE

With a single Ubertooth attached, the tools find it automatically. With more
than one attached, every tool takes `-U <index|serial>` to choose one: an
index is a position in the list of attached Ubertooths (`-U 0` is the first),
and a serial-number string selects that board whatever the enumeration order.
An index that is not attached is refused (`error: no Ubertooth at index 2 (2
attached)`) and the tool exits non-zero. Indexes are positions in the list of
boards attached when the command runs, so they shift when a board is added or
removed; to keep targeting the same board, select it by serial.

A serial may be shortened to its first few characters, in either case, so
`-U 1cc0` and `-U 1CC0` select the same board. If those characters start more
than one attached board's serial, the tool lists the matching serials and
selects neither. An all-digits string is read as an index, so include a
non-digit (`-U 05f` rather than `-U 05`). See ubertooth-util(1), SELECTING A
DEVICE, for the full description and for listing the attached devices.

## WHEN A BOARD CANNOT BE USED

A board can be attached and still be unavailable, because only one program at
a time can use it and because a mode keeps running on the board until it is
stopped. Both cases are reported in words, with the libusb code in parentheses
for a bug report.

The board opened, and then something else turned out to be holding it:

    error: Ubertooth 05f00014e380df95f4436562c32000f5 is in use by another program (libusb -3)

That is another tool from this package, a graphical specan, or anything else
holding its USB interface; on Linux it may be a kernel driver. Stop that
program and run the command again. Which libusb code says so depends on the
system, which is why the message does not depend on it. Nothing is sent to the
board on this path, so the command does not disturb whatever is using it.

With one board attached and no `-U`, there is no name to use, and a `-U
<index>` run has only the index:

    error: the Ubertooth is in use by another program (libusb -6)
    error: Ubertooth at index 1 is in use by another program (libusb -3)

The board took the request and sent nothing back:

    error: the Ubertooth did not answer in time (libusb -7)

A board that has stopped answering control requests does this: it happens
after some captures end, and a board left in a mode that keeps polling can do
it too. Try `ubertooth-util [-U <selector>] -S` to stop a mode and
`ubertooth-util -r` to reset the board; a board that answers neither has to be
unplugged and plugged in again, or its hub port power-cycled. The line says
only what happened, because what it means depends on what the command was
doing: mid-flash, for instance, it is a bootloader that lost its place, and no
mode is involved.

A serial-number selector names the reason, because a board it could not read
may be the board asked for:

    no Ubertooth found with serial matching '1cc'
    1 attached Ubertooth device did not answer the serial read
    note: a board that does not answer may be in use by another program, or busy in a mode; stop a mode with ubertooth-util -U <index> -S

The remedy is by index there because a board whose serial could not be read
cannot be selected by serial.

A board that cannot be *opened* at all is a different problem, usually file
permissions: on Linux, install the udev rules shipped with this package (see
ubertooth-util(1), SELECTING A DEVICE).

## OUTPUT AND DIAGNOSTICS

This is the split the tools are being brought to, tool by tool. Each tool's
own page says what it does today, under DIAGNOSTICS and EXIT STATUS; where a
page has neither section yet, that tool has not been converted.

Standard output carries only what was asked for: the report a query returns,
the packets or samples a capture decodes, the capture data itself when the
file argument is `-`, and the usage text for `-h`. Everything else goes to
standard error, so a script reading a tool's standard output is not fed a
diagnostic or a progress line.

Errors are one line starting `error: `; warnings start `warning: ` and notes
start `note: `. Progress and status ("Resetting ...", "Ubertooth stopped", DFU
progress) carry no prefix. The usage text goes to standard output for `-h`, and
to standard error after the `error: ` line when a tool that needs arguments is
run with none (a tool with work to do and no options, such as
ubertooth-specan(1), runs instead). Every other refused command line is the
`error: ` line alone: it describes the problem, so `-h` is where the usage text
comes from.

Exit status is 0 only when the command did what was asked, including `-h`, and
1 otherwise: a usage error, a command line with nothing to do, a device that
is missing or failed, a partial failure. ubertooth-afh(1) also uses 2, for a
detection phase that received no packet.

One warning is worth knowing on sight. When a capture ends, the USB transfer
it was reading into has to come back before the device can be closed; a board
that never returns it leaves the tool no safe way to close the device, so it
says `warning: the receive transfer did not finish; the Ubertooth is left open
and may still be capturing` and exits with the status the command earned. The
board usually needs `ubertooth-util [-U <selector>] -S` or `-r` afterwards, and
one that answers neither has to be unplugged and plugged in again.

libubertooth follows the split already: it returns what it reads to the tool
that called it and reports its own problems on standard error, so a library
message does not land in the middle of a capture. The clock trim and AFH
channel notes raised while following a piconet are `note: ` lines on standard
error. What the library still writes to standard output is the calling tool's
result — the decoded packet reports, the AFH map, the version line — which is
the right stream for them, whichever layer prints them. Its device-selection
messages are on standard error but do not all carry a prefix yet.

## ENVIRONMENT

`UBERTOOTH_TEST_NO_DEVICES=1` makes every tool behave as if no Ubertooth were
attached: nothing is listed or opened on the USB bus, and the tool reports that
it found no device. It exists for the project's own test suite, which runs the
tools while boards may be in use elsewhere, and it is the only setting that
changes device selection. Only the exact value `1` turns it on; unset, empty or
any other value leaves device selection alone. A tool that finds no board
because of it says so:
`note: UBERTOOTH_TEST_NO_DEVICES is set, so no board is selected`.

## SUPPORT

Ubertooth is an open source project maintained primarily by volunteers.
Please keep that in mind when seeking support.

If you are having issues with a tool or believe you have found a bug,
please file an issue on the project GitHub:
<https://github.com/greatscottgadgets/ubertooth>

For interactive support, join the `#ubertooth` channel on Discord:
<https://discord.gg/rsfMw3rsU8>

## COPYRIGHT

The code and documentation for Project Ubertooth are Copyright 2010-2017
and are available under the terms of the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for
details. Ubertooth is a registered trademark of Great Scott Gadgets.

## AUTHOR

This manual page was written by Mike Ryan.
