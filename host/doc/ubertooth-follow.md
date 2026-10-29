# ubertooth-follow(1) -- active (BlueZ) CLK discovery and follow for a target address

## SYNOPSIS

    ubertooth-follow [-t BD_ADDR] [-l LAP] [-u UAP] [-U <index|serial>]
    [-r file] [-q file] [-d file] [-e errors] [-a] [-b hciN] [-w delay]

## DESCRIPTION

Follows a classic Bluetooth (BR/EDR) piconet for a given target address.
It uses a local BlueZ Bluetooth device to assist with clock discovery and an
Ubertooth to follow the piconet's frequency hopping.

Name the target one of two ways:

 - `-t` is its whole address, `xx:xx:xx:xx:xx:xx`, in either case of hex
   digit. That is what the tool connects to, and it gives the LAP and UAP the
   board follows, so `-t` needs neither `-l` nor `-u`. Use `-t` for a real
   target: it is the only form `-a` works with, and the only one that reaches
   an address not beginning `00:00` (see AFH below).
 - `-l` and `-u` together are the lower and upper address parts, which is what
   `ubertooth-rx` reports for a piconet it has seen. They are four of the six
   bytes of an address; the two missing ones are the non-significant address
   part, which a sniffer never sees, and the tool fills them in as zero. So
   the connection it makes to collect the clock offset only reaches a target
   whose address begins `00:00`.

One of `-l` and `-u` without the other names no piconet, so it is refused and
the missing half is named: `error: -l <LAP> requires -u <UAP>`. It used to be
taken as no target at all, which read the address from the local Bluetooth
device and followed that instead of what was asked for.

`-t` with `-l` or `-u` is refused: the two are the same setting written two
ways, and the tool does not reconcile them. With no target at all, the address
is read from the local Bluetooth device — note this may yield the wrong
address in some cases, so naming the target explicitly is recommended.

The capture is gated by the board's squelch, which this tool does not set: it
is the default -120 dBm unless `ubertooth-util -z` set another value while the
board was idle, and the firmware resets it on every mode change, so it never
carries over from an earlier capture. See ubertooth-util(1), SQUELCH.

Requires BlueZ (Linux only).

## OPTIONS

 - `-t <BD_ADDR>` :
   the target's whole address, `xx:xx:xx:xx:xx:xx`, which gives the LAP and
   UAP to follow as well as the address to connect to. Not with `-l` or `-u`,
   and given once.
 - `-l <LAP>` :
   target LAP (3 bytes / 6 hex digits), given once, and with `-u`. Not with
   `-t`.
 - `-u <UAP>` :
   target UAP (1 byte / 2 hex digits), given once, and with `-l`. Not with
   `-t`.
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-V` :
   print the version: the host tools, then the firmware version and build of
   each attached board; see ubertooth(7), COMMON OPTIONS
 - `-r <filename>` :
   capture packets to a PCAPNG file. A PcapNG writer seeks back over what it
   has written, which standard output cannot do, so `-r -` is refused:
   `error: PCAPNG capture to stdout is not supported; use -q -`
 - `-q <filename>` :
   capture packets to a PCAP file. `-q -` writes the capture to standard
   output; see CAPTURING TO STANDARD OUTPUT
 - `-d <filename>` :
   dump received symbols to the named file. `-d -` writes them to standard
   output; see CAPTURING TO STANDARD OUTPUT
 - `-e <0-5>` :
   maximum access-code errors to tolerate (default: 2).
 - `-a` :
   enable AFH (adaptive frequency hopping); experimental, and needs `-t`. See
   AFH below.
 - `-b <hciN>` :
   local Bluetooth device to use (default: hci0).
 - `-w <timeslots>` :
   USB delay in 625us timeslots (default: 5).
 - `-h` :
   show help.

## AFH

`-a` is experimental. It makes the local BlueZ device connect to the target and
asks the local controller for the AFH channel map of *that* link, then follows
the piconet keyed on the target's LAP/UAP. The two are the same hop sequence
only when the target is master of the piconet you want to follow, which
`ubertooth-follow` cannot check. Treat a capture made with `-a` accordingly.

Because the map is read over a connection to the target, `-a` needs the whole
address, which only `-t` gives. `-a` without `-t` is refused before any file is
created or device opened, with
`error: -a reads the AFH map over a Bluetooth connection to the target, so it needs the whole target address: give -t xx:xx:xx:xx:xx:xx`.

A follow keyed on `-l` and `-u` instead connects to `00:00:<UAP>:<LAP>`, with
the two most significant bytes zero, because a sniffer cannot see them. That
reaches only a target whose address begins `00:00`; for any other target the
connection fails and the command stops, saying what to do about it:

    error: connecting to 00:00:33:44:55:66 failed: Host is down; -l and -u give four of the six bytes of the target address, so 00:00 is a guess: give the whole target address with -t xx:xx:xx:xx:xx:xx

Give the target as `-t` and the connection is made to the address as it is, so
that failure means the target itself did not answer; the line is then the
reason alone, without the advice. A page that fails without leaving a reason
behind drops the `: <reason>` from whichever of the two lines it is.

The map that comes back is used only when it could be a hop sequence at all:

 - a failed read stops the command (`error: AFH map read failed: ...`), rather
   than following with whatever the map happened to contain. `-a` therefore
   does not work on a controller that does not implement
   HCI_Read_AFH_Channel_Map; run without `-a` on one of those;
 - a reply of AFH off is followed without it:
   `the remote device is not using AFH; following without it`. Nothing is sent
   to the board in that case: starting the follow resets its hop mode and AFH
   map, so a map left by an earlier run cannot make it hop adaptively anyway.
   An accepted map is applied once the follow has started, for the same reason
   (see ubertooth-util(1), CARRY-OVER BETWEEN RUNS);
 - a map with fewer than the 20 channels AFH requires, or none at all, is read
   again — three reads in total, 200ms apart — in case it has not settled. If
   it still reports too few, the command stops and says so;
 - a mode byte that is neither off nor on is refused and printed.

Nothing is set on the Ubertooth until the map has been accepted, so a refused
`-a` leaves the board alone, and the connection the tool made to the target is
dropped on the way out.

## EXAMPLES

Follow a target whose address you know, writing a PCAP:

    ubertooth-follow -t 11:22:33:44:55:66 -q capture.pcap

Follow the same target with an AFH map (see AFH above):

    ubertooth-follow -t 11:22:33:44:55:66 -a

Follow a piconet by the LAP and UAP `ubertooth-rx` reported for it. This
connects to `00:00:33:44:55:66`, so it works only against a target whose
address really does begin `00:00`; against any other it stops and asks for the
whole address (see AFH above). Use `-t` for a real target:

    ubertooth-follow -l 445566 -u 33 -q capture.pcap

## ARGUMENTS

`ubertooth-follow` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

An option the tool does not know is refused the same way, as one line on
standard error with no usage text after it: `error: unrecognized option '-Z'`.
An option whose value is missing reads `error: option '-l' requires a value`.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: LAP must be 6 hex digits (got 4466)` and
`error: target address must be xx:xx:xx:xx:xx:xx (got 11:22:33:44:55)`.

Three more kinds of command line are refused before any device is opened, each
with its one line:

 - any option given a value twice, `error: option '-t' given more than once`:
   the tool does not pick between two answers to the same question. This is
   the rule for every tool; see ubertooth(7), OPTION VALUES;
 - `-t` with `-l` or `-u`,
   `error: -t gives the whole target address, so it cannot be combined with -l or -u`;
 - one of `-l` and `-u` without the other,
   `error: -l <LAP> requires -u <UAP>` or
   `error: -u <UAP> requires -l <LAP>`: the two name a piconet only together.
   `-a` beside half a pair still reports `-a` needing `-t`, since completing
   the pair would not let `-a` run.

`-` means standard output only as the whole value of `-q` or `-d`
(ubertooth(7), OPTION VALUES). Any other value is a file name, including one
that starts with `-`: `-q -x` writes the file `-x`. Standard output carries
one stream, so `-q -` and `-d -` in one command are refused:
`error: -q and -d cannot both write to standard output`.

What the tool is following — the target address, or the LAP and UAP it read
from the local device — is reported on standard error, with the rest of the
tool's progress. Standard output carries the decoded packets, and, with `-a`,
the accepted channel map, which libbtbb prints itself — unless a capture has
taken that stream, and then the map is on standard error with the progress;
see CAPTURING TO STANDARD OUTPUT.

A refused command line creates and truncates nothing: `-r`, `-q` and `-d` name
their file while the command line is read, and it is opened only once every
check has passed. The board is opened and its firmware is checked before that,
so a run refused for either reason leaves no empty file behind either. A file
that cannot be created is one `error: ` line naming it, for example `error:
/no/such/dir/dump.bin: No such file or directory`, and exit 1. For a `-q` file
the line carries no reason, `error: CAPTURE.pcap: could not be created`: the
PCAP writer in libbtbb prints the reason itself, above that line, and leaves
nothing this tool can trust.

## CAPTURING TO STANDARD OUTPUT

`-q -` writes the PCAP capture to standard output and `-d -` the raw symbols,
for a pipe into another program. Either way the capture owns standard output
for the whole run, so nothing else is printed there: not the decoded packets,
and not the lines libbtbb prints about a clock it recovered. What the tool is
following, the notes and the failures are on standard error and still arrive,
and so is the `AFH map:` line of `-a`, which is the answer to that option.

Redirect standard output to a new file or a pipe, not to one being appended
to: on Linux the capture is opened again through `/proc/self/fd`, which
truncates, so `ubertooth-follow -q - >> capture.pcap` loses what the file
held.

Binary on a terminal is a mistake rather than a stream anyone reads, so it is
refused: `error: stdout is a tty, refusing to write binary capture data to
it`, exit 1. Redirect or pipe standard output instead.

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

## EXIT STATUS

A follow that was interrupted (Ctrl-C) exits 0 with whatever it captured, and
so does `-h`. A refused command line exits 1 (see ARGUMENTS), as does a file
that cannot be opened and a target the tool could not set up a follow on.

A page to the target that fails exits 1 as well, before any board is opened:
the page is what yields the target's clock offset, and a follow of a named
target without it would hop on the local device's clock instead. A follow keyed
on `-l` and `-u` pages a guessed `00:00:<UAP>:<LAP>`, so that line also asks
for the whole address with `-t` (see AFH above).

A board that refuses the target's address or the clock reset exits 1 before the
capture starts, with the request named on standard error. Following anyway
would mean a board that is not set up for the piconet: without the target's
address it matches no packet, and with its clock never zeroed it hops on a
clock the offset was not measured from.

Standard output stays empty in that case, and standard error names the request,
why the board refused it and the libusb code, adding a note where there is
something to try:

    error: could not zero the board's clock: the board did not answer in time (libusb -7)
    note: if a mode is running, stop it with ubertooth-util -S; a board that does not answer at all usually resets itself within a few seconds, so run the command again, and unplug it or power-cycle its hub port if it stays silent

That pair is the report, the same one ubertooth-rx(1) and ubertooth-scan(1)
give: the request named as `set the Bluetooth address` for the address, and as
`zero the board's clock` for the clock reset. A request the board's firmware
has not got is stalled, which reads `error: could not zero the board's clock:
the board's firmware does not support it (stalled, libusb -9)`, with a note
naming the board's firmware API version and the one these tools need (see
ubertooth(7), FIRMWARE OF ANOTHER API VERSION). A board that took only part of
either write gives the counts instead, as `error: Bluetooth address write
failed: board took 8 of 16 bytes` or `error: clock write failed: board took 2
of 4 bytes`, and the follow stops on that one line.

A follow whose USB stream fails part-way also exits 1: the board stopped
answering, or dropped off the bus, so the capture stops there and holds only
what arrived before it. The failure is reported on standard error as it
happens, as `error: receive transfer failed: <reason>`. A board that never
hands the transfer back at all exits 1 too, reported by the `warning:` line
ubertooth(7) describes rather than that error.

A follow the board would not stop exits 1 as well: the request that takes the
board out of its capture mode was stalled, so the board is left in that mode
and no other tool can open it until it is reset or re-plugged. What was
captured stays written out, and libubertooth names the refused request on
standard error.

The `-q`, `-r` and `-d` files are created once the board has been opened and
its firmware accepted, not before, so a run refused for either reason leaves
no empty capture behind. For this tool that is later than it sounds: the
adapter, the connection to the target and the clock and AFH reads all come
first, so a path that cannot be opened is reported after them. A terminal on
standard output is still refused while the command line is read, and so is
every other fault in the command line.

A capture that could not be started at all exits 1 as well, whichever step
failed: the receive transfer libusb would not take, the host thread that
collects the capture, or the request that starts hopping. Standard error
carries the one `error: ` line for that step, for example `error: could not
start the thread that collects the capture: Resource temporarily unavailable`.
The status is 1 in every case, never the failing call's own return value: a
negative libusb code handed to the exit status is wrapped into a byte, so
`libusb -9` reached a script as 247.

An adapter `-b` names that does not exist, or that cannot be opened, exits 1
with one `error: ` line naming it, the same wording ubertooth-scan(1) uses:
`error: Bluetooth device hci7 not found: No such device`.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts, and CARRY-OVER BETWEEN RUNS what else a board keeps from one
command to the next

ubertooth-rx(1), ubertooth-afh(1)

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
