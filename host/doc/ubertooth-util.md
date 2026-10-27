# ubertooth-util(1) -- general purpose Ubertooth utility

## SYNOPSIS

    ubertooth-util [-v] [-V] [-H] [--host-version] [-I] [-d [0-1]] [-l [0-1]]
    [-S] [-r] [-U <index|serial|all>] [-a [0-7]] [-c [2402-2480]] [-C [0-78]]
    [-q [20-120]] [-t] [-z [-128..127]] [-e] [-m] [-n] [-i] [-b] [-p] [-s]
    [-N] [-x] [-h]

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

Without a number after it, `-c` (and `-a`, `-C`, `-d`, `-l`, `-z`) reads the
current value instead. See ubertooth(7), OPTION VALUES, for the forms an
option's value can take.

A set and a read of the same value in one command both run, the set first, so
the read reports the new value. The order they are written in does not matter:

    $ ubertooth-util -c 2426 -c
    Current frequency: 2426 MHz (Bluetooth channel 24)

The same holds for `-a`, `-C`, `-d`, `-l` and `-z`. Two sets of the same value
in one command keep the last one.

Setting and reading are separate device operations here, which is why one
command can do both; `ubertooth-btle -a` refuses the combination instead,
because there the access address parameterises the capture that command is
about to start. One exception to the read reporting the new value: a board
already running a connection-following BLE mode keeps a requested channel
aside until that mode ends, so `-c 2426 -c` against such a board reports the
frequency still in use. `ubertooth-util` cannot enter that mode itself, so this
only arises alongside another program.

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
ends by itself when the test finishes, and waits for a reply until `-S` stops
it if no repeater answers. `-S` stops either of the range test pair at any
point.

`-I` (identify) blinks the USR, RX and TX LEDs together, about three on-off
blinks a second, until it is stopped:

    ubertooth-util -U <serial> -I      # start blinking
    ubertooth-util -U <serial> -S      # stop; the LEDs end off

`-I` needs firmware of this tree's API version and refuses any other, saying
so. The other options work with any firmware, so `-v` can still read the
version, and ubertooth-dfu(1) can reflash it.

The API version is bumped whenever a firmware change affects the host, so a
board flashed from an earlier commit is normally refused rather than used. It
still does not pin down a commit, since several commits share one number: the
firmware revision shown by `-v` beside the API version is what identifies the
build. Flash the firmware from the commit the tools were built from
(`./install.sh --flash`).

Only one mode option can be given per command: `-q`, `-n`, `-e`, `-t`, `-x`,
`-I` and `-i` (In-System Programming, ISP) each end the command, so a command
with two or more of them is refused before any device is opened, nothing is
sent to the device, and the exit status is 1:

    $ ubertooth-util -q -n
    error: options -q -n cannot be combined; give only one of -q -n -e -t -x -I -i

To put a board into the DFU bootloader and reflash it, use ubertooth-dfu(1),
which enters the bootloader itself as part of a write (`-d`), a read (`-u`) or
`-r`. `ubertooth-util -f` entered it too, but warned for years that it was no
longer required and pointed at ubertooth-dfu(1); it has been removed, so `-f`
is now an option the tool does not know (`error: unrecognized option '-f'`,
exit 1). Nothing enters the bootloader and leaves the board there any more; to
park a board in it, jumper the expansion header (ubertooth-dfu(1), MANUALLY
ENTERING BOOTLOADER).

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
 - `-d [0-1]` :
   get/set all LEDs
 - `-l [0-1]` :
   get/set USR LED
 - `-S` :
   stop current operation
 - `-r` :
   full reset; then waits up to 10 seconds for the same board to come back on
   USB before any later option runs, and exits non-zero with `error: Ubertooth
   device <selector> did not come back within 10 s after the reset` if it does
   not. The board's serial number is read before the reset and the wait looks
   for that serial, whatever `-U` selected, so a later option in the same
   command (`-U 0 -r -b`) stays on the board that was reset even if the index
   order changed across the reset. If the serial cannot be read, the `-U`
   argument is waited for instead; unless it is a serial, or the only board
   attached is this one, a warning on stderr says later options may reach a
   different board
 - `-U <index|serial>` :
   select which Ubertooth to use, either by USB enumeration index or by its
   serial number string. See SELECTING A DEVICE.
 - `-U all` :
   report the read-only queries `-b`, `-v`, `-V`, `-p` and `-s` for every
   attached Ubertooth; refused with any other option, `-N` excepted, and,
   unless `-N` is the only option beside it, refused with no query to list.
   See SELECTING A DEVICE.
 - `-N` :
   print the number of attached Ubertooths, as a single decimal number on
   standard output, then run the rest of the command. The number comes from
   the USB device list, so it needs no board and opens none, and it is taken
   before any device is opened: it is printed first, ahead of anything the
   other options report. `-N` is neither a selector nor a per-device query, so
   it combines with everything else, in either order — reads, setters, modes,
   `-r`, `-S`, `-U <index|serial>` and the `-U all` listing — and a count that
   succeeds leaves the exit status to the rest of the command. On its own, or
   with nothing but a `-U` selector beside it (`-U all` included), it prints
   the count, opens no device and exits 0, unless the count itself fails: a
   selector alone asks nothing of the board it names. A command line refused
   before anything runs (two modes, `-z` with a mode, `-U all` with an option
   it does not support, a bad option value, an unknown option or a stray word)
   prints no count, and `-h`, `-H` and `--host-version` end the command before
   it, as they do before every other option.

Radio options:

 - `-a [0-7]` :
   get/set power amplifier level, 0 (lowest, -25 dBm) to 7 (maximum, 0 dBm) at
   the CC2400 radio. The board powers on and resets (`-r`) at level 7, the
   CC2400's own reset value, which the firmware keeps rather than choosing a
   level of its own. Receiving does not change it. Transmit modes use the level
   set here and leave it unchanged: `-t` (TX test), Bluetooth and BLE transmit,
   and BLE and E-GO jamming. The range test and repeater (`-n`, `-e`) step
   through levels by design and leave the level wherever their sweep ended, so
   set it again after using them.
 - `-c [2402-2480]` :
   get/set channel in MHz. A standalone set is kept: the board holds that
   channel after the command exits, so a later command reads the same value
   back, and the next mode starts on it unless that tool sets a channel of its
   own. Once that mode ends the channel is back to 2441 MHz, as described
   above, so set it again for the one after. See CARRY-OVER BETWEEN RUNS.
 - `-C [0-78]` :
   get/set channel, by Bluetooth channel number rather than frequency. It is the
   same setting as `-c` and keeps its value the same way.
 - `-q [20-120]` :
   start LED spectrum analyzer. The threshold is -N dBm, so a larger N lights
   the LEDs for weaker signals; 20-120 is the radio's RSSI range, and bare `-q`
   uses -85 dBm, which shows Wi-Fi traffic without lighting on noise. The TX, RX and USR LEDs stand for 2412, 2437
   and 2462 MHz. `-S` stops it and leaves the LEDs off.
 - `-t` :
   intitiate continuous transmit test
 - `-z [-128..127]` :
   get/set squelch level (a signed RSSI threshold; values above 0 are relative
   to the average RSSI) for an RX capture. The firmware puts it back to the
   default -120 dBm on every mode change, so set it in the same step as the
   capture it is meant for, or once that capture is running; it does not carry
   into the next mode. See [SQUELCH][]. `-z` cannot be combined with a mode
   option (`-q`, `-n`, `-e`, `-t`, `-x`, `-I`, `-i`), which would reset it
   before the mode started.

Range test:

 - `-e` :
   start repeater mode
 - `-m` :
   display range test result
 - `-n` :
   initiate range test

Miscellaneous:

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

- `-U <n>` selects by index (`-U 0` is the first match, `-U 1` the second, and
  so on). This index reflects **USB enumeration order** (bus/port and plug
  order) — it is **not** derived from the serial number and is **not guaranteed
  stable** across reboots or re-plugging.
- `-U <serial>` selects the device whose serial number matches the given string
  (`-s` prints the serials). This is stable across reboots and re-plugging, so
  it is the recommended way to pin a specific board.

Index or serial is auto-detected: an all-digits value is treated as an index,
anything else as a serial. See ubertooth(7), OPTION VALUES, for the forms the
value can take.

An index must be one that is actually attached: it is a position in the list of
attached Ubertooths, so with *n* attached only `-U 0` to `-U <n-1>` select a
device. Any other index is refused before any device is opened, and the tool
exits non-zero:

    $ ubertooth-util -U 2 -s
    error: no Ubertooth at index 2 (2 attached)
    indexes change when boards are added or removed; select a board by serial with -U<serial>
    could not open Ubertooth device

Indexes are positions in the list of boards attached when the command runs:
unplugging `-U 0` makes the old `-U 1` the new `-U 0`. To keep targeting the same
board, select it by serial number, in full or by its first few characters (as
long as no other attached board's serial starts the same way).

This includes a nonzero index with a single Ubertooth attached (`-U 1` of one
device is refused; use `-U 0`, or no `-U` at all). With no Ubertooth attached,
any `-U <n>` reports that no device could be opened.

A digit string too large to be a device index at all is refused before the
device list is read, naming it as given. That is the whole message, without the
`could not open Ubertooth device` line, because no device was looked for:

    $ ubertooth-util -U 4294967296 -s
    error: no Ubertooth at index 4294967296 (2 attached)
    indexes change when boards are added or removed; select a board by serial with -U<serial>

There is **no fixed upper limit** on the number of attached Ubertooths: the
device list is sized dynamically and the index is not capped at any particular
number (older docs showed `-U <0-7>`, which was only a convention). In practice
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
apart, and the message says so and points at `-U <index>` instead.

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

`-U all` only applies to the read-only queries `-b`, `-v`, `-V`, `-p` and `-s`.
Combined with any other option (a setter, a mode such as `-x` or `-q`, `-r`,
`-S`, or another read such as `-c`) it is refused before any device is opened:

    error: -U all only supports the read-only queries -b, -v, -V, -p and -s; select one device with -U<index|serial> for other options

Given with no query at all there is nothing to list, and the refusal says so:

    $ ubertooth-util -U all
    error: -U all needs one of the read-only queries -b, -v, -V, -p or -s to list

`-N` is the one option that is not refused: it asks about the USB bus rather
than about a device, so it prints the count and the listing follows it, from
the same enumeration:

    $ ubertooth-util -U all -N -s
    2
    0: 1cc001038088cb956c43ad61c52000f5
    1: 05f00014e380df95f4436562c32000f5

`-U all -N` with no query asks only for the count, which comes from the bus and
not from any device. There is nothing to list, so nothing is refused either: it
prints the count and exits 0, as `-U 0 -N` does.

    $ ubertooth-util -U all -N
    2

Only the exact word `all` has this meaning. The other tools do not support
`-U all`; they treat it as a serial number and report that no device matches.

To tell which index or serial is which physical device:

- Run `ubertooth-util -s`, or any other tool with no `-U`: with more than one
  Ubertooth present, the tool lists each device's index with its serial number
  (other tools refuse to pick one). Use that to map index to serial.
- Or flash the LEDs on a given device and watch which board blinks, then stop
  it:

        ubertooth-util -U 0 -I
        ubertooth-util -U 0 -S

For a stable identity across reboots, select a device by its serial number
(`ubertooth-util -U <serial> -I`); unlike the index, the serial does not change.

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
`no range test result yet` on standard output and exits 0: there is no result
yet, which is not a failure. A test that ran but whose packets came back
corrupted has no result to report either, so it is a diagnostic rather than a
value: `error: invalid range test: mismatch on byte <n>` on standard error,
with exit status 1 and nothing on standard output.

**The range test does not yet produce a usable measurement.** With a repeater
running, the exchange completes within a couple of seconds, and the reply
arrives with a valid checksum and reads back corrupted, so `-m` reports
`error: invalid range test: mismatch on byte <n>` rather than power levels.
That is the expected outcome today, on any pair of boards; which byte it names
depends on the sender's serial number. Tracked as
[issue 26](https://github.com/SqueakyFloors/ubertooth/issues/26).

Note that the range test transmits the Cortex M3 microcontroller's
serial number. If you do not wish to broadcast that information
wirelessly, do not use the range test functionality.

## SQUELCH

`-z` writes the squelch to the board. It gates the carrier-sense trigger of
the Classic Bluetooth symbol capture (`ubertooth-rx`, `ubertooth-dump`,
`ubertooth-scan`, `ubertooth-follow`, `ubertooth-afh`) and of the BLE
promiscuous mode of `ubertooth-btle`; at the default -120 dBm nothing is
squelched. No other tool sets or reports it.

It lasts for one mode, not until the board is reset: the firmware runs its
idle reset whenever it leaves a mode, before the next one starts, and that
puts the squelch back to -120 dBm along with the LEDs and the channel. So a
value set for one capture never reaches a later one. That is deliberate — a
squelch left behind would quietly change every capture that followed — and it
is the behaviour to work with:

- Set it while the board is idle, then start the capture. A mode entered from
  idle keeps the value, because no mode is being left:

        ubertooth-util -U <serial> -z -70
        ubertooth-rx -U <serial>

- Or set it once the capture is running: `-z` works on a board that is in a
  mode and takes effect immediately. Bare `-z` reads back what the running
  capture is using.

- Set it again for each capture. After a capture ends, or as soon as one mode
  replaces another, `-z` reads -120 dBm again.

`-z` together with one of `ubertooth-util`'s own mode options is refused
rather than quietly dropped, because each of those modes leaves whatever was
running first and the reset would discard the value before the mode started.

## CARRY-OVER BETWEEN RUNS

One rule covers what a board holds from one command to the next:

- **Every mode start resets the mode state, hop mode and AFH channel map the
  mode before it left.** A capture hops the way the mode that started it asks,
  and no other way: `ubertooth-rx` after an `ubertooth-afh` hops across the
  whole channel set, not on the map that run found. This holds even when the
  earlier tool was killed and left the board streaming. The squelch and the
  LEDs go with it, as above; the power amplifier level set with `-a` does not.
- **A channel set on its own is kept, and the next mode starts on it.** `-c`/`-C`
  with no mode on the command line leaves the board on that channel for as long
  as it stays idle, a later `-c` reads it back, and a mode started afterwards
  captures there — unless that tool sets a channel of its own, as `ubertooth-rx`
  does when it asks for a band sweep. Once that mode ends the channel is back to
  2441 MHz, so set it again for the next one.

What is set on an idle board is for the capture that follows, as the squelch
is: no mode is being left, so there is nothing to reset. What a mode left
behind, on the other hand, never decides how the next one captures.

Re-sending the mode a board is already running is not a mode start, and resets
nothing. That is what lets a tool restate its mode mid-session without losing
what it is capturing on, as `ubertooth-follow` does each time it re-syncs its
clock. A tool cannot tell the board which of the two it is doing, so one that
wants a clean map after a restart says what it wants of the map once its mode
is running, rather than before it starts.

## EXIT STATUS

`ubertooth-util` exits with 0 when every requested operation succeeds, including
queries that report a nonzero value such as `-b` (board ID), `-c` (channel), or
`-a` (power amplifier level). `-m` with no range test result yet also exits 0
(see [RANGE TEST][]); `-m` on a result whose packets came back corrupted
exits 1. It exits with a nonzero status if any requested operation fails, even
when a later operation in the same command succeeds.

A command line that asks for no operation at all is refused before any device
is opened, rather than opening the selected board, doing nothing with it and
exiting 0:

    $ ubertooth-util
    error: nothing to do: give an option that reads or sets something, or -N to count the attached boards

    $ ubertooth-util -U 0
    error: nothing to do: -U '0' selects a device but no option asks anything of it

`-U` only says which board the rest of the command line acts on, so it is not
an operation of its own; `-N`, `-h`, `-H` and `--host-version` are, and each
works on its own. The usage text follows the `error: ` line on standard error,
and the exit status is 1.

It exits with 1, without opening a device, when two or more mode options are
given (see DESCRIPTION), when `-z` is combined with a mode option, or when an
`-a` level is not a single digit 0-7
(`error: PA level must be a single digit 0-7 (got <value>)`). The same holds
for any other option value that is not a whole decimal number in its range
(`-c`, `-C`, `-d`, `-l`, `-q`, `-z`), for example
`error: channel must be 0-78 (got abc)`.
It also exits with 1, without opening a device, when the command line contains
a word that is not an option, and names it: `error: unexpected argument '<word>'`.
`ubertooth-util` takes no other arguments. An option the tool does not know is
refused the same way, as one line on standard error with no usage text after
it: `error: unrecognized option '-Z'`; an option whose value is missing reads
`error: option '-U' requires a value`. The removed `-f` is one of those
options now: use ubertooth-dfu(1) instead.

A `-N` count that succeeds does not affect the exit status: the number is
printed and the rest of the command decides it. A count that cannot be read is
a failed operation like any other: it prints no number and makes the command
exit 1, and the rest of it still runs.

When a `-b`, `-c`, `-a`, `-z`, `-l`, `-d`, `-p`, `-s` or `-m` read fails,
no value line is printed on standard output, so an error code is never shown as
a value; the other requested queries are still reported. A failed `-b`, `-c`,
`-a`, `-z`, `-l`, `-d`, `-p`, `-s` or `-m` read also prints
`error: <query> read failed` (for example `error: channel read failed` or
`error: board ID read failed`) on standard error, after any message from
libubertooth. When the board returns fewer bytes than the read needs, that is
the only line, with the counts added, for example
`error: channel read failed: board returned 0 of 2 bytes`.

## DIAGNOSTICS

Standard output carries only what was asked for: the values the queries read,
the device listing, and the usage text for `-h`. Everything else goes to
standard error, so a script reading a value is not fed anything else.

That includes the progress lines for the operations that do something rather
than report something: `Resetting ubertooth device <selector>`,
`Stopping ubertooth device <selector>`,
`Entering flash programming (ISP) mode`,
`Entering LED specan mode (threshold -<n> dBm)`, `Starting range test`,
`Starting repeater` and `Starting TX test`. They were written to standard
output before.

Each operation says what it did once. `-I` reports the blink only through
`note: the board is blinking; stop it with ubertooth-util [-U <selector>] -S
or any mode option`, naming the selector so the command can be pasted; there
is no separate line announcing the LEDs. Setting the squelch with `-z <n>`
prints nothing, as `-a`, `-c`, `-C`, `-d` and `-l` do not: the value `-z`
reads back afterwards (`Squelch set to <n>`) is the result and goes to
standard output. A set that fails is still reported, on standard error, and
the command exits 1. `-x` has never had a line of its own.

The errors this tool prints itself are one line starting `error: `; its
warnings and notes start `warning: ` and `note: `. Some lines from elsewhere
do not follow that yet: the unknown-option line from the option parser, and
the libubertooth messages that have not been converted, such as `could not
open Ubertooth device`. Those that have carry the prefix, for example
`error: Ubertooth <serial> is in use by another program (libusb -3)`; see
ubertooth(7), WHEN A BOARD CANNOT BE USED.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-util(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
