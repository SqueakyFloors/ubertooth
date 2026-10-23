# ubertooth-util(1) -- general purpose Ubertooth utility

## SYNOPSIS

`ubertooth-util` [-v] [-V] [-H] [--host-version] [-I] [-d[0-1]] [-l[0-1]]
[-S] [-r] [-U<index|serial>] [-a[0-7]] [-c[2400-2483]] [-C[0-78]] [-q[1-225]] [-t]
[-z] [-e] [-m] [-n] [-f] [-i] [-b] [-p] [-s] [-N] [-x] [-h]

## DESCRIPTION

ubertooth-util(1) is a catch-all tool for doing generally useful things
with, to, and about Ubertooth. To get firmware revision and compile
information, run:

    ubertooth-util -vV

To fully reset Ubertooth, run:

    ubertooth-util -r

Other options are available for modifying LED states, setting channel,
and performing miscellaneous radio-related actions. Refer to the
[OPTIONS][] section for full details.

The utility also includes a simple range test tool for use with two
Ubertooth devices. Refer to [RANGE TEST][] for full details.

Operations that put the device into a continuous mode — `-I` (identify),
`-q` (LED spectrum), `-x` (xmas), `-t` (TX test), `-n` (range test), and
`-e` (repeater) — first stop any operation already running on that device, so
each starts from a known idle state. (This only resets the operating mode;
configured channel and power level are preserved.)

## OPTIONS

Common options:

 - `-v` :
   get firmware revision number
 - `-V` :
   get compile info
 - `-H`, `--host-version` :
   get host tools (libubertooth) version; works without a device
 - `-I` :
   identify ubertooth device by flashing the USR, RX, and TX LEDs (runs until Ctrl-C; see LEDs doc)
 - `-d[0-1]` :
   get/set all LEDs
 - `-l[0-1]` :
   get/set USR LED
 - `-S` :
   stop current operation
 - `-r` :
   full reset
 - `-U<index|serial>` :
   select which Ubertooth to use, either by USB enumeration index or by its
   serial number string. See SELECTING A DEVICE.

Radio options:

 - `-a[0-7]` :
   get/set power amplifier level
 - `-c[2400-2483]` :
   get/set channel in MHz
 - `-C[0-78]` :
   get/set channel
 - `-q[1-225 (RSSI threshold)]` :
   start LED spectrum analyzer
 - `-t` :
   intitiate continuous transmit test
 - `-z` :
   set squelch level

Range test:

 - `-e` :
   start repeater mode
 - `-m` :
   display range test result
 - `-n` :
   initiate range test

Miscellaneous:

 - `-f` :
   activate flash programming (DFU) mode
 - `-i` :
   activate In-System Programming (ISP) mode
 - `-b` :
   get hardware board id number
 - `-p` :
   get microcontroller Part ID
 - `-s` :
   get microcontroller serial number

## SELECTING A DEVICE

With a single Ubertooth attached, the tools find it automatically and `-U`
is unnecessary.

With **multiple** Ubertooths attached, `-U` selects one either by its position
in the USB device list or by its serial number:

- `-U<n>` selects by index (`-U0` is the first match, `-U1` the second, and so
  on). This index reflects **USB enumeration order** (bus/port and plug order) —
  it is **not** derived from the serial number and is **not guaranteed stable**
  across reboots or re-plugging.
- `-U<serial>` selects the device whose serial number matches the given string.
  This is stable across reboots and re-plugging, so it is the recommended way
  to pin a specific board.

The argument may be written **attached or separated** — `-U0` and `-U 0` are
equivalent, as are `-U<serial>` and `-U <serial>`. (Index or serial is
auto-detected: an all-digits argument is treated as an index, anything else as
a serial.)

There is **no fixed upper limit** on the number of attached Ubertooths: the
device list is sized dynamically and the index is not capped (older docs showed
`-U<0-7>`, which was only a convention). In practice the limit is USB topology,
hub bandwidth, and power — with many devices, prefer serial selection since
indices become unwieldy and unstable.

To tell which index or serial is which physical device:

- Run any tool with no `-U`: when more than one Ubertooth is present the tool
  refuses to pick and instead lists each device's index with its serial number.
  Use that to map index to serial.
- Or flash the LEDs on a given device and watch which board blinks:

        ubertooth-util -U0 -I

For a stable identity across reboots, select a device by its serial number
(`ubertooth-util -U<serial> -I`); unlike the index, the serial does not change.

## RANGE TEST

Using two Ubertooth devices it is possible to perform a range test. One
Ubertooth acts as a repeater by running:

    ubertooth-util -e

A second Ubertooth (the sender) initiates the range test by running:

    ubertooth-util -n

During the range test the sender Ubertooth will transmit data packets at
increasing power level, listening for repeats from the repeater
Ubertooth. The receiving Ubertooth will validate that the received data
was not corrupted in flight.

The range test results can be displayed on the sender Ubertooth's system
by running:

    ubertooth-util -m

Note that the range test transmits the Cortex M3 microcontroller's
serial number. If you do not wish to broadcast that information
wirelessly, do not use the range test functionality.

## EXIT STATUS

`ubertooth-util` exits with 0 when every requested operation succeeds, including
queries that report a nonzero value such as `-b` (board ID), `-c` (channel), or
`-a` (power amplifier level). It exits with a nonzero status if any requested
operation fails, even when a later operation in the same command succeeds.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-util(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to `COPYING` for further details.
