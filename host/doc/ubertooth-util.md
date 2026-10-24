# ubertooth-util(1) -- general purpose Ubertooth utility

## SYNOPSIS

`ubertooth-util` [-v] [-V] [-H] [--host-version] [-I] [-d[0-1]] [-l[0-1]]
[-S] [-r] [-U<index|serial|all>] [-a[0-7]] [-c[2400-2483]] [-C[0-78]] [-q[1-225]] [-t]
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
   get firmware revision number; exits non-zero if the read fails
 - `-V` :
   get compile info; exits non-zero if the read fails
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
   full reset; then waits up to 10 seconds for the same device (by serial
   with `-U<serial>`) to come back on USB before any later option runs, and
   exits non-zero with `error: Ubertooth device <selector> did not come back
   within 10 s after the reset` if it does not. With several boards attached,
   select by serial: the index order can change across a reset
 - `-U<index|serial>` :
   select which Ubertooth to use, either by USB enumeration index or by its
   serial number string. See SELECTING A DEVICE.
 - `-U all` :
   report the read-only queries `-b`, `-v`, `-V`, `-p` and `-s` for every
   attached Ubertooth; refused with any other option. See SELECTING A DEVICE.

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
   get microcontroller Part ID (32-bit, hexadecimal); exits non-zero if the
   device reports a nonzero status for the read
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

With several Ubertooths attached and no `-U`, the read-only identity queries
`-s` (serial), `-v` (firmware version), `-V` (compile info), `-p` (part ID) and
`-b` (board ID) report every device, in enumeration order, instead of refusing
to pick one. A single query prints one `<index>: <value>` line per device:

    $ ubertooth-util -s
    0: 1cc001038088cb956c43ad61c52000f5
    1: 05f00014e380df95f4436562c32000f5

Several queries together (for example `-vVspb`) print each report line
prefixed with the device index. A device that cannot be opened is shown as
`<index>: <unavailable>`, a failed read as `<index>: <query read failed>`, and a
query that reports an error as that error (for example `<error: unknown board
ID 7>`). The other devices are still reported, and the command exits non-zero
if any device failed. A failed `-v` or `-V` read is shown as
`<index>: <firmware read failed>` or `<index>: <compile info read failed>`; a
firmware version whose API version cannot be read is shown without it and also
makes the command fail. Listing does not stop a mode a device is running. Any other
option in the same command still needs `-U`.

`-U all` asks for the same listing explicitly. It works with any number of
attached devices, including one (`0: <value>`), and fails with `error: no
Ubertooth devices found` when none is attached:

    $ ubertooth-util -U all -s
    0: 1cc001038088cb956c43ad61c52000f5

`-U all` only applies to the read-only queries `-b`, `-v`, `-V`, `-p` and
`-s`. Combined with any other option (a setter, a mode such as `-x` or `-q`,
`-r`, `-S`, `-N`, or another read such as `-c`), or given with no query at
all, it is refused before any device is opened:

    error: -U all only supports the read-only queries -b, -v, -V, -p and -s; select one device with -U<index|serial> for other options

Only the exact word `all` has this meaning. The other tools do not support
`-U all`; they treat it as a serial number and report that no device matches.

To tell which index or serial is which physical device:

- Run `ubertooth-util -s`, or any other tool with no `-U`: with more than one
  Ubertooth present, the tool lists each device's index with its serial number
  (other tools refuse to pick one). Use that to map index to serial.
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

When a `-b`, `-c`, `-a`, `-z`, `-l`, `-d`, `-p`, `-s`, `-m` or `-9` read fails,
no value line is printed on standard output, so an error code is never shown as
a value; the other requested queries are still reported. A failed `-b`, `-c`,
`-a`, `-z`, `-l`, `-d`, `-p`, `-s`, `-m` or `-9` read also prints
`error: <query> read failed` (for example `error: channel read failed` or
`error: board ID read failed`) on standard error, after any message from
libubertooth.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-util(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
