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
 - ubertooth-scan(1) : Active (BlueZ) scanning of undiscoverable devices; Linux only
 - ubertooth-afh(1) : Detecting the AFH map of a Classic Bluetooth piconet
 - ubertooth-ego(1) : Yuneec E-Go electric skateboard sniffing
 - ubertooth-follow(1) : Active (BlueZ) clock discovery and piconet following;
   Linux only
 - ubertooth-btbr(1) : Experimental Classic Bluetooth connections and
   monitoring; needs the separate `btbr` firmware image

Utility commands:

 - ubertooth-dfu(1) : Firmware update tool
 - ubertooth-dump(1) : Dumping raw RF symbols to disk, Bluetooth or generic
   (experimental)
 - ubertooth-tx(1) : Retransmitting symbols dumped with ubertooth-dump
 - ubertooth-ducky(1) : Driving an Uberducky as a BLE USB Rubber Ducky
 - ubertooth-util(1) : "Everything else"

Less useful commands:

 - ubertooth-debug(1) : Reading registers on the CC2400, and writing them
   (experimental)
 - ubertooth-specan(1) : Raw RSSI values used by graphical specan

## COMMON OPTIONS

`-h` and `-V` mean the same thing in every tool, including the three written
in Python -- ubertooth-btbr(1), `btle-extcap.py` and ubertooth-specan-ui(1) --
which take their command lines from getopt in Python, from Wireshark's extcap
protocol and from `argparse`. Those three answer `-V` by running
ubertooth-util(1) `-V` and passing its answer on, so the lines below are the
same words whichever tool was asked; `ubertooth-util` has to be on their
`PATH`, as it is when the tools are installed together, and one that is not
there is reported as `error: could not run ubertooth-util: No such file or
directory`. `btle-extcap.py` has no `-U`, so its `-V` reports every attached
board. ubertooth-specan-ui(1) answers `-V` and `-h` without the `numpy` and
Qt binding its window needs, and ubertooth-btbr(1) answers them without the
Python `libusb1` package a session of its own needs. `-v` is not in every tool; the last paragraph here says what it means
where it is.

`-h` prints that tool's usage text on standard output and exits 0.

`-V` prints the version on standard output, and is the way to ask what is
installed and what a board is running. It exits 0, except for the two
failures below: a board that is attached and cannot be read, and a USB bus
that cannot be listed at all. Every tool answers it in the same words, one
labelled line each, so a script can take the line it wants whichever tool it
ran:

    Host tools version: libubertooth <version> (<revision>), libbtbb <version> (<revision>)
    libbtbb loaded from: <path>
    Firmware version: <image> <revision> (API:<version>)
    Firmware build: <build string the board reports>

The first two lines are the host's and need no board: the versions these tools
were built from, and which libbtbb this run loaded, which is what tells a
distribution or Homebrew build apart from this fork's.

The two firmware lines describe the board `-U` names, or the only board
attached. With several boards attached and no `-U`, both lines are printed for
each of them with its index in the label, as the identity queries of
ubertooth-util(1) are: `Firmware version (0): ...`, `Firmware build (0): ...`.

With no Ubertooth attached the host lines are followed by `Firmware version:
no Ubertooth attached` and the status is still 0: the host half of the answer
needs no board, and nothing attached is an answer rather than a failure. A
board sitting in the DFU bootloader is named in a `note:` on standard error
there, because no tool can open one and that is the usual reason a board is
not in the answer. A
board that is attached but cannot be read -- in use by another program, or not
answering -- has the reason on its own line (`Firmware version: unavailable,
in use by another program`, or `Firmware version: read failed`) and the status
is 1. So does a USB bus that cannot be listed at all, which is nothing known
rather than nothing attached: `Firmware version: unavailable, the USB bus
could not be listed`.

Part of an answer can go missing on its own, and the status is 1 then too: a
board that answered its version but not its build string prints `Firmware
build: read failed` under a good version line, or `Firmware build: not
reported` if the read worked and the board had no build string to give, and
one whose API version could not be read prints the version with no
` (API:...)` after it.
So a script that cares about the whole answer should test the status, not
only look for a reason in place of a version.

Asking for the version does not disturb a board: it is not told to stop, so a
mode running on it keeps running, and any firmware is read whatever API
version it reports, which is what makes `-V` the way to identify an unknown
board.

`-v` means verbose, where a tool has verbose output: ubertooth-specan(1) `-v`,
ubertooth-debug(1) `-v <0-2>`, and ubertooth-btle(1) `-v`, which describes each
packet whose data the decoder could not read. One tool spells something else
`-v`, an older spelling kept so that existing scripts keep working:
ubertooth-util(1) `-v` prints the `Firmware version:` line on its own
(deprecated, and unchanged: use `-V`). ubertooth-btle(1) `-v` named that tool's
CRC verify setting until `-k` took it over, so a value written after it is
refused with a line naming `-k`.

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
while `-z -90` sets the squelch to -90 dBm. The `ubertooth-*` tools written in
C take single-letter options only, and refuse a long one with `error:
unrecognized option '--bogus': the tools take single-letter options`, and so
does ubertooth-btbr(1). The long options of the two tools that have them --
`btle-extcap.py`, whose are Wireshark's, and ubertooth-specan-ui(1) `--wifi`
and `--padding` -- take `--name value` or `--name=value`, never attached
without the `=`. A lone `-` as the value of
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
`-h`), neither of which the user had written. Where several words hold a dash,
the one named is the word the parse stopped in, which is the first word the
tools would not take: `ubertooth-util -v- -Ufoo-` names `-v-`, because the
dash in `-Ufoo-` is part of `-U`'s value and nothing in that word is wrong.

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
      Device 0: aa1100112233445566778899aabbccdd
      Device 1: 19bb00112233445566778899aabbccdd

With none attached, every tool says `error: no Ubertooth devices found`.

Selecting by serial number is the slower form, because every attached board is
opened and asked for its serial, and a board that does not answer costs a
second. A Ctrl-C during that search ends the run: the search gives up where it
is, no board is selected, and the tool exits 0 without a message of its own,
because the user stopped a run that had not yet asked anything of a board. A
Ctrl-C between the board being opened and the first request made of it ends the
run the same way. A search that did finish and matched nothing still says so
and exits 1, whether or not a signal arrived while it ran: the answer is true
either way, and a script reading the status to learn whether its board is there
must not be told that it is.

SIGTERM, SIGHUP, SIGQUIT and an alarm (SIGALRM, as `timeout` and `kill` send
them) end a run the same way Ctrl-C does, which means exit 0 rather than death
by the signal. SIGHUP is caught even when it was inherited ignored, so a
capture run under `nohup` stops when the terminal closes instead of carrying on
with its mode left running on the board.

A serial may be shortened to its first few characters, in either case, so
`-U aa11` and `-U AA11` select the same board. If those characters start more
than one attached board's serial, the tool lists the matching serials and
selects neither. An all-digits string is read as an index, so include a
non-digit (`-U 19b` rather than `-U 19`). See ubertooth-util(1), SELECTING A
DEVICE, for the full description and for listing the attached devices.

## WHEN A BOARD CANNOT BE USED

A board can be attached and still be unavailable, because only one program at
a time can use it and because a mode keeps running on the board until it is
stopped. Both cases are reported in words, with the libusb code in parentheses
for a bug report.

The board opened, and then something else turned out to be holding it:

    error: Ubertooth 19bb00112233445566778899aabbccdd is in use by another program (libusb -3)

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

A board still answering, left in a mode by a run that was killed, needs no
`-S` before most captures. `ubertooth-rx`, `ubertooth-dump`, `ubertooth-scan`,
`ubertooth-follow`, `ubertooth-afh` and `ubertooth-specan` read which mode the
board is in as they start, stop a mode they find and wait for the board to
report itself idle before they read. They say so, because the capture they
stopped was somebody's:

    warning: the board is still in mode 9 from an earlier run; stopping it first (ubertooth-util -w names the modes)

Without that wait, the stopped mode could hand the board's receive endpoint
one more packet before the new capture's own mode had started, and the capture
would open with a packet or two of the earlier run's. A board that does not
reach idle within about a second is a second warning, and the capture goes
ahead.

`ubertooth-btle` and `ubertooth-ego` read their packets a different way — one
request per packet rather than the stream endpoint — so neither does this, and
a board left in a mode is still theirs to meet: stop it with
`ubertooth-util -S` first.

A board running this tree's firmware usually recovers itself: it resets about
three seconds after it stops servicing USB, drops off the bus and comes back
idle, so the fix is to run the command again. `ubertooth-util [-U <selector>]
-w` then says how often that has happened and which mode and which wait the
board was in the last time.

Not every board gets that self-reset, and where it does not the fallback is
always the same: unplug the board or power-cycle its hub port. That is what the
`note: ` line the tools print after the error names (see ubertooth-rx(1),
EXIT STATUS). Firmware without the watchdog, such as `2020-12-R1`, has
no self-reset at all. Neither has anything that feeds the watchdog from its
loop rather than from a USB service pass — the six modes that service USB from
the interrupt, and the `btbr` image — because that loop keeps turning when the
USB path alone has died (see ubertooth-util(1), WATCHDOG). The bootloader runs
with the watchdog off by design, so a `-7` mid-flash, where no mode is
involved, waits for nothing either; ubertooth-dfu(1) reports that one in its
own words, and its page says the same about a board that answers nothing.

A serial-number selector names the reason, because a board it could not read
may be the board asked for:

    error: no Ubertooth found with serial matching 'aa1'
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

    error: no Ubertooth found with serial matching 'aa1'
    note: a board is in the DFU bootloader, so no tool can open it: it is being flashed, or a flash left it there

A board that cannot be *opened* at all is a different problem, usually file
permissions: on Linux, install the udev rules shipped with this package (see
ubertooth-util(1), SELECTING A DEVICE).

## FIRMWARE OF ANOTHER API VERSION

These tools drive the firmware built from the same source tree, which both
run-time images report as their USB device version. A release build refuses
anything else, naming both versions and which side to replace:

    error: Ubertooth firmware API version <version> found, libubertooth 1.1 requires <version>.
    note: flash the firmware built from the same source tree, for example: ./install.sh --flash-device aa11

A build made from a git checkout that is not on a tag is a development build,
and says the same thing as a warning and carries on, because host tools and
firmware from different commits are the normal state of lab work and
debugging:

    warning: Ubertooth firmware API version <version> found, libubertooth 1.1 requires <version>.
    note: flash the firmware built from the same source tree, for example: ./install.sh --flash-device aa11
    note: this is a development build, so the command runs on; what it reports may be wrong, and a request this firmware has not got will fail

Both notes are worth reading before the output is trusted. What the two
versions disagree about may be the layout of a record, in which case the
capture is decoded by the wrong rules and is wrong rather than empty. Where
the firmware is simply missing a request, the command fails on it:

    error: could not set the channel: the board's firmware does not support it (stalled, libusb -9)
    note: the board reports firmware API version <version>, libubertooth 1.1 requires <version>
    note: a board that stalls every request is wedged rather than short of this one; reset it with ubertooth-util -r on the same board

That is the one wording for a request the board refuses, wherever the request
came from. The board answers such a request by stalling it and says nothing
else, so the line names the request that was refused, and the first note puts
the two versions beside each other: different versions name the reflash that
fixes it, and equal versions say the version is not the problem. The libusb
code is there for a request sent and answered one at a time; a request sent
without waiting for its answer has only the stall to report, so that line ends
`(stalled)`. A board in the DFU bootloader runs no application firmware, so
ubertooth-dfu(1) reports a refused request without either note.

The second note is for the other reason a request stalls, which the first
cannot be told apart from: a wedged board stalls everything sent to it,
whatever its firmware implements, so a report naming the firmware would send
the user to reflash a board that is not short of the request at all. The
question that separates the two is whether any request gets through, and
`ubertooth-util -r` is the recovery for the wedge: it resets the board's USB
connection and then the board, with no one at the bench.

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
    note: flash it with ubertooth-dfu -U aa11 -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r

`btbr` answers the requests both images share and nothing else, so without
that the command failed on the first request it does not answer, reported as a
refused request — true, but about the wrong thing. ubertooth-btbr(1) is the
tool that drives `btbr`, and makes no such check: a board carrying
`bluetooth_rxtx` reaches its session and is reported from the write that times
out there. ubertooth-util(1) and ubertooth-dfu(1) check neither version nor
image, because both have to work on whatever firmware a board is carrying.
ubertooth-debug(1) checks the image and not the version: reading CC2400
registers is debugging work on whatever revision a board carries, but the
other image answers no register read, so that board is named rather than left
to fail on the read.

A board that will not say which image it runs is left to the request that
needs it. Two boards answer nothing there: one too busy to take the read, and
one running firmware older than the info block, which a development build
reaches because it reports the version mismatch and runs on. Both are boards
somebody means to use, and neither can be told from the other, so neither is
refused.

## GENERIC RADIO MODE

Experimental. The symbol-stream half works: `ubertooth-dump -g` with a
configuration written by hand streams as the Bluetooth captures do. The
packet-framing half does not yet produce records on our bench, and the
deviation the board transmits a generic packet at is unsettled, so no tool
transmits one. Expect the options and the output to change.

The CC2400 on an Ubertooth is a plain 2.4 GHz radio, and Bluetooth is only
what the firmware usually asks it to do. The generic receive hands that choice
to you: it programs no demodulator of its own and receives with whatever
configuration is already in the radio's registers. So a 2.4 GHz protocol that
is not Bluetooth can be captured without new firmware, as long as the CC2400
can be set up for it.

Two commands do it. `ubertooth-debug -w` writes the registers, and
`ubertooth-dump -g` starts the capture:

    ubertooth-debug -w %manand=0x7fff,%lmtst=0x2b22,%mdmtst0=0x134b,%grmdm=0x0101,%fsdiv=2401,%mdmctrl=0x0029
    ubertooth-dump -g -b -d symbols.txt

One `-w` takes the whole list, so the registers go in one of them.

That example is Bluetooth Basic Rate, as a case you can check against
`ubertooth-dump -c`: GFSK at 160 kHz deviation (MDMCTRL `0x0029`) on 2402 MHz
(FSDIV is the frequency in MHz less the 1 MHz intermediate frequency), with
GRMDM `0x0101` for un-buffered mode and no packet framing. The board streams
demodulated symbols and `-b` prints them as `0` and `1`.

The registers that matter, and what each decides:

 - MDMCTRL : the frequency deviation, which is the data rate the demodulator
   expects. `0x0029` is 160 kHz, `0x0040` is 250 kHz.
 - FSDIV : the channel, as the frequency in MHz less 1 for the intermediate
   frequency.
 - GRMDM : buffered or un-buffered, packet mode or not, how many preamble
   bytes, how much of the sync word to match, and whether the radio checks a
   CRC. Its packet-mode bit is what the board reads to choose between the two
   receives below.
 - SYNCH and SYNCL : the sync word, in packet mode.
 - MANAND, LMTST and MDMTST0 : the analog and test settings the Bluetooth
   receives use; a protocol at a different rate may want others.

`ubertooth-debug -r` reads any of them back, and the CC2400 datasheet is where
the fields come from. Nothing in these tools knows which values make sense
together: a configuration that does not work gives a capture with nothing in
it, not an error.

With GRMDM's packet-mode bit set, the board waits for the sync word instead,
and sends one record each time the radio reports one: the four sync-word
bytes, then a fixed 32 bytes read out of the radio's FIFO. It reads no length
and waits for no end of packet, so a record is a sample of the FIFO and not
necessarily one whole packet. It keeps waiting on a quiet channel, answering
requests, until the capture is stopped.

That path asks more of the configuration than the symbol stream does, because
the radio does the framing: the preamble length, how much of the sync word it
matches and its CRC choice all have to match what is on the air, and a choice
that does not gives a capture with no records in it rather than a message.
This is the half that is not working yet, so treat it as something to
experiment with rather than to rely on.

The configuration lives in the radio and nothing stores it, so write the
registers and start the capture as two commands with nothing in between. A
mode ending takes the radio through its own reset, which clears what `-w`
wrote; the board being idle is what keeps it. `ubertooth-util -S` leaves an
idle board, so a run that is stopped properly can be set up again and
restarted.

The board can also transmit one packet with a host-written configuration,
through a request no tool drives yet: the deviation it transmits at is still
being settled, and as it stands the board's own receive cannot demodulate it.

## OUTPUT AND DIAGNOSTICS

Every tool follows this split. Each tool's own page gives the lines it prints
and the statuses it exits with, under DIAGNOSTICS, EXIT STATUS, or both.

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
the path `ubertooth-dump`, `ubertooth-rx`, `ubertooth-scan`, `ubertooth-afh`,
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
result — the decoded packet reports, the AFH map of `ubertooth-afh`, the
version line — which is the right stream for them, whichever layer prints
them. libbtbb writes there too, while it recovers a piconet's address and
about a packet it could not use. The channel map the two BlueZ tools report is
the exception: those tools print it themselves, so no library writes it. Its device-selection
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
