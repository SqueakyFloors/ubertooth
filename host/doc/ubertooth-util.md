# ubertooth-util(1) -- general purpose Ubertooth utility

## SYNOPSIS

`ubertooth-util` [-v] [-V] [-H] [--host-version] [-I] [-d[0-1]] [-l[0-1]]
[-S] [-r] [-U<index|serial|all>] [-a[0-7]] [-c[2402-2480]] [-C[0-78]] [-q[20-120]] [-t]
[-z[-128..127]] [-e] [-m] [-n] [-f] [-i] [-b] [-p] [-s] [-N] [-x] [-h]

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

An option value can be attached or given as the next word: `-c2440` and
`-c 2440` both set the channel, and `-z-90` and `-z -90` both set the squelch.
Without a number after it, `-c` (and `-a`, `-C`, `-d`, `-l`, `-z`) reads the
current value instead.

The utility also includes a simple range test tool for use with two
Ubertooth devices. Refer to [RANGE TEST][] for full details.

Operations that put the device into a continuous mode — `-I` (identify),
`-q` (LED spectrum), `-x` (xmas), `-t` (TX test), `-n` (range test), and
`-e` (repeater) — first stop any operation already running on that device, so
each starts from a known idle state. Stopping a mode, with `-S` or normally by
starting another, puts the board back to idle: LEDs off, channel 2441 MHz and
squelch -120 dBm. The power amplifier level set with `-a` is kept. Each of
them runs on the board and `ubertooth-util` returns immediately, so the board
stays usable from other commands meanwhile. `-I`, `-q`, `-x`, `-t` and `-e` keep
running until `-S` stops them or another mode replaces them; `-n` (range test)
ends by itself when the test finishes.

`-I` (identify) blinks the USR, RX and TX LEDs together, about three on-off
blinks a second, until it is stopped:

    ubertooth-util -U<serial> -I      # start blinking
    ubertooth-util -U<serial> -S      # stop; the LEDs end off

`-I` needs firmware of this tree's API version and refuses any other, saying
so. The other options work with any firmware, so `-v` can still read the
version and `-f` can reflash it.

Only one mode option can be given per command: `-q`, `-n`, `-e`, `-t`, `-x`,
`-I`, `-i` (In-System Programming, ISP) and `-f` (Device Firmware Upgrade,
DFU) each end the command, so a command with two or more of them is refused
before any device is opened, nothing is sent to the device, and the exit
status is 1:

    $ ubertooth-util -q -n
    error: options -q -n cannot be combined; give only one of -q -n -e -t -x -I -i -f

## OPTIONS

Common options:

 - `-v` :
   get firmware revision number; exits non-zero if the read fails
 - `-V` :
   get compile info; exits non-zero if the read fails
 - `-H`, `--host-version` :
   get host tools (libubertooth) version; works without a device
 - `-I` :
   identify ubertooth device by flashing the USR, RX, and TX LEDs on the board;
   the command returns at once and the blink runs until `-S` or any mode option
   ends it, leaving the LEDs off (see LEDs doc)
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
 - `-U <index|serial>` :
   select which Ubertooth to use, either by USB enumeration index or by its
   serial number string. See SELECTING A DEVICE.
 - `-U all` :
   report the read-only queries `-b`, `-v`, `-V`, `-p` and `-s` for every
   attached Ubertooth; refused with any other option. See SELECTING A DEVICE.

Radio options:

 - `-a[0-7]` :
   get/set power amplifier level, 0 (lowest, -25 dBm) to 7 (maximum, 0 dBm) at
   the CC2400 radio. The board powers on and resets (`-r`) at level 7, the
   CC2400's own reset value (the firmware does not set it at start-up).
   Receiving does not change it. Transmit modes use the level set here and
   leave it unchanged: `-t` (TX test), Bluetooth and BLE transmit, and BLE and
   E-GO jamming. The range test and repeater (`-n`, `-e`) step through levels
   by design and leave the level wherever their sweep ended, so set it again
   after using them.
 - `-c[2402-2480]` :
   get/set channel in MHz
 - `-C[0-78]` :
   get/set channel
 - `-q[20-120]` :
   start LED spectrum analyzer. The threshold is -N dBm, so a larger N lights
   the LEDs for weaker signals; 20-120 is the radio's RSSI range, and bare `-q`
   uses -85 dBm, which shows Wi-Fi traffic without lighting on noise. The TX, RX and USR LEDs stand for 2412, 2437
   and 2462 MHz. `-S` stops it and leaves the LEDs off.
 - `-t` :
   intitiate continuous transmit test
 - `-z[-128..127]` :
   get/set squelch level (a signed RSSI threshold; values above 0 are relative
   to the average RSSI) for a later RX capture.
   Stopping a running mode resets it to the default, so `-z` cannot be
   combined with a mode option (`-q`, `-n`, `-e`, `-t`, `-x`, `-I`, `-i`,
   `-f`).

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
- `-U<serial>` selects the device whose serial number matches the given string
  (`-s` prints the serials). This is stable across reboots and re-plugging, so
  it is the recommended way to pin a specific board.

The argument may be written **attached or separated** — `-U0` and `-U 0` are
equivalent, as are `-U<serial>` and `-U <serial>`. (Index or serial is
auto-detected: an all-digits argument is treated as an index, anything else as
a serial.)

An index must be one that is actually attached: it is a position in the list of
attached Ubertooths, so with *n* attached only `-U0` to `-U<n-1>` select a
device. Any other index is refused before any device is opened, and the tool
exits non-zero:

    $ ubertooth-util -U2 -s
    error: no Ubertooth at index 2 (2 attached)
    indexes change when boards are added or removed; select a board by serial with -U<serial>
    could not open Ubertooth device

Indexes are positions in the list of boards attached when the command runs:
unplugging `-U0` makes the old `-U1` the new `-U0`. To keep targeting the same
board, select it by serial number, in full or by its first few characters (as
long as no other attached board's serial starts the same way).

This includes a nonzero index with a single Ubertooth attached (`-U1` of one
device is refused; use `-U0`, or no `-U` at all). With no Ubertooth attached,
any `-U<n>` reports that no device could be opened.

A digit string too large to be a device index at all is refused before the
device list is read, naming it as given. That is the whole message, without the
`could not open Ubertooth device` line, because no device was looked for:

    $ ubertooth-util -U4294967296 -s
    error: no Ubertooth at index 4294967296 (2 attached)
    indexes change when boards are added or removed; select a board by serial with -U<serial>

There is **no fixed upper limit** on the number of attached Ubertooths: the
device list is sized dynamically and the index is not capped at any particular
number (older docs showed `-U<0-7>`, which was only a convention). In practice
the limit is USB topology, hub bandwidth, and power — with many devices, prefer
serial selection since indices become unwieldy and unstable.

A serial number need not be given in full: its **first few characters** are
enough, upper or lower case. For a board whose serial is
`1cc001038088cb956c43ad61c52000f5`, `-U 1cc0` and `-U 1CC0` both select it.

If those characters start the serial of **more than one** attached board, the
tool selects neither: it lists the matching serials and exits non-zero, and a
few more characters pick one:

    $ ubertooth-util -U 1c -s
    error: '1c' matches 2 Ubertooth devices: 1cc001038088cb956c43ad61c52000f5, 1cd3f0a9b4e26c1d8a7f30b5c42000f5
    select one of them by a longer prefix of its serial number

If the matching boards report the *same* serial, no longer string can tell them
apart, and the message says so and points at `-U<index>` instead.

A string that matches no attached board also exits non-zero, with
`no Ubertooth found with serial matching '<string>'`.

A board that cannot be opened, or that does not answer the serial read, is
skipped quietly while another board is being selected. It is reported with the
result, because the string may well be that board's serial:

    $ ubertooth-util -U 1cc0 -s
    no Ubertooth found with serial matching '1cc0'
    2 attached Ubertooth devices could not be opened
    libUSB Error: Access denied (insufficient permissions)

(On Linux, that usually means the udev rules are not installed.)

An all-digits string is always an index, never a serial, even when it would
match a serial: `-U 05` asks for index 5, not for the board whose serial starts
`05f0`. Select that board by a longer prefix that includes a non-digit
(`-U 05f`), or by its full serial.

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
- Or flash the LEDs on a given device and watch which board blinks, then stop
  it:

        ubertooth-util -U0 -I
        ubertooth-util -U0 -S

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

Before a range test has completed since the board started, `-m` prints
`no range test result yet` and exits 0. A test whose packets did not match prints
`Invalid range test: mismatch on byte <n>`.

Note that the range test transmits the Cortex M3 microcontroller's
serial number. If you do not wish to broadcast that information
wirelessly, do not use the range test functionality.

## EXIT STATUS

`ubertooth-util` exits with 0 when every requested operation succeeds, including
queries that report a nonzero value such as `-b` (board ID), `-c` (channel), or
`-a` (power amplifier level). `-m` with no range test result yet also exits 0
(see [RANGE TEST][]). It exits with a nonzero status if any requested
operation fails, even when a later operation in the same command succeeds.
It exits with 1, without opening a device, when two or more mode options are
given (see DESCRIPTION), when `-z` is combined with a mode option, or when an
`-a` level is not a single digit 0-7
(`error: PA level must be a single digit 0-7 (got <value>)`). The same holds
for any other option value that is not a whole decimal number in its range
(`-c`, `-C`, `-d`, `-l`, `-q`, `-z`), for example
`error: channel must be 0-78 (got abc)`.
It also exits with 1, without opening a device, when the command line contains
a word that is not an option, and names it: `error: unexpected argument '<word>'`.
`ubertooth-util` takes no other arguments.

When a `-b`, `-c`, `-a`, `-z`, `-l`, `-d`, `-p`, `-s`, `-m` or `-9` read fails,
no value line is printed on standard output, so an error code is never shown as
a value; the other requested queries are still reported. A failed `-b`, `-c`,
`-a`, `-z`, `-l`, `-d`, `-p`, `-s`, `-m` or `-9` read also prints
`error: <query> read failed` (for example `error: channel read failed` or
`error: board ID read failed`) on standard error, after any message from
libubertooth. When the board returns fewer bytes than the read needs, that is
the only line, with the counts added, for example
`error: channel read failed: board returned 0 of 2 bytes`.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-util(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
