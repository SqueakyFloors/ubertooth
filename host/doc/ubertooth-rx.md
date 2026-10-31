# ubertooth-rx(1) -- Classic Bluetooth discovery, sniffing, and decoding

## SYNOPSIS

    ubertooth-rx [-l <lap> [-u <uap>] | -z] [-c <0-78> | -s] [-e <0-5>]
    [-t <seconds>] [-r <file.pcapng>] [-q <file.pcap>|-] [-d <file.bin>|-]
    [-U <index|serial>]
    ubertooth-rx -i <file.bin> [-l <lap> [-u <uap>] | -z] [-e <0-5>]
    [-r <file.pcapng>] [-q <file.pcap>|-] [-d <file.bin>|-]
    ubertooth-rx [-U <index|serial>] -V
    ubertooth-rx -h

## DESCRIPTION

ubertooth-rx(1) is the primary interface into Classic Bluetooth (BR)
functionality provided by Ubertooth. It has two main modes of operation:
piconet following and survey mode. In either mode, ubertooth-rx(1) is
able to discover undiscoverable devices. See [DISCOVERING UNDISCOVERABLE
DEVICES][].

In piconet following mode, the tool will follow the first piconet it
fully identifies. In survey mode the device will attempt to identify all
piconets in a given area and display them after either a timeout or
manual interruption.

Piconet following is the main mode entered when no arguments are passed
to the command or a LAP and optionally a UAP are provided. If no
arguments are passed, the tool will attempt to calculate the UAP for any
observed LAPs. If a LAP is passed, the UAP will be calculated for that
specific LAP. Once a LAP and UAP have been recovered, the tool will
attempt to recover the clock value, and if that succeeds it will follow
that piconet.

Survey mode, entered using `-z`, will record all LAPs and attempt to
calculate the UAPs for any observed LAPs. This mode can be combined with
a timeout using `-t`, and it can be interrupted at any time using
`ctrl-C`.

This tool's `-z` is survey mode, not a squelch: only `ubertooth-util -z` sets
the receive squelch. The capture runs at whatever squelch the board holds when
the mode starts, the default -120 dBm unless `ubertooth-util -z` set another
value while the board was idle. The firmware resets the squelch on every mode
change, so a value set for an earlier capture is already gone; set it again
before this command, or while it is running. See ubertooth-util(1), SQUELCH.

## EXAMPLES

Follow the first piconet whose LAP, UAP, and clock are recovered from
the air:

    ubertooth-rx

For a given LAP, calculate the UAP and recover the clock, then follow:

    ubertooth-rx -l 112233

For a given LAP and UAP, recover the clock then follow:

    ubertooth-rx -l 112233 -u ab

Enter survey mode for 20 seconds, and print out the BD ADDRs of all
observed piconets:

    ubertooth-rx -z -t 20

Decode a capture saved earlier, writing a PCAP of it:

    ubertooth-rx -i capture.bin -q capture.pcap

## OPTIONS

Major modes:

 - `-l <lap>` :
   Limit UAP recovery, clock recovery, and piconet following to a given
   LAP. Format is 3 bytes / 6 hex characters.

 - `-u <uap>` :
   Limit clock recovery and piconet following to a given UAP. Must be
   used in conjunction with `-l`. Format is 1 byte / 2 hex characters.

 - `-z` :
   Survey mode: recover all LAP and UAP pairs and display them. Will run
   indefinitely until interrupted with `ctrl-C` unless paired with `-t`.

Options:

 - `-i <input>` :
   Replay a capture saved earlier with `-d` (or with `ubertooth-dump -d`
   without `-b`, which writes the same records) instead of capturing live with
   an Ubertooth. See [REPLAYING A SAVED CAPTURE][].

 - `-c <0-78>` :
   Fixed channel for all major modes. If not specified will sweep
   through all channels.

 - `-s` :
   Sweep through all channels. This is the default, so `-s` only states it
   explicitly; a `-c` channel wins over it.

 - `-e <0-5>` :
   Maximum access code bit errors. [Default: 2]

 - `-t <seconds>` :
   Timeout in seconds. If not specified will run indefinitely. Suggested
   values for `-z`: 20-60 seconds.

Output options:

 - `-r <file.pcapng>` :
   Capture packets to PcapNG. A PcapNG writer seeks back over what it has
   written, which standard output cannot do, so `-r -` is refused:
   `error: PCAPNG capture to stdout is not supported; use -q -`
 - `-q <file.pcap>` :
   Capture packets to PCAP. `-q -` writes the capture to standard output; see
   CAPTURING TO STANDARD OUTPUT
 - `-d <file.bin>` :
   Capture packets to binary file suitable for use with `-i`. `-d -` writes
   the records to standard output; see CAPTURING TO STANDARD OUTPUT

A record of a `-q` or `-r` capture carries the packet's own power and an
estimate of the background around it. Both are averaged in power rather than in
dBm, which is what the average of a set of dBm readings means.

The signal figure is the mean power of the loudest 10 dB of the readings the
board took across the receive window the packet arrived in. The window is a
fixed stretch of air time, not the packet, so leaving the quieter readings out
is what makes the figure the packet's: without it a packet filling a third of
the window would read 5 dB low.

The background is an estimate because the board cannot time the quiet between
packets, so it goes by level: it averages the readings that sat more than 10 dB
below the loudest it had taken since it last started counting. It starts
counting again on every hop, and 25 times a second even on a fixed channel, so
the figure describes the last few tens of milliseconds of air. Three things
follow. A faded part of a packet sits that far down too and is counted in.
Quiet taken before a packet is not admitted, so a window whose packet arrives
last contributes nothing. And the mean is over the loudest 10 dB of what is
admitted, so on a channel that is mostly silent the figure sits well above the
floor.

The screen shows the two as `s=` and `n=`, in dBm, with `snr=` the difference.
A capture record carries them as its signal power and noise power, and the
noise-power-valid flag says the noise field holds the board's estimate, so a
reader shows a signal-to-noise ratio wherever the estimate sits below the
packet. The ratio is as good as the estimate it rests on.

Where the board has admitted no reading since it last started counting -- a
channel it has heard loud but never quiet, or quiet but never loud -- there is
no estimate. Such a record leaves the flag clear, Wireshark shows the field as
`Invalid Noise Power`, and the screen prints `n=none snr=none`. A record whose
own window admitted none still carries the figure if an earlier window since
the last reset did: the estimate belongs to the channel over that stretch, not
to one window.

The board has one reading of the channel to work from, so both figures are of
whatever is on it. A transmitter louder than the packet, on the same channel at
the same moment, is what the signal figure describes, and the packet is then
part of the background.

Miscellaneous:

 - `-V` :
   print the version: the host tools, then the firmware version and build of
   each attached board; see ubertooth(7), COMMON OPTIONS

 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value, a stray word or
   an option given twice is still reported.

## HOW THE CHANNEL IS SET

The capture is asked for first and the channel, or the band sweep that is the
default, is stated to the board after it. A mode start resets the board's hop
mode, so a sweep stated before the capture request was thrown away and the
capture sat on one channel without saying so. See ubertooth-util(1),
CARRY-OVER BETWEEN RUNS.

The board keeps what is stated after the capture request whatever it was doing
before, so nothing has to be stopped first. A board left in a mode that
captures nothing — a killed `ubertooth-specan`, `ubertooth-btle` or
`ubertooth-ego` — used to swallow the sweep, because it runs the reset a moment
after the capture request has been answered, and the capture then sat on
2441 MHz.

The board stays on whatever it was tuned to until the request arrives and the
next hop acts on it, which takes up to 40 ms. Packets captured in that window
are reported on the channel they were captured on, as every packet is.

The modulation this capture reads, Classic, goes the other way round: it is
stated **before** the capture request, because that is what the board programs
its demodulator from as the mode comes up, and every reset that could throw it
away leaves Classic anyway. It is the board's own default, but an idle board
holds the last modulation a tool set, because a mode starting from idle resets
nothing. A `ubertooth-dump -l` interrupted before its capture started used to
leave the board demodulating low energy, and a capture here then found little
and said nothing about why. The board can still be streaming the previous
mode's records when the request arrives, so the first few packets of a capture
started on a board another run left going are that run's.

## DISCOVERING UNDISCOVERABLE DEVICES

Classic Bluetooth piconets are defined by the Lower Address Part (LAP)
and Upper Address Part (UAP) of the master device. These are elements
of the master device's Bluetooth Address (BD ADDR).

Consider the following BD ADDR:

    22:44:66:88:AA:BB

The lower address part (LAP) is the lower 24 bits, so `88:AA:BB`. In the
context of this tool, the value is written `88AABB`. The upper address
part is the next 8 bits, so `66`. The `22:44` is called the
Non-significant Address Part (NAP) and as you might imagine it is not
significant.

In piconet following mode, the tool will recover LAP values from the air
and attempt to calculate the UAP from those. It will go on to follow the
piconet if it can recover the clock value. In survey mode, the tool will
simply recover LAP and UAP values.

A clock recovered from a handful of packets is a guess, and a wrong guess
shows up as packet after packet of the piconet arriving and none of them
decoding. When that happens the tool reports

    warning: the clock being followed was not this piconet's; searching for it again

on standard error, stops following that clock and goes back to recovering
one. It keeps the LAP it has, so the search starts from what is already
known, and the capture carries on throughout.

To convert LAP + UAP pairs back into Bluetooth addresses, do the reverse
of the above. For example, if the tool recovers a LAP of 36A2B4 and a
UAP of 98, the associated Bluetooth address is `??:??:98:36:A2:B4`. Any
value can be substituted into the `??` slots, except one that makes the
address the local Bluetooth device's own, which a controller refuses to
page: of the paged device's address a page uses only the LAP and the
UAP, so the two unknown bytes have no part in it (Bluetooth Core,
Vol 2, Part B 1.3, 6.4.6, 7.1, and Table 2.3). So
`hcitool name 00:00:98:36:A2:B4` should
establish a connection to the device and return its name. What a real
controller makes of the guess has not been measured on this project's
bench ([issue #699](https://github.com/SqueakyFloors/ubertooth/issues/699)).

This attack works against discoverable and undiscoverable devices alike.

## REPLAYING A SAVED CAPTURE

Record the raw packets once with `-d <file.bin>`, then decode them as often
as you like with `-i <file.bin>`, without the Ubertooth or the piconet. Use it
to decode again with other options (the `-l`/`-u` you have since learned, or a
different `-e`), to analyse on another machine, to attach a capture to a bug
report, or as fixed input for tests:

    ubertooth-rx -d capture.bin                    # live, saving raw packets
    ubertooth-rx -i capture.bin -l 446688 -u 22    # later, no device needed

With `-i <file>`, ubertooth-rx(1) decodes the records of a saved capture
instead of opening a device: each record holds the host time the packet
was received and the packet as the Ubertooth delivered it. No device is
needed, and the decoding options (`-l`, `-u`, `-z`, `-e`) work as they do
on live data. `-c` and `-t` have no effect on a replay.

The times reported are the recorded ones, not the time of the replay. The
`systime` of each packet line is the host time stored with that record, a
capture written with `-q` or `-r` starts from the host time of the first
record it decodes and follows the Ubertooth clock from there, and `-d`
writes the records back out with their original times.

That clock starts over every 327.68 s, and each Classic packet carries a count
of how often it has, so the times hold across a quiet gap: a `-c` on a quiet
channel, a tight `-e`, or a capture left idle. The count is one byte, so a gap
of up to about 11.6 hours is covered; past that the periods cannot be counted
and every later time comes out short, which is reported on standard error:
`warning: packet times are behind the host clock by more than half the capture
clock's 327.68 s period: a quiet gap lost a clock wrap, or the host clock was
stepped forwards`. Both causes look the same from here, which is why the
message names each.

Where the records carry no count at all, which here is a capture's first
period, a live capture takes the periods in a quiet gap from the host's own
clock instead. A replay cannot: the host time a record carries is only a whole
second, and a saved file's records can be out of order or edited. So a replay
reads one period from a clock reading that came back lower than it did and
none from one that came back higher, as it always has.

Files recorded with older firmware can begin with a record out of place.
Firmware from before this fork's first release, 2020-12-R1+modern1, and
upstream firmware up to 2020-12-R1, could begin a capture with a record left
over from the mode before it, holding that mode's clock reading and its count
of periods. The second record then reads lower, which used to be taken for the
capture clock starting over and put 327.68 s into every time after it, and the
count dropping back to the new mode's used to be read as whole periods of time
passing: 18,321 s, over five hours, where the mode before had run 200 periods.
A replay now compares both against the host times the records carry: a clock
that started over, or periods that really went by, account for the time between
the two records, and a leftover record does not, so the capture is anchored on
the second record instead and the times that follow are its own. Only one
leftover is handled this way: a file that begins with two or more can still
come out 327.68 s, or a whole number of periods, late for the whole capture.
What cannot be
told apart is a leftover within a couple of seconds of a whole period of the
capture's own reading or count, which looks exactly like a wrap that late, or
of two periods of its count. The capture's times then come out one or two
periods early for the whole of it, and the warning above reports it; record
again with current firmware if a file's times still look wrong.

A replay that reaches the end of the file exits 0, including for an empty
file. These two are reported on standard error and exit 1, after whatever
was decoded:

 - a file that cannot be read, for example a directory:
   `error: reading <file> failed: Is a directory`
 - a last record cut short:
   `error: <file> ends in a partial record: 58 of 68 bytes, ignored`

A file that cannot be opened at all is reported the same way, before anything
is decoded: `error: <file>: No such file or directory`, exit 1. So is a `-d`
dump file that cannot be created, and a `-r` or `-q` capture file.

## ARGUMENTS

`ubertooth-rx` takes options only. A word that is not an option, including a
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

`-h` is what the user asked for, so its usage text goes to standard output,
nothing goes to standard error, and it exits 0.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: channel must be 0-78 (got 79)`.

An option combination the tool cannot carry out is refused there too, before
any device is opened, so the message never depends on which boards are
attached: `-u` without `-l` gives `error: -u <UAP> requires -l <LAP>`, and an
address beside survey mode gives `error: survey mode (-z) discovers addresses,
so it cannot be given -l or -u`.

A refused command line creates and truncates nothing: `-r`, `-q` and `-d` name
their file while the command line is read, and it is opened only once every
check has passed. The board is opened, its firmware is checked, and the capture
is set up as far as it can be, all before that, so a run refused for any of
those reasons leaves no empty file behind either (see EXIT STATUS). The
requests that start the capture itself are the one part that cannot come first,
and a board that refuses one of those has the capture files removed again, so a
refused run leaves nothing to tell apart from a capture that caught nothing. A
capture that ran keeps its file, however little arrived.

`-` means standard output only as the whole value of `-q` or `-d`
(ubertooth(7), OPTION VALUES). Any other value is a file name, including one
that starts with `-`: `-q -x` writes the file `-x`. Standard output carries
one stream, so `-q -` and `-d -` in one command are refused:
`error: -q and -d cannot both write to standard output`.

## WHAT A CAPTURE RECORD HOLDS

Every record of a `-q` or `-r` capture carries the LAP recovered from the
packet's own access code, in every mode, including a survey (`-z`). The
reference LAP beside it is a different thing: the address this tool was told
to look for, and a run given none records no reference LAP. The reference UAP
is the UAP this tool knows for that piconet, whether `-u` gave it one or it
worked the UAP out from the packets. Either field says with the flags when it
holds nothing, which is what a reader checks before using it.

A survey keeps a piconet for each LAP it hears and recovers its UAP from that
piconet's own packets, so a survey's records carry a reference UAP from the
packet that settles one onwards, beside a reference LAP that stays empty.

The packet's own 18-bit header joins them once a followed piconet's clock bits
are known, which is what the UAP search works out along with the UAP. A reader
checks a recorded header by recomputing its check bits from the reference UAP
in the same record. That is also how it tells a header this tool could not
place from one that was really sent: a packet of another piconet whose access
code matched is recorded with the bits the error correction made of its
header, and those bits fail the check.

A record written before those clock bits are known carries no header at all.
The header is whitened at the piconet's clock, so until the decoder has worked
that clock out there is nothing to unwhiten it with, and the record holds 18
zero bits instead. So do the records of a survey, which follows no piconet.
The packet that settles the UAP is the exception in either mode: the decoder
reads that packet's header and payload to settle the UAP in the first place,
so both reach its record, with the UAP they settled beside them. A piconet
whose UAP is `ff` records no reference UAP either: the decoder uses that value
for "none", so a real one cannot be told from it.

A record also carries flags for what the decoder verified, whether the header
check and the payload CRC were checked and whether each passed. The format of
a PcapNG capture's own options is specified in
`docs/source/bluetooth_captures_PCAP.rst`.

## CAPTURING TO STANDARD OUTPUT

`-q -` writes the PCAP capture to standard output and `-d -` the raw records,
for a pipe into another program. Either way the capture owns standard output
for the whole run, so the per-packet report is not printed; the packets
themselves are in the capture.

What the run works out still arrives, on standard error: the UAP the decoder
recovers (`Correct CRC! UAP = 0x22 found after 31 total packets.`), the clock
it goes on to recover from that UAP, the notes, the warnings, the failures,
and the survey results of `-z`, which is what that mode was asked for. So
`ubertooth-rx -l 446688 -q - > capture.pcap` reports the UAP it found while
the capture goes to the file.

Redirect standard output to a new file or a pipe, not to one being appended
to: on Linux the capture is opened again through `/proc/self/fd`, which
truncates, so `ubertooth-rx -q - >> capture.pcap` loses what the file held.

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

A capture transfer error is reported the same way. The chip's transfer
controller can report an error on the channel a capture receives into: what
the board was receiving at the time had arrived, so it is kept and marked,
while some of its bytes may be wrong and whatever the fault cut short before
it is lost. The first mark warns `warning: the board had a transfer error
while capturing; what it was receiving was kept, but some of those bytes may
be wrong and the capture may be missing packets around it`, and when the
capture ends it says how many there were: `warning: the capture has 3 transfer
errors where a packet may be wrong or missing`. ubertooth-util(1) `-w` reports
the board's own count of them since power-on, over every capture the board has
run.

Symbols the board wrote over are reported the same way. The board receives
into one block of symbols while its capture loop reads the one before it, and
when a block finishes arriving before the loop has read that one, the new
symbols go over the old ones. The block is gone before anything looked in it
for a packet, so this loss has no packet to point at and nothing in the output
marks where it happened. The first one warns `warning: the board fell behind
while capturing and wrote new symbols over a block it had not read yet;
whatever was in the air then is missing from this capture`, and when the
capture ends it says how many such places there were: `warning: the capture has
3 places where the board wrote new symbols over a block it had not read`. The
count is of those places, not of the blocks lost at them: the board has one
flag for this and no field to send a number in, so one place can stand for
several blocks.

A capture read back from a file with `-i` reports the same way: all three
losses are recorded in the packets, so replaying one warns as it reads them
and gives the same totals at the end.

## EXIT STATUS

A capture that ran to its `-t` timeout, or that was interrupted (Ctrl-C),
exits 0 with whatever it captured, and so does `-h`.

A run stopped by Ctrl-C before it had opened a board exits 0 as well: see
ubertooth(7), SELECTING A DEVICE.

A refused command line exits 1 (see ARGUMENTS), as does a file that cannot be
opened and a replay that could not be read whole (see REPLAYING A SAVED
CAPTURE). So does a run that cannot allocate the piconet `-l` names
(`error: out of memory`), which no capture can follow.

A live capture whose USB stream fails part-way also exits 1: the board stopped
answering, or dropped off the bus, so the capture stops there and holds only
what arrived before it. The failure is reported on standard error as it
happens, as `error: receive transfer failed: <reason> (<status>)`. A board
that never hands the transfer back at all exits 1 too, reported by the
`warning:` line ubertooth(7) describes rather than that error. A `-z` survey
still prints the results it did get.

A capture whose teardown the board refuses exits 1 too, even though the
capture body itself ran to its timeout: the board is left in the mode this run
started, and no other tool can open it until it is reset or re-plugged.
Standard error carries the `error: could not stop the running mode` line
libubertooth prints, and whatever was captured is still written out — those
packets are real.

A capture that could not be started at all exits 1 as well, whichever step
failed: the receive transfer libusb would not take, the host thread that
collects the capture, or the request that puts the board in its capture mode.
Standard output stays empty and standard error carries the one `error: ` line
for that step, for example `error: could not start the thread that collects
the capture: Resource temporarily unavailable`. The capture request is named
the way the channel request below is, with the reason the board gave and the
libusb code. The status is 1 in every case, never the failing call's own
return value: a negative libusb code handed to the exit status is wrapped into
a byte, so `libusb -9` reached a script as 247.

A board that refuses the channel request, the modulation request, or the
address request `-l` and `-u` together name, exits 1 as well, and nothing is
captured, so a board that will not leave the channel it is on cannot be
mistaken for one that is sweeping. The channel is stated once the capture has
been asked for and the modulation just before it (see HOW THE CHANNEL IS SET),
so a board that refuses either has the capture stopped and is given back, and
the modulation reads `error: could not set the modulation: ...` in the same
shape as the lines below. Standard output stays empty. Standard error names the
request, why
the board refused it and the libusb code, and adds a note where there is
something to try:

    error: could not set the channel: the board did not answer in time (libusb -7)
    note: if a mode is running, stop it with ubertooth-util -S; a board that does not answer at all usually resets itself within a few seconds, so run the command again, and unplug it or power-cycle its hub port if it stays silent

That pair is the report: the request named as `set sweep mode` without `-c`,
since the command then asks for the sweep across the whole band rather than
for a channel, and as `set the Bluetooth address` for the address request. A
request the board's firmware has not got is stalled, which reads `error: could
not set the channel: the board's firmware does not support it (stalled, libusb
-9)`, with a note naming the board's firmware API version and the one these
tools need (see ubertooth(7), FIRMWARE OF ANOTHER API VERSION). A board
that took only part of the address gives the counts instead, as `error:
Bluetooth address write failed: board took 8 of 16 bytes`.

A refused channel request is answered with the capture already running (see
HOW THE CHANNEL IS SET), so the mode is stopped on the way out. A board that
has stopped answering altogether can add a line of its own to the report as
that is done.

A capture the board would not stop exits 1 as well, however cleanly the
capture itself ran: the request that takes the board out of its capture mode
was stalled, so the board is left in that mode and no other tool can open it
until it is reset or re-plugged. What was captured stays written out, a `-z`
survey still prints its results, and libubertooth names the refused request on
standard error. This is what a `-t` run that reported success and then left a
board nothing could open used to look like.

The `-q`, `-r` and `-d` files are created once the board has been opened, its
firmware accepted, and the capture set up as far as it can be without them.
The decoder, the piconet `-l` names, and the address request that `-l` with
`-u` sends all come first, so a run refused at any of those steps leaves no
empty capture behind. A replay (`-i`) creates them once its input file has
been opened, since it needs no board. A terminal on standard output is still
refused while the command line is read.

What is left after that is the capture starting: the USB transfer, the thread
that completes it, and the modulation, capture, and channel requests. A run
that ends at one of those removes the capture files it had created, naming any
it cannot remove as `warning: cannot remove FILE: ...`. Only a regular file at
that name is removed: a name the user had set up as something else --
`/dev/null`, a pipe, or a symbolic link -- is left where it is, so no run takes
away a path the user arranged, and a capture written to standard output is no
file at all. A `-q` or `-d` name that already held a file of the user's is
removed along with the rest, since creating the capture had truncated it in any
case; `-r` refuses such a name outright and never reaches this. Once the first
packet can arrive the file stays, because a capture that decoded nothing is a
result of its own and its header says the capture ran.

A replay is the same with its input in place of the board: an input that could
not be read, or that ends partway through its first record, leaves no capture
file behind. From the first whole record on the files stay, however little of
the input could be read after it. An input read to its end is no failure at
all, an empty one included: that replay exits 0 and keeps its files, as a
capture that caught nothing does.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts

ubertooth-scan(1): active device scanning and inquiry using Ubertooth
and BlueZ

ubertooth-dump(1): records the same packets `-i` reads back, with `-d`

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

D. Spill and A. Bittau. "BlueSniff: Eve Meets Alice and Bluetooth."
USENIX WOOT 2007.

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-rx(1) is Copyright (c) 2010-2017 Michael Ossmann, Dominic
Spill, and others. This tool is released under the GPLv2. Refer to
[COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
