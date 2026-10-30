# ubertooth-afh(1) -- passive detection of AFH channel map

## SYNOPSIS

    ubertooth-afh -u <uap> -l <lap> [-r] [-m <count>]

## DESCRIPTION

Classic Bluetooth piconets commonly make use of Adaptive Frequency
Hopping (AFH) to avoid interference with other signals in 2.4 GHz. This
tool attempts to determine the AFH map being used for a given piconet
identified by UAP and LAP.

At a minimum you must provide UAP and LAP. See ubertooth-rx(1) for a
method for sniffing these over the air and details on where else to find
them. By default the tool will only print the AFH map when it is
updated. By providing the `-r` flag, the AFH map will be printed out in
binary form once per second.

The capture is gated by the board's squelch, which this tool does not set: it
is the default -120 dBm unless `ubertooth-util -z` set another value while the
board was idle, and the firmware resets it on every mode change, so it never
carries over from an earlier capture. See ubertooth-util(1), SQUELCH.

## OPTIONS

 - `-l <LAP>` :
   LAP of target piconet (3 bytes / 6 hex digits)
 - `-u <UAP>` :
   UAP of target piconet (1 byte / 2 hex digits)
 - `-m <int>` :
   threshold for channel removal: a channel is marked unused once this many packets
   arrive without it being seen; 1 or more (default: 800). It counts packets, not
   seconds; see THE REMOVAL THRESHOLD
 - `-r` :
   print AFH channel map once every second (default: print on update)

Other options:

 - `-t <seconds>` :
   timeout for initial AFH map detection (not required); see DETECTION AND
   MONITORING. No effect with `-r`.
 - `-e <0-5>` :
   maximum access code errors (default: 2, range: 0-5)
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

## DETECTION AND MONITORING

`ubertooth-afh` does not stop on its own. It keeps monitoring the piconet and
reporting changes to the map until it is interrupted (Ctrl-C), both with and
without `-t`.

While it listens, the board hops across the channels the piconet has not been
seen on, and the tool tells the board to stop visiting a channel once a packet
from the piconet has arrived there. That is how the map fills in: a channel is
reported used when a packet is received on it.

The one exception is a single unused channel between two used ones. The tool
guesses that such a channel is one no packet has landed on yet rather than one
the piconet left out, so when a packet arrives on a channel whose next-but-one
neighbor is already used, the channel between them is marked used too, with a
note on standard error:

    note: activating additional channel 37

Nothing in the specification stops a piconet from leaving out one channel on
its own, so a map reported by this tool can name a channel the piconet does not
use. Only a packet is evidence.

Basic Rate has 79 channels, 0 to 78, and the rule needs a real channel on the
far side of the gap, so it never reaches past either end of the band: channels
0 and 78 are reported used only once a packet arrives on them. Channel 1 is
still filled by a packet on channel 0 when channel 2 is used, and channel 77 by
a packet on channel 78 when channel 76 is.

The map lives on the board only while this run does. Every mode start resets
the board's hop mode and AFH map, so this run begins from an empty map, and the
next capture — another `ubertooth-afh`, an `ubertooth-rx`, anything — hops
across the whole channel set rather than on the map found here. That holds even
if this run is killed and leaves the board streaming. See ubertooth-util(1),
CARRY-OVER BETWEEN RUNS.

`-t <seconds>` only adds an initial detection phase: the tool listens for that
many seconds, prints the map it found, and then starts monitoring. Only the
timeout moves it on to monitoring: interrupting the detection phase (Ctrl-C)
ends the run there, printing the map detected so far, rather than starting to
monitor something you have just asked the tool to stop. If that
phase received no packet from the piconet there is no map to print, so the tool
says so, does not start monitoring, and exits with status 2 instead of
printing an all-zero map (which reads as a detected map whose piconet uses no
channel at all):

    $ ubertooth-afh -l 446688 -u 22 -t 3
    error: no packets from ??:??:22:44:66:88 within 3 s; AFH map unknown, not monitoring

Nothing is wrong with the board in that case: the piconet is out of range, it
is not transmitting, or the UAP/LAP is not the one in use. A longer `-t`, or no
`-t` at all, keeps listening.

To wait for a piconet that is not in range yet (start the tool, then bring the
devices close or start the connection), run without `-t`: monitoring starts at
once, waits as long as it takes, and notes `+ channel N is used now` on
standard error as the piconet's packets arrive. `-t` is for a bounded check
that gives up when nothing is heard in time.

`-r` has its own loop, which prints the map once a second from the start and
ignores `-t`.

## THE REMOVAL THRESHOLD

`-m` counts packets, not seconds: a channel leaves the map once that many
packets from the piconet have arrived with none of them on that channel. So
what the threshold means depends on how many channels the piconet is using, not
on how long the run has been going.

That is a trade-off, and both ends of it cost something. A small threshold
notices quickly when the piconet really does stop using a channel, and reports
channels it is still using as dropped. A large one settles into a map that
stops changing, and is slow to notice a channel the piconet has given up.

The default is 800, which is chosen to hold the map of a piconet using the
whole band. A piconet hops to a different channel each slot, so with 79
channels in use a given channel carries about one packet in 79, and a threshold
has to be a good few times the number of channels in use before a channel that
is still used is reliably seen again within it: 800 packets give each of 79
channels about ten chances to appear. Below that, channels the piconet is using
are pruned and the map does not hold still — the monitor output becomes a
stream of `+ channel N is used now` and `- channel N is not used any more`
lines, and at a threshold of a few tens it reports a handful of channels for a
piconet that is using all 79.

The cost is the other end of the trade-off. A channel the piconet really has
given up stays in the map until 800 more packets have arrived without it, and
how long that takes depends on how busy the piconet is. Lower `-m` if the
piconet is known to use few channels, or if noticing a dropped channel quickly
matters more than a map that holds still.

## ARGUMENTS

`ubertooth-afh` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: timeout must be a whole number of seconds, 0 for none (got abc)`.

Both halves of the piconet address are required, and a command line missing
either is refused before any device is opened:
`error: -l <LAP> and -u <UAP> are both required: the map is detected for one
piconet`.

## DIAGNOSTICS

Standard output carries the detected channel maps. Errors, warnings and the
tool's own progress go to standard error, each error as one line starting
`error: `. The channel-change lines that libubertooth raises alongside each map
are `note: ` lines on standard error; see ubertooth(7), OUTPUT AND DIAGNOSTICS.

A command line the tool will not take is reported as that one `error: ` line
on standard error, with no usage text after it: the line describes the
problem, and `-h` prints the usage text for a reader who wants it. An option
the tool does not know reads `error: unrecognized option '-Z'`; one whose
value is missing reads `error: option '-l' requires a value`.

`-h` is what the user asked for, so the usage text goes to standard output
and nothing goes to standard error. A bare `ubertooth-afh`, with no arguments
at all, is the one refusal the usage text does follow, on standard error with
the `error: ` line.

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

## EXIT STATUS

`ubertooth-afh` does not stop on its own, so it normally exits 0 after being
interrupted (Ctrl-C). `-h` also exits 0.

A run stopped by Ctrl-C before it had opened a board exits 0 as well: see
ubertooth(7), SELECTING A DEVICE.

It exits 2 when the `-t` detection phase received no packet from the piconet
(see DETECTION AND MONITORING). The number of seconds in that message is the
`-t` value, not the time actually spent listening, which is shorter if the
tool was interrupted during the phase.

It exits 1 without opening a device for a bad command line (a missing UAP or
LAP, an option value out of range, or a word that is not an option) or when the
piconet it tracks cannot be allocated (`error: out of memory`), and when the
selected Ubertooth is not found. It also exits 1, with or without `-r`, when
the receive could not be started at all: a setup request the board refused,
or a USB transfer that could not be set up. One of those is the band sweep
the whole band is watched through, without which the board stays on the
channel it was left on and the map would hold that channel alone.

A run whose USB stream fails part-way, with or without `-r`, exits 1: the
board stopped answering or dropped off the bus, so what it reported is
whatever arrived before that. A detection phase that lost its stream is never
reported as a quiet piconet, so it exits 1 rather than 2.

A teardown the board refused exits 1 too, and this is one of the paths that
reach it: the request that takes the board out of its capture mode was
stalled, so the board is left in that mode and no other tool can open it
until it is reset or re-plugged. The map the run detected is still printed,
and libubertooth names the refused request on standard error. A quiet piconet
is not reported as 2 in that case either.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts, and CARRY-OVER BETWEEN RUNS what else a board keeps from one
command to the next

ubertooth-rx(1): a tool for finding UAP and LAP

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-afh(1) is Copyright (c) 2015-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
