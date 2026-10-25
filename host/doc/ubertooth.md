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
