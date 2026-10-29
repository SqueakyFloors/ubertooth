# ubertooth-btle(1) -- Bluetooth Low Energy (BLE) sniffing and more

## SYNOPSIS

    ubertooth-btle -f [-A 37|38|39] [-r output.pcapng]

## DESCRIPTION

`ubertooth-btle` is a tool for doing Fun Stuff(TM) with BLE. It can do
the following things:

 - Sniff connections
 - Interfere with connections
 - Send advertising packets (experimental)

Sniffing connections is the most robust feature supported by
`ubertooth-btle`. It has two primary modes of operation: follow mode and
promiscuous mode.

Follow mode is the preferred mode for general use. In this mode,
Ubertooth will listen on one of three advertising channels waiting for a
BLE connection to be established. When a connection is established,
Ubertooth will hop along the data channels, passively capturing the data
sent between the central and peripheral. After the connection
terminates, Ubertooth will return to the advertising channel and wait
for another connection.

No-follow mode is similar to follow mode, but it only logs advertising
packets and will not follow connections as they are established.

Promiscuous mode is an experimental mode for sniffing connections after
they have already been established. This mode can be used to sniff
long-lived connections.

When sniffing, Ubertooth can only operate in either follow mode or
promiscuous mode, but not both at the same time. If you are unsure which
mode to use, use follow mode.

While promiscuous mode hunts for an access address it is gated by the board's
squelch; follow and no-follow mode are not. `ubertooth-btle` does not set the
squelch, so it is the default -120 dBm unless `ubertooth-util -z` set another
value while the board was idle, and the firmware resets it on every mode
change, so it never carries over from an earlier capture. See
ubertooth-util(1), SQUELCH.

By default, Ubertooth will follow any connection it observes. You can
limit this to following a specific Bluetooth Address (BD ADDR) using the
`-t` command line flag. `-t` is for follow mode: it picks out a connection by
the device address its advertising and connect packets carry, which is what
`-f` watches for (in `-n` the board drops the advertising packets that do not
match it, and nothing else). Promiscuous mode starts on a connection that is
already running, whose packets carry no device address at all, so `-t` has
nothing to match there and is refused with `-p`. For example, the following
command will only sniff connections where the central or peripheral's BD ADDR
is `22:44:66:88:AA:CC`:

    ubertooth-btle -f -t 22:44:66:88:AA:CC

`-t` can also take a mask length in CIDR-like notation. Masks can be
between 1 and 48 bits long, with a 48 bit mask matching the entire
address. Using a /24 mask will filter on just the OUI. For example, to
limit sniffing to just TI devices with the OUI 00:1A:7D, use the
following:

    ubertooth-btle -t 00:1A:7D:00:00:00/24

Filters persist until they are explicitly cleared or the system restarts
(either via `ubertooth-util -r` or unplug/replug). To clear a filter,
use the special filter `none`. Example:

    ubertooth-btle -t none

When filtering, previous versions of the firmware would still log all
advertising packets but only follow connections based on the filter
parameters. As of 2018-06-R1, advertising packets that do not match the
filter are dropped.

In all sniffing modes, Ubertooth can log data to PCAP or PcapNG with a
variety of pseudoheaders. The recommended logging format is PcapNG
(`-r`) or PCAP with LE Pseudoheader (`-q`). For compatibility with
crackle (see [USING WITH CRACKLE][]), use PCAP with PPI (`-c`).

In every sniffing mode, a packet carrying a Bluetooth 4.2 Data Length
Extension payload, up to its 251-byte maximum, is captured whole: on the
screen, in PCAP and in PCAPNG. The board sends such a packet in several
fixed-size records, which are put back together before anything is decoded or
written. Older firmware cut every packet to 50 bytes.

Interfering with connections is a feature for causing intentional
interference with newly established or long-lived connections. When this
attack succeeds, the BLE connection between the central and peripheral
will be terminated. Pair the `-i` or `-I` flag with `-f` to interfere
with new connections or `-p` to interfere with long-lived connections.
`-i` or `-I` (or their older spellings `-j` and `-J`) with neither `-f` nor
`-p` is refused before any device is opened: there is no connection to
interfere with.
Note that causing intentional interference may be illegal in your
jurisdiction. Check your local laws before using this feature.

Finally, `ubertooth-btle` supports transmitting advertising packets with
a specified BD ADDR. This feature, referred to as faux slave mode, is
experimental and may not function as intended. Use at your own risk.

## EXAMPLES

Sniff all connections on advertising channel 38, logging all data to
PcapNG:

    ubertooth-btle -f -A 38 -r log.pcapng

Log advertising packets without following connections:

    ubertooth-btle -n

Interfere with connections recovered with promiscuous mode:

    ubertooth-btle -p -I

Send advertising packets using BD ADDR `22:44:66:88:AA:CC`:

    ubertooth-btle -s 22:44:66:88:AA:CC

## OPTIONS

 - `-h` :
   Displays help message
 - `-V` :
   print the version: the host tools, then the firmware version and build of
   each attached board; see ubertooth(7), COMMON OPTIONS

Major modes:

 - `-f` :
   Follow mode: sniff connections as they are established
 - `-n` :
   No-follow mode: log advertising packets but don't follow connections
 - `-p` :
   Promiscuous mode: sniff already-established connections. It sets no channel
   of its own: it listens on the one the board is holding, 2440 MHz unless
   `ubertooth-util -c` left it on another even frequency. It discovers the
   access address of a connection on that channel, unless an earlier
   `ubertooth-btle -a <address>` command gave the board one to follow; nothing
   else makes it skip discovery, and `-t` is refused with it. As it works a
   connection out it reports each parameter it has recovered: the access
   address, the CRC init value, the hop interval and the hop increment. Those
   lines go to standard output, or to standard error once `-q -` or `-c -` has
   taken standard output, so a capture written to a file still says which
   connection it is of.
 - `-s <BD ADDR>` :
   Inject advertising packets using specified BD ADDR. Like `-a`, it does not
   go with `-f`, `-n` or `-p`. The value is one whole address and nothing
   else: a faux slave advertises from a single address, so the `/<bits>` mask
   and `none` that `-t` takes are refused here, each as the address it is not
   (`error: faux slave address must be xx:xx:xx:xx:xx:xx (got none)`). Both
   used to be parsed and then dropped, so `-s none` advertised from
   `00:00:00:00:00:00`.

Interference (only with `-f` or `-p`):

 - `-i` :
   Interfere with one connection and return to idle
 - `-I` :
   Interfere continuously with many connections
 - `-j` :
   Original "jam" spelling of `-i`; does exactly what `-i` does
 - `-J` :
   Original "jam" spelling of `-I`; does exactly what `-I` does

Filtering:

 - `-t <BD ADDR>` :
   Limit connection following and interference in follow mode to the
   specified BD ADDR. It is for the follow modes: `-f` follows only a
   connection whose advertising or connect packets carry this address, and
   `-n` drops the advertising packets that do not. Every form of it, `-t none`
   included, is refused with `-p`, which has no device address to match
   against: `error: -t does not go with -p: promiscuous mode has no device
   address to match a target against`. The combination used to be accepted,
   and the board then read "a target was set" as "the host gave an access
   address", so `-p` followed `8e89bed6` and captured nothing.
   `-s` and `-t` are two different addresses and a command
   can give both: `-s A -t B` advertises from A and follows B, and `-t none`
   clears the target without touching the address `-s` was given. They shared
   one address, so whichever of the two was written last used to set both, and
   `-s <BD ADDR> -t none` advertised from `00:00:00:00:00:00` without a word.

Logging:

 - `-r <output.pcapng>` :
   Log to PcapNG (preferred). PcapNG output cannot be streamed to stdout because
   the writer must seek while finalizing the file; `-r -` is rejected.
 - `-q <output.pcap|->` :
   Log to PCAP with `DLT_BLUETOOTH_LE_LL_WITH_PHDR`. Use `-` to stream binary
   PCAP to stdout.
 - `-c <output.pcap|->` :
   Log to PCAP with PPI (for compatibility with crackle(1)). Use `-` to stream
   binary PCAP to stdout, for example:

       ubertooth-btle -f -c - | wireshark -k -i -

   When stdout capture is selected, regular packet/status text is suppressed so
   it cannot corrupt the binary stream; errors remain on stderr. Such a run
   does not produce the per-packet dump at all, so the decoder's complaints
   about an advertising field it could not read in one of those packets do not
   reach stderr either. The lines that
   answer an option in the same command go to stderr rather than being
   suppressed with it: the `Access address:` line of `-a`, the `CRC:` line of
   `-k`, the target lines of `-t`, and the connection parameters `-p`
   recovers. The tool
   refuses to write binary capture data directly to an interactive terminal:
   `error: stdout is a tty, refusing to write binary capture data to it`, with
   exit status 1 and before any device is opened.

The times in a logged capture follow the board's capture clock, which starts
over every 429.50 s in the `-f` and `-n` modes and every 327.68 s in
promiscuous mode. A promiscuous packet carries a count of how often it has;
the packets of the other two modes carry none, so how often it started over is
worked out from the host's own clock instead: the time the host saw pass
between two packets says how many periods lie between their clock readings, so
a quiet gap keeps its time however long it was. Before, a gap longer than one
period left every later time short by a period, since the clock then reads
higher than it did and nothing in the capture shows it started over at all.

Times that still come out short are reported, on standard error: `warning:
packet times are behind the host clock by more than half the capture clock's
429.50 s period: a quiet gap lost a clock wrap, or the host clock was stepped
forwards`. Both causes look the same from here, which is why the message names
each.

Miscellaneous:

 - `-A <37|38|39>` :
   Which advertising channel to use in follow mode (`-f`, `-n`) and faux slave
   mode (`-s`) (default: 37). Refused with `-p`, which sets no channel of its
   own.
 - `-a [address]` :
   Get (`-a`) or set (`-a <address>`) the access address used in promiscuous
   mode. The address is 1 to 8 hex digits (`-a 8e89bed6`); fewer than 8 means
   leading zeros (`-a 12345` sets `00012345`). Anything else is refused before
   the device is opened. Getting and setting in one command is refused; other
   options such as `-k` still run after a get. `-a` does not go with a
   sniffing mode (`-f`, `-n`, `-p`), which runs until it is interrupted; the
   combination is refused before any device is opened. So an address for `-p`
   is given by a command of its own first: `ubertooth-btle -a <address>` and
   then `ubertooth-btle -p`. It is the one setting that makes `-p` skip
   discovery, and the board keeps it until its low-energy state next resets:
   a connection timing out during a `-p` run, a `-p` run that went looking for
   an address itself, `ubertooth-util -r`, or a re-plug. A follow run (`-f`,
   `-n`) does not clear it. Promiscuous mode then discovers an address again
   rather than falling back to the advertising one.
 - `-k [01]` :
   Read (`-k`) or set (`-k 0` or `-k 1`) CRC verification
   (default: 0). Any other value is refused. A set and a read in one command
   both happen, the set first, so the read reports the new value, in whichever
   order they are written; two sets are refused, as two values for one setting
   are everywhere (ubertooth(7), OPTION VALUES). In a sniffing mode both
   run once the mode has started, because entering a mode clears the setting,
   and the firmware then turns verification on and off itself as it recovers a
   connection.
 - `-v [01]` :
   The older spelling of `-k`, **deprecated**. It still does what `-k` does
   and prints one line on standard error -- `warning: -v for the CRC verify
   setting is deprecated and will be removed; use -k` -- so a script that uses
   it keeps working for this release. After that `-v` means verbose here as it
   does in every other tool (ubertooth(7), COMMON OPTIONS). A command line
   that gives both spellings names one setting twice and is refused:
   `error: -k cannot be combined with -v; they are the same CRC verify setting
   written two ways`.
 - `-x <0-32>` :
   Allow n access address violations (default: 32). Filtering occurs on
   host.

Data source:

 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE

## USING WITH CRACKLE

`crackle` is a tool for cracking the BLE key exchange and decrypting
encrypted data. To capture data for use with `crackle`, sniff
connections in follow mode using `-f` and log data to PCAP/PPI using
`-c`. Example:

    ubertooth-btle -f -c crack.pcap

Refer to `crackle` documentation for further details.

## TRANSMIT POWER

Injecting advertising packets (`-s`) and interference (`-i`, `-I`, `-j`, `-J`)
transmit at the CC2400 power level set with `ubertooth-util -a` and leave it
unchanged. A board that has not been given a level transmits at 7 (0 dBm), the
CC2400's reset value. Sniffing does not transmit and does not change the level.
These levels are at the CC2400 radio; the Ubertooth One's RF amplifier changes
the actual output after that.

## ARGUMENTS

`ubertooth-btle` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: advertising channel index must be 37-39 (got 40)`.

A bare `-` is still the stdin/stdout value of `-q`, `-c` and `-r`. See
ubertooth(7), OPTION VALUES, for the forms an option's value can take,
including the optional values of `-a` and `-k`.

An option combination the tool cannot carry out is refused there too, before
any device is opened, so the message never depends on which boards are
attached: more than one of `-f`, `-n` and `-p` gives
`error: choose one of -f, -n or -p`, and a read and a set of the
access address in one command gives
`error: -a cannot both read and set the access address`.

`-a` or `-s` alongside a sniffing mode gives
`error: -a and -s do not go with -f, -n or -p: a sniffing mode runs until it
is interrupted`, so there is nothing for an access address or faux slave mode
request to run after. `-t` and `-k` are not in that list: `-t` is sent before
the mode starts, which is what it is for, and the `-k` options run once it has
started.

`-t` with `-p` gives
`error: -t does not go with -p: promiscuous mode has no device address to
match a target against`, whatever the target is: a connection that is already
running names no device, so `-t none` is refused beside `-p` as well.

`-i` or `-I` without `-f` or `-p` gives
`error: -i needs -f or -p: there is no connection to interfere with
otherwise`, naming the spelling that was used (`-j` and `-J` included).

That includes `-i` on its own: it says more than listing every major mode
would, because only two of them can carry interference.

A command line with no work in it is refused the same way. `-U`, `-A`, `-x`
and the capture options only shape work another option asks for, so a command
made up of those alone, or a bare `ubertooth-btle`, gives
`error: nothing to do: choose a major mode (-f, -n or -p), or -a, -k, -s or -t`,
on standard error with nothing on standard output. A bare `ubertooth-btle` is
the one refusal the usage text follows, also on standard error; every other
refused command line is the `error: ` line alone, and `-h` prints the usage
text on standard output.

A refused command line creates and truncates nothing: `-r`, `-q` and `-c` name
their file while the command line is read, and it is opened only once every
check has passed. The board is opened and its firmware is checked before that,
so a run refused for either reason leaves no empty file behind either.

## DIAGNOSTICS

Standard output carries only what was asked for: either the decoded packets or
the capture data of `-q -` or `-c -`, plus the lines that answer `-a`, `-k` and
`-t` and the parameters `-p` recovers. With the capture on standard output,
standard output belongs to the capture for the whole run: those lines move to
standard error, and the decoded packets are not printed at all.

The `rssi=` figure on each packet's `systime=` line is that packet's own level
in dBm: the strongest RSSI reading the board reports for the packet, through
the CC2400 datasheet's conversion. It is the same number the signal field of a
`-q` or `-r` capture carries for that packet, so a dump and a capture of one
run agree. A `-c` PPI capture carries the board's raw counts instead, which
are not dBm. The conversion has ends, so `rssi=0` means "at least this loud"
and `rssi=-120` means "no louder than this".

Earlier releases printed the weakest reading the board reports for the packet
instead, through a rougher approximation than the datasheet's curve. That
figure ran far below the packet's own level: on the bench it reached -96 dBm
where the quietest packet the capture recorded was -78, so a dump and a
capture of the same run disagreed by up to 55 dB.

Errors, warnings and progress go to standard error, each error as one line
starting `error: `, for example `error: USB poll failed` when the board stops
answering during a capture, `error: jamming not supported by this firmware`,
`error: target set failed`, or
`error: target address must be xx:xx:xx:xx:xx:xx (got 11:22:33:44:55)`. A value
that is not an address reads the same way whatever is wrong with it, and names
the address the option sets, so `-s` gives
`error: faux slave address must be xx:xx:xx:xx:xx:xx (got <word>)`, including
for the `none` and `22:44:66:88:aa:cc/24` that `-t` takes, and a bad target
mask gives `error: target mask must be 0-48 (got 49)`. A capture
file that cannot be created is reported as `error: <file>: <reason>`.

A capture that is losing packets says so. This tool reads its packets one at a
time, and a reply that held part of a record cannot be read as one, so it is
dropped and reported once per run: `warning: a poll reply arrived with 32 of a
packet's 64 bytes and was dropped; the capture is losing packets`. The capture
runs on and ends as it otherwise would, so the warning is the only sign.

Packets the board had to throw away are reported too. It holds captured
packets in a queue until this tool reads them, and a queue with no room left
loses the next packet; a packet longer than 50 bytes takes several of the
queue's slots and is lost whole, so a busy capture can lose the long packets
while the short ones still arrive. The first time it happens the capture warns
`warning: the board's packet queue filled up and it dropped at least one
packet; this capture has a gap`, and when the capture ends it says how many
such gaps there were: `warning: the capture has 3 gaps where the board's queue
filled up and it dropped packets`. The count is of gaps, not of lost packets:
one full queue can swallow several packets, and the board sends no number.
Nothing says which packets are missing, so a capture with gaps is complete
only between them. The board reports a gap on the first packet that gets
through after it, so a capture that ends in the moment after a gap does not
report that last one.

A poll the board does not answer is tried again, up to three attempts, before
the capture gives up: a board busy elsewhere for a moment no longer ends a
capture that has been running for hours. A board that has left the bus ends it
at once. The retry saves the capture, not the packet: the board takes a packet
out of its queue as it answers the poll, so the one a timed-out poll was
carrying is gone.

Each request that sets a mode up is named on its own, so the line says which
one the board refused: `error: could not set the advertising channel`,
`error: could not set the advertising data`, `error: could not start
advertising` for `-s`, and `error: could not start follow mode` (or
`no-follow`, or `promiscuous`) for the sniffing modes. No capture starts after
one of these.

`-q` and `-c` share one capture handle, so a second file between them is
ignored with `warning: ignoring extra capture file: <name>` and the first one
given is used; `-r` has its own handle, so `-q a.pcap -r b.pcapng` writes both.

A command line the tool will not take is reported as that one `error: ` line
on standard error, with no usage text after it: the line describes the
problem, and `-h` prints the usage text for a reader who wants it. An option
the tool does not know reads `error: unrecognized option '-Z'`; one whose
value is missing reads `error: option '-A' requires a value`.

`-h` is what the user asked for, so the usage text goes to standard output.
Nothing else goes to standard error either, unless the rest of the command
line produced a warning before `-h` ended the command. A bare
`ubertooth-btle`, with no arguments at all, is the one refusal the usage text
does follow, on standard error with the `error: ` line.

## EXIT STATUS

0 when the command did what was asked, including `-h`, and after a capture is
interrupted (Ctrl-C) having polled cleanly to that point.

0 as well for a run stopped by Ctrl-C before it had opened a board: see
ubertooth(7), SELECTING A DEVICE.

1 for a command line the tool will not take or with nothing to do, for a
capture file that cannot be created, when the selected Ubertooth is not found
or cannot be opened, when the board refuses the jam mode, a `-t` target, a
sniffing or faux slave mode, or an access address or CRC verify request, and
when a capture stops short because a poll failed. No device is opened for a
refused command line.

A poll the board does not answer is tried again, up to three attempts of one
second each, so one unlucky poll does not end a capture that has been running
for a quarter of an hour. A stop (Ctrl-C, or SIGTERM, SIGHUP or SIGQUIT) drops
the attempts still to come: the attempt already under way finishes, which costs
up to a second, and if it failed, that failure is reported (`error: USB poll
failed`) and the run exits 1, where a capture left to use its remaining
attempts might have recovered and exited 0.

1 as well when the board refuses the request that ends a capture: it is left
in the capture mode, and no other tool can open it until it is reset or
re-plugged. What was captured stays written out, and libubertooth names the
refused request on standard error.

A `-q`, `-c` or `-r` file is created once the board has been opened and its
firmware accepted, not before, so a run refused for either reason leaves no
empty capture behind. A file the tool then cannot create is reported with the
name the user gave, and the board is given back.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts

crackle(1): https://github.com/mikeryan/crackle

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

`ubertooth-btle` is Copyright (C) 2012-2018 Mike Ryan. This tool is
released under the GPLv2. Refer to COPYING for further details.
