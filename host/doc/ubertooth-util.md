# ubertooth-util(1) -- general purpose Ubertooth utility

## SYNOPSIS

    ubertooth-util [-V] [-v] [-I] [-d [0-1]] [-l [0-1]]
    [-S] [-r] [-U <index|serial|all>] [-a [0-7]] [-c [2402-2480]] [-C [0-78]]
    [-q [20-120]] [-t] [-z [-128..127]] [-e] [-m] [-n [seconds]] [-i] [-b]
    [-p] [-s]
    [-w] [-N] [-x] [-h]

## DESCRIPTION

ubertooth-util(1) is a catch-all tool for doing generally useful things
with, to, and about Ubertooth. To get the host tools' version and the
firmware version and build of each attached board, run:

    ubertooth-util -V

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
squelch -120 dBm. The power amplifier level set with `-a` is kept. `-I`, `-q`,
`-x`, `-t` and `-e` run on the board and the command returns immediately, so
the board stays usable from other commands meanwhile, and they keep running
until `-S` stops them or another mode replaces them. `-n` (range test) is the
one that does not return at once: the test it starts ends by itself, so `-n`
follows it and reports it, as [RANGE TEST][] describes. `-S` stops either of
the range test pair at any point.

`-I` (identify) blinks the USR, RX and TX LEDs together, about three on-off
blinks a second, until it is stopped:

    ubertooth-util -U <serial> -I      # start blinking
    ubertooth-util -U <serial> -S      # stop; the LEDs end off

`-I` needs firmware of this tree's API version, and treats any other the way
the rest of the tools do: a release build refuses it, a development build warns
and blinks anyway (see ubertooth(7), FIRMWARE OF ANOTHER API VERSION). The
other options are not gated on the API version, and ubertooth-dfu(1) reflashes
a board whatever firmware it is running, so a board is always recoverable with
an image from this tree.

`-V` is the version command, and gives the same answer in every tool here: the
tools written in Python answer it by running this one and passing its answer
on (ubertooth(7), COMMON OPTIONS). Its firmware lines, and `-v`, read the
firmware info block described below, so they name the image the board is
running:

    $ ubertooth-util -V
    Host tools version: libubertooth <version> (git-ed30131d*), libbtbb <version> (git-2fd0d39)
    libbtbb loaded from: /usr/local/lib/libbtbb.so.1
    Firmware version: bluetooth_rxtx git-ed30131d* (API:<version>)
    Firmware build: ubertooth git-ed30131d* (builder@buildhost) Mon Oct 27 11:22:33 2025
    $ ubertooth-util -v
    Firmware version: bluetooth_rxtx git-ed30131d* (API:<version>)

Every image built from this tree carries that block at a fixed offset, which
is also where ubertooth-dfu(1) reads it out of a firmware file, so a board and
the file it was flashed from are named in the same terms. Firmware built before
the block existed has none to read, and both options then report what it does
answer: the revision alone, with no image name in front of it. Identifying an
unknown board is what these options are for, so neither needs firmware from
this tree.

    $ ubertooth-util -v        # a board running 2020-12-R1
    Firmware version: 2020-12-R1 (API:1.07)

The API version beside the
revision is not from the block: it is the USB `bcdDevice`, which is the number
the other tools check a board against. The samples here write it as
`<version>`, because it moves with the firmware; `COMPATIBILITY.md` names the
value this tree requires. Both images report it, so a `btbr` board is not
refused over its version; the other tools still cannot drive that image, and
fail on the first request it does not answer.

During development the API version stays where it is through an ordinary
firmware change, because host tools and firmware are built and flashed
together, and moves for a change that breaks older host tools: a request
removed or renumbered, or a reply layout an older host would misread. A
release takes a version of its own, to mark that its tools and firmware work
only with each other; `COMPATIBILITY.md` has the whole rule. Where the two
numbers do disagree, a release build refuses the
board rather than using it and a development build warns about it, unless
`UBERTOOTH_ALLOW_API_MISMATCH` says otherwise (see ubertooth(7),
ENVIRONMENT). So the number does not pin down a commit, and many commits
share one: the firmware revision shown by `-v` beside the API version is what
identifies the build. Flash the firmware from the commit the tools were built
from (`./install.sh --flash`).

Only one mode option can be given per command: `-q`, `-n`, `-e`, `-t`, `-x`,
`-I` and `-i` (In-System Programming, ISP) each end the command, so a command
with two or more of them is refused before any device is opened, nothing is
sent to the device, and the exit status is 1:

    $ ubertooth-util -q -n
    error: options -q -n cannot be combined; give only one of -q -n -e -t -x -I -i

To put a board into the DFU bootloader and reflash it, use ubertooth-dfu(1),
which enters the bootloader itself as part of a write (`-d`), a read (`-u`) or
`-r`. `ubertooth-util -f` entered it too; earlier releases already noted that
it was no longer required and pointed at ubertooth-dfu(1), and it has now been
removed, so `-f` is an option the tool does not know (`error: unrecognized option '-f'`,
exit 1). Nothing enters the bootloader and leaves the board there any more; to
park a board in it, jumper the expansion header (ubertooth-dfu(1), MANUALLY
ENTERING BOOTLOADER).

## OPTIONS

Common options:

 - `-V` :
   print the version and end the command: the host tools, then the firmware
   version and build of the board `-U` names, or of each attached board. The
   same answer the other C tools give, including with no board attached; see
   ubertooth(7), COMMON OPTIONS
 - `-v` :
   get the image and revision the firmware was built from, as the
   `Firmware version:` line alone; exits non-zero if the read fails.
   **Deprecated**: use `-V`, which prints that line among the others. `-v`
   keeps working, and prints no warning, so scripts that parse it are left
   alone
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
   different board.

   A board that answers nothing has its USB connection reset first, because it
   would refuse the reset request as it refuses every other request: that is
   the re-plug done in software, and it is what recovers such a board. `-r`
   says `Ubertooth device <selector> is not answering; resetting its USB
   connection` and then resets the board as usual, so it comes back idle.
   Select such a board by index: its serial cannot be read, so it cannot be
   selected by serial. A board that answers nothing even after that exits
   non-zero with `error: Ubertooth device <selector> is still not answering
   after its USB connection was reset; power-cycle the board`
 - `-U <index|serial>` :
   select which Ubertooth to use, either by USB enumeration index or by its
   serial number string. See SELECTING A DEVICE.
 - `-U all` :
   report the read-only queries `-b`, `-v`, `-p` and `-s` for every
   attached Ubertooth; refused with any other option, `-N` excepted, and
   refused with no query to list, `-N` included: a count is not something the
   listing can show. `-h` and `-V` end the command before any of that, as
   they do before every other option. See SELECTING A DEVICE.
 - `-N` :
   print the number of attached Ubertooths, as a single decimal number on
   standard output, then run the rest of the command. The number comes from
   the USB device list, so it needs no board and opens none, and it is taken
   before any device is opened: it is printed first, ahead of anything the
   other options report. `-N` is neither a selector nor a per-device query, so
   it combines with everything else, in either order — reads, setters, modes,
   `-r`, `-S`, `-U <index|serial>` and the `-U all` listing — and a count that
   succeeds leaves the exit status to the rest of the command. On its own it
   prints the count, opens no device and exits 0, unless the count itself
   fails. A `-U` selector and nothing else beside it is refused, because `-N`
   asks nothing of the board the selector names:
   `error: nothing to do: -U '0' selects a device but no option asks anything
   of it`, and `-U all` with only `-N` beside it has nothing to list. A
   command line refused
   before anything runs (two modes, `-z` with a mode, `-U all` with an option
   it does not support, a bad option value, an unknown option or a stray word)
   prints no count, and `-h` and `-V` end the command before it, as they do
   before every other option.

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
   same setting as `-c` and keeps its value the same way, so a command line
   that gives a value to both is refused. See ARGUMENTS
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
   display the result of the last range test
 - `-n [seconds]` :
   run a range test and report it: a progress line about once a second while
   it runs, then the same result block `-m` prints, both on standard output.
   The value is how long to wait for the repeater's reply, 10 seconds by
   default; `-n 0` waits forever. Nothing answering within that time is an
   error on standard error, and the board is left stopped. See
   [RANGE TEST][].

Miscellaneous:

 - `-i` :
   activate In-System Programming (ISP) mode. The board hands itself to the
   on-chip programmer once the host has collected its answer — about a second
   later on `bluetooth_rxtx`, 10 ms on `btbr` — and stays there, answering no
   other request, until its power is cut: unplug it, or power-cycle its hub
   port. The command says so in a `note:` line of its own
 - `-b` :
   get hardware board id number
 - `-p` :
   get microcontroller Part ID (32-bit, hexadecimal); exits non-zero if the
   device reports a nonzero status for the read
 - `-s` :
   get microcontroller serial number
 - `-w` :
   get the watchdog state, why the board last reset itself, how deep its
   stack has been, and how many of the firmware's waits on the radio gave up.
   See [WATCHDOG][]; exits non-zero if either read fails
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value, a stray word or
   an option given twice is still reported.

## WATCHDOG

The firmware resets the board if it goes three seconds without servicing USB,
or without its mode getting back round its own loop. A board that stops
answering the host is useless to it and cannot be recovered from it — the reset
request travels over USB too — so the board recovers itself, re-enumerates, and
comes back idle. What a capture used to see as a hang is now the board
disappearing and coming back; the tools treat a board that has gone as lost and
exit.

The loop half is the weaker of the two. Six modes service USB from the
interrupt rather than by polling, and get no service pass at all while the host
is quiet, so for them the guard is that the mode loop is still turning. A board
in one of those whose USB path alone has died, with its loop still going round,
keeps feeding the watchdog and is not reset: `-r` on that board resets its USB
connection, and failing that it has to be unplugged or have its hub port
power-cycled, which is the fallback the tools' `libusb -7` note names. The
`btbr` image feeds the watchdog from its loop in the same way, so it has the
same gap. Everything the lab soak caught was caught by the other half.

A board the watchdog did reset can still answer nothing afterwards. It
re-enumerates in a few hundred milliseconds, which the host does not always
notice as the board leaving and coming back, and until it does every request to
that board is refused (`libusb -9`). `-r` on it resets the USB connection,
which is what the host needed to see, and the board answers again.

The bootloader has no self-reset at all, by design — it runs with the watchdog
off — so a `-7` mid-flash needs the same fallback, reported by ubertooth-dfu(1)
rather than by that note.

Before it resets, the firmware has already recorded what it was doing in a
corner of RAM that a reset does not clear, so `-w` can say where the board
was stuck:

    $ ubertooth-util -U <serial> -w
    Last boot cause: watchdog
    Watchdog: armed; 3000 ms without USB being serviced or the mode looping resets the board
    Unrequested watchdog resets since power-on: 2 (in 3 boots)
      the last was in BLE follow mode, waiting for the 16 MHz crystal oscillator
    Now in idle mode, not in a wait
    Capture transfer errors since power-on: 0
    Most stack ever used since boot: 432 of 11128 bytes
    Radio waits that gave up since boot: synthesizer lock 0, unlock 0, oscillator 0, state machine 0, packet bytes 0

`Last boot cause` is why the board started this time: `power-on`, `watchdog`,
`reset pin`, `brown-out`, or several of those joined with `+`. A reset the
firmware asked for — `-r`, or entering the bootloader for ubertooth-dfu(1) —
is also a watchdog reset, because that is how this microcontroller resets
itself; those are not counted and do not overwrite the last line, so the
counter and the mode it names describe only the times the board stopped
answering on its own.

A board running the `btbr` image reads `Now in a mode the board does not
report`: that image runs its own state machine rather than one of the modes
above, so it records no mode instead of claiming to be idle.

Both counters are since power-on and stop at 255. Unplugging the board, or
cutting power to its hub port, starts them again; `-r` does not. So does
flashing a firmware that keeps more in that corner of RAM than the one before
it: the record says which firmware wrote it, and a firmware that does not
recognize what it finds starts the counts over rather than reading its own
fields out of what was there.

`Capture transfer errors` counts the times the chip's transfer controller
reported an error on the channel a capture receives into. Each one costs the
packet that was arriving, unless the error came with the byte that finished it:
such a packet is whole, so the board keeps it and records the fault on it. The
promiscuous mode drains the receiver and carries on; the mode behind `-f` and
`-n` does not yet, and a capture that stops that way looks exactly like a quiet
band, which is why this count is worth reading after a capture that came back
short. It is since power-on and stops at 65535.

`Most stack ever used` is how deep the board's stack has been, out of the room
the link leaves it, and it is worth reading after a long capture or a mode
that is rarely exercised. The firmware writes a known word over the unused
stack when it starts and counts how much of it has gone, so the number is
measured and not estimated. Unlike the counters above it is since the last
boot, because a reset writes that word again. The second number is whatever
the build left over, so it moves by a few hundred bytes between compilers and
between the two images; compare a board against itself, not against the figure
above.

A board that has faulted adds one more line, above that count:

    Last processor fault: status 0x00000082, the access was at 0x4000c008

The four processor faults each blink a code and never return, so the watchdog
resets the board three seconds later; the reset line above names which of the
four it was, and this line says where it went wrong. `status` is the part's
fault status register for that fault. The address is the access the processor
refused, and is printed only when the status says the board recorded one for
this fault — zero is a real address, so the line says so rather than leaving
it out. There is none for a usage fault, for a hard fault the processor took
in its own right, nor for an instruction fetch refused because the region
cannot be executed — which is a branch through a corrupted pointer or return
address. A hard fault that is one of the other three escalated reports that
fault's status and address instead of its own, so it can carry one.

A refused address just below the stack is a stack overflow: the firmware keeps
32 bytes there that the part refuses, so a stack that runs out of room is
caught and reported where it happens rather than running on unnoticed. That one
reads as a memory management fault, or as an escalated hard fault where the
processor could not stack the fault's own frame clear of those 32 bytes. The
line is absent until a fault happens, and a power-cycle clears it.

The last line counts something else: the firmware's waits on the radio. Each
of them gives up after a bound and carries on, which is what keeps a radio
that never reports the state it was asked for from holding the board — but
carrying on is silent, so a half-working radio reads as a quiet channel. The
counts are the times each shape of wait ran out: the synthesizer never
locking, never going back off, the 16 MHz crystal never reporting stable, the
radio's state machine never reaching a state, and the radio stopping partway
through delivering a packet's bytes. The last of those is only ever above zero
after a `ubertooth-btle -p` capture, the one mode that waits for them. All zero
is what a working board shows; a count that climbs during a capture says the
samples or packets that came out of it are not to be trusted, whatever they
look like. They are since the board started (a `-r` reset included) and stop
at 65535.

`-i` (In-System Programming) is the one hand-over the watchdog cannot be
switched off for: it keeps the chip running, so the timeout is stretched to
about 71 minutes first.

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

Indexes are positions in the list of boards attached when the command runs:
unplugging `-U 0` makes the old `-U 1` the new `-U 0`. To keep targeting the same
board, select it by serial number, in full or by its first few characters (as
long as no other attached board's serial starts the same way).

This includes a nonzero index with a single Ubertooth attached (`-U 1` of one
device is refused; use `-U 0`, or no `-U` at all). With no Ubertooth attached,
any `-U <n>` says `error: no Ubertooth devices found`.

A digit string too large to be a device index at all is refused before the
device list is read, naming it as given:

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
`error: no Ubertooth found with serial matching '<string>'`.

A `note: ` line follows that when a board is sitting in the DFU bootloader: a
board being flashed enumerates as the bootloader, which this tool cannot open,
so it is not among the boards a selection can match. The selection still fails
the same way; the note only says why there is one board fewer than expected
(see ubertooth(7)).

A board that cannot be opened, or that does not answer the serial read, is
skipped quietly while another board is being selected. It is reported with the
result, because the string may well be that board's serial:

    $ ubertooth-util -U 1cc0 -s
    error: no Ubertooth found with serial matching '1cc0'
    2 attached Ubertooth devices could not be opened
    note: Access denied (insufficient permissions) (libusb -3)

(On Linux, that usually means the udev rules are not installed.)

An all-digits string is always an index, never a serial, even when it would
match a serial: `-U 05` asks for index 5, not for the board whose serial starts
`05f0`. Select that board by a longer prefix that includes a non-digit
(`-U 05f`), or by its full serial.

With several Ubertooths attached and no `-U`, the read-only identity queries
`-s` (serial), `-v` (firmware image and revision), `-p` (part ID) and
`-b` (board ID) report every device, in enumeration order, instead of refusing
to pick one. Each line is the label the single-device report prints, with the
device's index in it, so a line read on its own says both what it is and which
board it came from:

    $ ubertooth-util -s
    Serial No (0): 1cc001038088cb956c43ad61c52000f5
    Serial No (1): 05f00014e380df95f4436562c32000f5
    $ ubertooth-util -v
    Firmware version (0): bluetooth_rxtx git-747484c (API:<version>)
    Firmware version (1): bluetooth_rxtx git-747484c (API:<version>)

Several queries together (for example `-vspb`) print the same lines, grouped
by device. `-V` is not one of them: it is the version command, and reports
every attached board itself, indexed the same way where there is more than
one. With a single board it prints the unindexed form, which the listing here
does not.

A device that cannot be opened is shown as `unavailable, ` and the reason, for
example `Firmware version (1): unavailable, in use by another program`, with a
line per query asked for and libubertooth's own `error: ` line on standard
error. A failed read is shown as `read failed`
(`Firmware version (0): read failed`), and a query that reports an error as
that error without its `error: ` prefix (`Board ID Number (0): unknown board
ID 7`). The other devices are still reported, and the command exits
non-zero if any device failed. A firmware version whose API version cannot be
read is shown without it and also makes the command fail. Listing does not
stop a mode a device is running. Any other option in the same command still
needs `-U`.

`-U all` asks for the same listing explicitly. It works with any number of
attached devices, including one, and fails with `error: no Ubertooth devices
found` when none is attached:

    $ ubertooth-util -U all -s
    Serial No (0): 1cc001038088cb956c43ad61c52000f5

`-U all` only applies to the read-only queries `-b`, `-v`, `-p` and `-s`.
Combined with any other option (a setter, a mode such as `-x` or `-q`, `-r`,
`-S`, or another read such as `-c`) it is refused before any device is opened:

    error: -U all only supports the read-only queries -b, -v, -p and -s; select one device with -U<index|serial> for other options

Given with no query at all there is nothing to list, and the refusal says so:

    $ ubertooth-util -U all
    error: -U all needs one of the read-only queries -b, -v, -p or -s to list

`-N` is the one option that is not refused: it asks about the USB bus rather
than about a device, so it prints the count and the listing follows it, from
the same enumeration:

    $ ubertooth-util -U all -N -s
    2
    Serial No (0): 1cc001038088cb956c43ad61c52000f5
    Serial No (1): 05f00014e380df95f4436562c32000f5

`-h` and `-V` are not refused either, because they end the command before the
listing is reached; `-V` then reports every attached board itself.

`-U all -N` with no query has nothing to list, so it is refused like `-U all`
on its own: the count is not a per-device query, and a listing is what `-U all`
asks for.

    $ ubertooth-util -U all -N
    error: -U all needs one of the read-only queries -b, -v, -p or -s to list

A selector that names one board is refused for the same reason, in the words
`-U` on its own gets: `-N` asks nothing of the board it selects.

    $ ubertooth-util -U 0 -N
    error: nothing to do: -U '0' selects a device but no option asks anything of it

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

    ubertooth-util -U <repeater> -e

A second Ubertooth (the sender) runs the range test:

    $ ubertooth-util -U <sender> -n
    Starting range test
    waiting for the repeater... (1 s)
    request PA level : 0
    request number   : 0
    reply PA level   : 0
    reply number     : 0

During the range test the sender transmits 16 packets at each of the power
amplifier levels 0 to 7, from the lowest, and then listens for the repeater to
send the same packet back the same way. The repeater answers the first request
it decodes, so each direction is measured by the weakest transmission that got
through it.

Both ends take only a packet whose CRC is good, and both then check that it
belongs to this exchange. The repeater answers requests and not replies, so
two repeaters in range of each other cannot answer each other. The sender
takes only a reply carrying its own serial number -- bytes 2 to 17 of the
payload -- and ignores anything else on the channel — a second pair running a range test, or a stray packet the
radio's CRC happened to accept — rather than ending the run on it. The first
packet it ignores is named once per run on standard error, and the test goes
on:

    warning: range test: a packet that is not this board's reply was ignored (first difference at byte 2)

The run still finishes and reports its result, and the warning does not change
the exit status. `-m` does not report it: only `-n`, which follows the run, can
tell which run a discarded packet belongs to.

`-n` follows the test to its end. While it runs it prints a line about once a
second, so a test that is waiting looks different from one that has finished:

    waiting for the repeater... (1 s)
    waiting for the repeater... (2 s)

and when the reply arrives it prints the result block below and exits 0. The
exchange normally takes two or three seconds: the sender's burst, then a
second during which the repeater holds off so the sender is listening by the
time the answer comes, then the reply burst.

### Reading the result

Each direction has a level and a number. The level is the lowest power the
other end heard, 0 (-25 dBm) to 7 (0 dBm). The number is which of the 16
packets sent at that level was the one that got through, 0 to 15, so a higher
number means more packets at that level were lost before one arrived. Together
they say how much margin the link has: level 0 and number 0 is the strongest
result there is, the very first packet at the lowest power decoded first time,
and a climbing number, then a climbing level, is a link with less and less to
spare. "request" is the sender to the repeater, "reply" the repeater back.

Two boards a short distance apart, both with their antennas on, report the
strongest result in both directions:

    request PA level : 0
    request number   : 0
    reply PA level   : 0
    reply number     : 0

Taking the antenna off one of them costs that link most of its margin, and the
numbers say so while the levels are still 0 — 15 packets lost on the way out
and 4 on the way back before one got through:

    request PA level : 0
    request number   : 15
    reply PA level   : 0
    reply number     : 4

### When nothing answers

The repeater is the only thing that answers a range test, so with none in
range the sender listens and nothing comes back. `-n` waits 10 seconds by
default, then stops the test and says so on standard error, with exit
status 1:

    $ ubertooth-util -U <sender> -n
    Starting range test
    waiting for the repeater... (1 s)
    ...
    error: no reply from the repeater within 10 s: start one in range with ubertooth-util -e, or wait longer with -n <seconds>; the range test has been stopped

`-n <seconds>` chooses the wait, and `-n 0` waits forever, for a repeater
being set up by hand at the other end. Interrupting `-n` with Ctrl-C stops the
test like the other endings do, and says so, with exit status 1:

    error: interrupted before the range test finished; the range test has been stopped

A board that leaves the test without a reply ends the wait at once rather than
waiting out the deadline: `error: the range test was stopped before the
repeater replied`, exit status 1. A board that never entered the test says so
instead of naming the repeater: `error: the board did not start the range test
within <n> s`.

`-n` reads the board twice a poll, ten polls a second, to follow all that.
Three polls in a row that cannot read it end the wait, as `error: range test
result read failed; <what became of the test>` or `error: board mode read
failed; ...`; one or two do not, since a board part-way through a mode stalls
the odd control request and the next poll answers. A board that has stopped
answering altogether therefore ends the command on those reads rather than on
the deadline, which can take a few seconds longer than the wait asked for:
each read has a timeout of its own, and libubertooth reports the transfer
itself on the line before.

Every ending leaves the board out of the test, so the next command finds it
idle: the ones above stop it, and a board that left the test by itself is
already out of it. Each of those messages ends by saying so, and says instead
that `-S` stops it if the stop itself was refused.

### -m, the last result

The result also stays on the board until the next test, so it can be read
again at any time, and that is all `-m` does:

    ubertooth-util -U <sender> -m

Both boards must be running firmware of this tree's API version, which is what
every host tool requires of the board it opens. The range test packets carry no
version of their own, so two boards at the same API version work together
whichever revision each was built from.

Before a range test has completed since the board started, `-m` prints
`no range test result yet` on standard output and exits 0: there is no result
yet, which is not a failure. A test that ran but whose packets came back
corrupted has no result to report either, so it is a diagnostic rather than a
value, from `-n` as from `-m`: `error: invalid range test: mismatch on byte
<n>` on standard error, with exit status 1 and no result line on standard
output. That means this board's own reply came back corrupted, so the levels
it carries cannot be trusted; somebody else's packet is discarded with the
warning above instead. (`-m` writes nothing at all to
standard output there; `-n` has already printed the progress lines of the test
it was watching.)

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
  LEDs go with it, as above, and so does the access code `ubertooth-ego -a`
  sets; the power amplifier level set with `-a` does not.
- **A channel, a band sweep, an access code or an AFH channel map stated after
  the mode request is kept.** It belongs to the mode that is starting, so the
  reset above leaves it alone, whatever the board was doing before. That is
  where a tool states what its capture needs — `ubertooth-rx`,
  `ubertooth-scan`, `ubertooth-btle`, `ubertooth-ducky`, `ubertooth-ego` and
  `ubertooth-follow` all do — and it is kept only for that mode: a setting
  stated for a run the host has moved on from is dropped rather than carried
  into whatever starts next.
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
is running, rather than before it starts. The channel a capture needs goes the
same way round, and for the same reason it is stated after the mode request.

One re-send is a mode start: a second `ubertooth-ego` request against a board
still capturing from a killed first run starts the E-GO mode it names, with the
default access code back and the hopping forgotten, because that request does a
mode start's work itself rather than waiting for a dispatch that is not coming.
So `ubertooth-ego -r` after a killed `-f` receives on the one channel it asked
for, rather than carrying on across the E-GO channel list.

A mode that tunes once reads a late setting a moment later than one that tunes
on every hop: `ubertooth-ego -r` re-tunes when its channel or access code
arrives after the capture has started, which costs the first millisecond of
it.

## EXIT STATUS

`ubertooth-util` exits with 0 when every requested operation succeeds, including
queries that report a nonzero value such as `-b` (board ID), `-c` (channel), or
`-a` (power amplifier level). `-m` with no range test result yet also exits 0
(see [RANGE TEST][]); `-m` on a result whose packets came back corrupted
exits 1. It exits with a nonzero status if any requested operation fails, even
when a later operation in the same command succeeds.

`-n` exits 0 when the range test finished and its result was printed. It
exits 1 when nothing replied within the wait, when the test ended or never
started, when the reply came back corrupted, and when the wait was interrupted
with Ctrl-C (see [RANGE TEST][]). It also exits 1 when three polls in a row
cannot read the board, as `error: range test result read failed; <what became
of the test>` or `error: board mode read failed; ...`.

A command line that asks for no operation at all is refused before any device
is opened, rather than opening the selected board, doing nothing with it and
exiting 0:

    $ ubertooth-util
    error: nothing to do: give an option that reads or sets something, or -N to count the attached boards

    $ ubertooth-util -U 0
    error: nothing to do: -U '0' selects a device but no option asks anything of it

`-U` only says which board the rest of the command line acts on, so it is not
an operation of its own; `-N`, `-h` and `-V` are, and each works on its own.
The usage text follows the `error: ` line on standard error, and the exit
status is 1.

`-h` and `-V` both end the command and both answer on standard output, so a
command line with both prints both, the version first and the usage text
last; the order they were written in makes no difference. The first one used
to win and the other was dropped.

`-c` and `-C` are the same channel setting written two ways, so a command line
that gives a value to both names two channels for one setting and is refused
before any device is opened: `error: -C cannot be combined with -c; they are
the same channel setting written two ways`. The second one used to be the one
sent. A value written one way and a read written the other is still one
setting and one read, so `-C 24 -c` sets the channel and then reports it.

It exits with 1, without opening a device, when two or more mode options are
given (see DESCRIPTION), when `-z` is combined with a mode option, or when an
`-a` level is not a single digit 0-7
(`error: PA level must be a single digit 0-7 (got <value>)`). The same holds
for any other option value that is not a whole decimal number in its range
(`-c`, `-C`, `-d`, `-l`, `-n`, `-q`, `-z`), for example
`error: channel must be 0-78 (got abc)` or
`error: timeout must be a whole number of seconds, 0 for none (got abc)`.
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

`-w` makes two reads, and a failure in either prints
`error: watchdog state read failed` or
`error: radio wait timeouts read failed` on standard error, after any message
from libubertooth, and makes the command exit 1. The other read is still
reported, so a board that cannot answer one still says what it can.

When a `-b`, `-c`, `-a`, `-z`, `-l`, `-d`, `-p`, `-s` or `-m` read fails,
no value line is printed on standard output, so an error code is never shown as
a value; the other requested queries are still reported. A failed `-b`, `-c`,
`-a`, `-z`, `-l`, `-d`, `-p`, `-s` or `-m` read also prints
`error: <query> read failed` (for example `error: channel read failed` or
`error: board ID read failed`) on standard error, after any message from
libubertooth. When the board returns fewer bytes than the read needs, that is
the only line, with the counts added, for example
`error: channel read failed: board returned 0 of 2 bytes`.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## DIAGNOSTICS

Standard output carries only what was asked for: the values the queries read,
the device listing, the range test `-n` runs, and the usage text for `-h`.
Everything else goes to standard error, so a script reading a value is not fed
anything else.

`-n` is the one operation whose running commentary is what it was asked for
rather than a note about itself: the test is what the command is, so its
`waiting for the repeater... (<n> s)` lines go to standard output with the
result they lead to, and a script reading `-n` reads those lines as well as
the four result lines `-m` gives. `Starting range test`, which is about the
command and not about the test, stays on standard error with the lines below.

That includes the progress lines for the operations that do something rather
than report something: `Resetting ubertooth device <selector>`,
`Stopping ubertooth device <selector>`,
`Entering flash programming (ISP) mode`,
`Entering LED specan mode (threshold -<n> dBm)`, `Starting range test`,
`Starting repeater` and `Starting TX test`. They were written to standard
output before.

`-i` adds a second line, because the first does not say what the board will
do: `note: the board hands itself to its on-chip programmer and
answers nothing else until it is power-cycled`.

Each operation says what it did once. `-I` reports the blink only through
`note: the board is blinking; stop it with ubertooth-util [-U <selector>] -S
or any mode option`, naming the selector so the command can be pasted; there
is no separate line announcing the LEDs. Setting the squelch with `-z <n>`
prints nothing, as `-a`, `-c`, `-C`, `-d` and `-l` do not: the value `-z`
reads back afterwards (`Squelch set to <n>`) is the result and goes to
standard output. A set that fails is still reported, on standard error, and
the command exits 1. `-x` has never had a line of its own.

The errors this tool prints itself are one line starting `error: `; its
warnings and notes start `warning: ` and `note: `. libubertooth's lines read
the same way, for example `error: Ubertooth <serial> is in use by another
program (libusb -3)` and `error: no Ubertooth devices found`; see
ubertooth(7), WHEN A BOARD CANNOT BE USED. A refusal can run to more than
one line: the boards listed after `error: multiple Ubertooth devices found;
select one with -U <index|serial>`, and the counts and reason after a serial
that matched nothing.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-util(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
