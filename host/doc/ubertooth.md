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
 - ubertooth-btbr(1) : Experimental Classic Bluetooth connections and
   monitoring; needs the separate `btbr` firmware image

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
a file option means standard output, so `ubertooth-btle -q -` writes the
capture to stdout. No option reads its input from a lone `-`: for those it is
a file name like any other, and there is no such file. Any other value is a
file name too, including one that starts with `-`, so a forgotten file name
writes a file rather than guessing. A destination whose writer seeks back over
what it has written cannot be `-` at all, which a pipe does not allow: the
PcapNG captures (`-r`) and `ubertooth-dfu -u` refuse it and say what to use
instead. Standard output carries one stream, so two capture options cannot
both have it, and the tool whose capture has it prints none of its own packet
report there.

Options can be written together behind one dash (`-f -n` is `-fn`), so a stray
dash inside such a word is a letter the tools do not have. The refusal names
the word it was written in rather than the dash on its own:
`ubertooth-util -v-` and `ubertooth-util -r-x` both read
`error: unrecognized option '-' in '<word>'`. The trailing form used to be
blamed on `--`, or on the option after it (`ubertooth-util -v- -h` named
`-h`), neither of which the user had written. A command line with more than
one word holding a dash names one of them.

An option may be given a value only once. `-l 446688 -l aabbcc` names two
LAPs for one capture, so it is refused with `error: option '-l' given more
than once` before any board is opened, rather than one of them being kept and
the other dropped without a word. Only a value counts: repeating an option
that takes none says the same thing twice, and an option whose value is
optional has two jobs, so `ubertooth-util -c 2426 -c` sets the channel and
then reads it back. A tool may still refuse a repeat of its own where the
second one is more likely a typo than a request, and says so on its page:
`ubertooth-specan -v` is the one that does.

The rule covers every `ubertooth-*` tool. `btle-extcap.py` and
`ubertooth-specan-ui` take their command lines from Wireshark's extcap
protocol and from `argparse`, and still keep the last value.

For the same reason, two options that cannot both run are refused with one
`error: ` line: two major modes, two modulations for one capture, two output
formats for one sweep, or the same setting written two ways
(`ubertooth-util -c 2440 -C 24`). Each page lists its own.

## SELECTING A DEVICE

With a single Ubertooth attached, the tools find it automatically. With more
than one attached, every tool takes `-U <index|serial>` to choose one: an
index is a position in the list of attached Ubertooths (`-U 0` is the first),
and a serial-number string selects that board whatever the enumeration order.
An index that is not attached is refused (`error: no Ubertooth at index 2 (2
attached)`) and the tool exits non-zero. Indexes are positions in the list of
boards attached when the command runs, so they shift when a board is added or
removed; to keep targeting the same board, select it by serial.

With several attached and no `-U`, no board is selected: the tools refuse the
command and then list the boards, so the next command can name one. Reading
those serials opens each board briefly, and none is kept open.

    error: multiple Ubertooth devices found; select one with -U <index|serial>
      Device 0: 1cc001038088cb956c43ad61c52000f5
      Device 1: 05f00014e380df95f4436562c32000f5

With none attached, every tool says `error: no Ubertooth devices found`.

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
`ubertooth-util -r` to reset the board; a board that answers neither usually
resets itself, as the next paragraphs describe. The line says
only what happened, because what it means depends on what the command was
doing: mid-flash, for instance, it is a bootloader that lost its place, and no
mode is involved.

A board running this tree's firmware usually recovers itself: it resets about
three seconds after it stops servicing USB, drops off the bus and comes back
idle, so the fix is to run the command again. `ubertooth-util [-U <selector>]
-w` then says how often that has happened and which mode and which wait the
board was in the last time.

Not every board gets that self-reset, and where it does not the fallback is
always the same: unplug the board or power-cycle its hub port. That is what the
`note: ` line the tools print after the error names (see ubertooth-rx(1),
DIAGNOSTICS). Firmware without the watchdog, such as `2020-12-R1`, has
no self-reset at all. Neither has anything that feeds the watchdog from its
loop rather than from a USB service pass — the six modes that service USB from
the interrupt, and the `btbr` image — because that loop keeps turning when the
USB path alone has died (see ubertooth-util(1), WATCHDOG). The bootloader runs
with the watchdog off by design, so a `-7` mid-flash, where no mode is
involved, waits for nothing either; ubertooth-dfu(1) reports that one in its
own words, and its page says the same about a board that answers nothing.

A serial-number selector names the reason, because a board it could not read
may be the board asked for:

    error: no Ubertooth found with serial matching '1cc'
    1 attached Ubertooth device did not answer the serial read
    note: a board that does not answer may be in use by another program, or busy in a mode; stop a mode with ubertooth-util -U <index> -S, or reset a board that answers nothing with -r

The remedy is by index there because a board whose serial could not be read
cannot be selected by serial. `-r` on a board that answers nothing resets its
USB connection before it asks the board to reset itself, because the board
would refuse that request as it refuses every other one; the connection reset
is what recovers it, and the board is then reset like any other. See
ubertooth-util(1).

A board in the DFU bootloader is named too, because it is not a board that
went away: while it is being flashed it enumerates as the bootloader, which no
tool but ubertooth-dfu(1) can open, so every selection finds nothing until the
flash finishes. An interrupted flash leaves it there until the next one
succeeds.

    error: no Ubertooth found with serial matching '1cc'
    note: a board is in the DFU bootloader, so no tool can open it: it is being flashed, or a flash left it there

A board that cannot be *opened* at all is a different problem, usually file
permissions: on Linux, install the udev rules shipped with this package (see
ubertooth-util(1), SELECTING A DEVICE).

## FIRMWARE OF ANOTHER API VERSION

These tools drive the firmware built from the same source tree, which both
run-time images report as their USB device version. A release build refuses
anything else, naming both versions and which side to replace:

    error: Ubertooth firmware API version <version> found, libubertooth 1.1 requires <version>.
    note: flash the firmware built from the same source tree, for example: ./install.sh --flash-device 1cc0

A build made from a git checkout that is not on a tag is a development build,
and says the same thing as a warning and carries on, because host tools and
firmware from different commits are the normal state of lab work and
debugging:

    warning: Ubertooth firmware API version <version> found, libubertooth 1.1 requires <version>.
    note: flash the firmware built from the same source tree, for example: ./install.sh --flash-device 1cc0
    note: this is a development build, so the command runs on; what it reports may be wrong, and a request this firmware has not got will fail

Both notes are worth reading before the output is trusted. What the two
versions disagree about may be the layout of a record, in which case the
capture is decoded by the wrong rules and is wrong rather than empty. Where
the firmware is simply missing a request, the command fails on it:

    error: could not set the channel: the board's firmware does not support it (stalled, libusb -9)
    note: the board reports firmware API version <version>, libubertooth 1.1 requires <version>

That is the one wording for a request the board refuses, wherever the request
came from. The board answers such a request by stalling it and says nothing
else, so the line names the request that was refused, and the note puts the
two versions beside each other: different versions name the reflash that
fixes it, and equal versions say the version is not the problem. The libusb
code is there for a request sent and answered one at a time; a request sent
without waiting for its answer has only the stall to report, so that line ends
`(stalled)`. A board in the DFU bootloader runs no application firmware, so
ubertooth-dfu(1) reports a refused request without the note.

A board that has left the USB bus stalls a request too, on some hosts, and its
firmware is not the reason: the board whose version the note would quote is
not there any more. Before the firmware is blamed the bus is listed, and a
board that has gone from it gets one line of its own, with no note:

    error: could not stop the running mode: the board is no longer attached

Nothing is asked of the board to find that out, so a board that is merely
silent is left alone. A bus that cannot be listed says nothing either way, and
the request is reported as the stall it arrived as. So is one sent to a board
whose version was never recorded, which is ubertooth-dfu(1) talking to the
bootloader: there is no handle there to ask the bus about. Every board these
tools open through libubertooth is recorded as it is opened, so every board a
mode runs on is covered.

Which answer a build gives can be set either way with
`UBERTOOTH_ALLOW_API_MISMATCH` (see ENVIRONMENT below).

The version is only half of it: this tree builds two run-time images, and they
report the same version. So the tools that check the version check the image
as well, as the board is opened, and one carrying the other image is refused
by name with the line that flashes it:

    error: the Ubertooth is running btbr; this tool needs bluetooth_rxtx
    note: flash it with ubertooth-dfu -U 1cc0 -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r

`btbr` answers the requests both images share and nothing else, so without
that the command failed on the first request it does not answer, reported as a
refused request — true, but about the wrong thing. ubertooth-btbr(1) is the
tool that drives `btbr`, and makes no such check: a board carrying
`bluetooth_rxtx` reaches its session and is reported from the write that times
out there. ubertooth-util(1) and ubertooth-dfu(1) check neither version nor
image, because both have to work on whatever firmware a board is carrying, and
ubertooth-debug(1) checks neither yet.

A board that will not say which image it runs is left to the request that
needs it. Two boards answer nothing there: one too busy to take the read, and
one running firmware older than the info block, which a development build
reaches because it reports the version mismatch and runs on. Both are boards
somebody means to use, and neither can be told from the other, so neither is
refused.

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
and may still be capturing`, asks that board for nothing further, and exits 1:
the capture lost its stream, so it holds less than was asked for. The
board usually needs `ubertooth-util [-U <selector>] -S` or `-r` afterwards, and
one that answers neither usually resets itself about three seconds later and
comes back idle.

A capture that is losing packets says so, whichever way it reads them. A
receive transfer that arrived with part of a packet cannot be read as a
record, so it is dropped, and the capture reports it once per run, because
such a fault usually repeats: `warning: a receive transfer arrived with 32 of
a packet's 64 bytes and was dropped; the capture is losing packets`. That is
the path `ubertooth-dump`, `ubertooth-rx`, `ubertooth-afh`,
`ubertooth-follow` and `ubertooth-specan` read through. `ubertooth-btle` and
`ubertooth-ego` poll for their packets one at a time instead, and report the
same fault in the same way: `warning: a poll reply arrived with 32 of a
packet's 64 bytes and was dropped; the capture is losing packets`. The short
replies a board sends on purpose are not reported: the keep-alive a quiet
streaming capture gets, a single byte, and the one-byte "nothing queued"
answer every poll of a quiet capture gets. The capture runs on and ends as it
otherwise would, so the warning is the only sign: it says packets were lost,
it does not fail the run.

libubertooth follows the split already: it returns what it reads to the tool
that called it and reports its own problems on standard error, so a library
message does not land in the middle of a capture. The clock trim and AFH
channel notes raised while following a piconet are `note: ` lines on standard
error. What the library still writes to standard output is the calling tool's
result — the decoded packet reports, the AFH map, the version line — which is
the right stream for them, whichever layer prints them. Its device-selection
messages are `error: ` lines on standard error, each with whatever detail it
needs under it: the boards listed after a refusal for several attached, and
the counts and reason after a serial that matched nothing.

## ENVIRONMENT

`UBERTOOTH_ALLOW_API_MISMATCH` decides what a tool does about firmware of
another API version, whatever the build it came from. `1` warns and runs the
command, which is how a release build is made to talk to firmware from
another commit for debugging; `0` refuses it, which is how a development
build is made strict, for a test harness that wants a refusal rather than a
warning. Only those two exact values decide: unset, empty or anything else leaves the
build's own answer, so a typo cannot turn the guard off. See FIRMWARE OF
ANOTHER API VERSION above.

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
