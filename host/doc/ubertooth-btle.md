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
`-t` command line flag. For example, the following command will only
sniff connections where the central or peripheral's BD ADDR is
`22:44:66:88:AA:CC`:

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

Major modes:

 - `-f` :
   Follow mode: sniff connections as they are established
 - `-n` :
   No-follow mode: log advertising packets but don't follow connections
 - `-p` :
   Promiscuous mode: sniff already-established connections. As it works a
   connection out it reports each parameter it has recovered: the access
   address, the CRC init value, the hop interval and the hop increment. Those
   lines go to standard output, or to standard error once `-q -` or `-c -` has
   taken standard output, so a capture written to a file still says which
   connection it is of.
 - `-s <BD ADDR>` :
   Inject advertising packets using specified BD ADDR. Like `-a`, it does not
   go with `-f`, `-n` or `-p`. It takes the same spellings `-t` does, and
   advertises from the address it was given: the `/<bits>` mask and `none`
   mean nothing to a faux slave, so `-s none` advertises from
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
   specified BD ADDR. `-s` and `-t` are two different addresses and a command
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
   `-v`, the target lines of `-t`, and the connection parameters `-p`
   recovers. The tool
   refuses to write binary capture data directly to an interactive terminal:
   `error: stdout is a tty, refusing to write binary capture data to it`, with
   exit status 1 and before any device is opened.

The times in a logged capture follow the board's capture clock, which starts
over every 429.50 s in the `-f` and `-n` modes and every 327.68 s in
promiscuous mode. A promiscuous packet carries a count of how often it has,
which covers a quiet gap; the packets of the other two modes carry none, so a
gap longer than one period leaves every later time short by a period. That
cannot be recovered, but it is reported, on standard error: `warning: packet
times are behind the host clock by more than half the capture clock's 429.50 s
period: a quiet gap lost a clock wrap, or the host clock was stepped
forwards`. Both causes look the same from here, which is why the message names
each.

Miscellaneous:

 - `-A <37|38|39>` :
   Which advertising channel to use in follow mode (default: 37)
 - `-a [address]` :
   Get (`-a`) or set (`-a <address>`) the access address used in promiscuous
   mode. The address is 1 to 8 hex digits (`-a 8e89bed6`); fewer than 8 means
   leading zeros (`-a 12345` sets `00012345`). Anything else is refused before
   the device is opened. Getting and setting in one command is refused; other
   options such as `-v` still run after a get. `-a` does not go with a
   sniffing mode (`-f`, `-n`, `-p`), which runs until it is interrupted; the
   combination is refused before any device is opened.
 - `-v [01]` :
   Read (`-v`) or set (`-v 0` or `-v 1`) CRC verification
   (default: 0). Any other value is refused. A set and a read in one command
   both happen, the set first, so the read reports the new value, in whichever
   order they are written; two sets are refused, as two values for one setting
   are everywhere (ubertooth(7), OPTION VALUES). In a sniffing mode both
   run once the mode has started, because entering a mode clears the setting,
   and the firmware then turns verification on and off itself as it recovers a
   connection.
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

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: advertising channel index must be 37-39 (got 40)`.

A bare `-` is still the stdin/stdout value of `-q`, `-c` and `-r`. See
ubertooth(7), OPTION VALUES, for the forms an option's value can take,
including the optional values of `-a` and `-v`.

An option combination the tool cannot carry out is refused there too, before
any device is opened, so the message never depends on which boards are
attached: more than one of `-f`, `-n` and `-p` gives
`error: choose one of -f, -n or -p`, and a read and a set of the
access address in one command gives
`error: -a cannot both read and set the access address`.

`-a` or `-s` alongside a sniffing mode gives
`error: -a and -s do not go with -f, -n or -p: a sniffing mode runs until it
is interrupted`, so there is nothing for an access address or faux slave mode
request to run after. `-t` and `-v` are not in that list: `-t` is sent before
the mode starts, which is what it is for, and the `-v` options run once it has
started.

`-i` or `-I` without `-f` or `-p` gives
`error: -i needs -f or -p: there is no connection to interfere with
otherwise`, naming the spelling that was used (`-j` and `-J` included).

That includes `-i` on its own: it says more than listing every major mode
would, because only two of them can carry interference.

A command line with no work in it is refused the same way. `-U`, `-A`, `-x`
and the capture options only shape work another option asks for, so a command
made up of those alone, or a bare `ubertooth-btle`, gives
`error: nothing to do: choose a major mode (-f, -n or -p), or -a, -v, -s or -t`,
on standard error with nothing on standard output. A bare `ubertooth-btle` is
the one refusal the usage text follows, also on standard error; every other
refused command line is the `error: ` line alone, and `-h` prints the usage
text on standard output.

A refused command line creates and truncates nothing: `-r`, `-q` and `-c` name
their file while the command line is read, and it is opened only once every
check has passed.

## DIAGNOSTICS

Standard output carries only what was asked for: either the decoded packets or
the capture data of `-q -` or `-c -`, plus the lines that answer `-a`, `-v` and
`-t` and the parameters `-p` recovers. With the capture on standard output,
standard output belongs to the capture for the whole run: those lines move to
standard error, and the decoded packets are not printed at all.

Errors, warnings and progress go to standard error, each error as one line
starting `error: `, for example `error: USB poll failed` when the board stops
answering during a capture, `error: jamming not supported by this firmware`,
`error: target set failed`, or
`error: target address must be xx:xx:xx:xx:xx:xx (got 11:22:33:44:55)`. A value
that is not an address reads the same way whatever is wrong with it, and names
the address the option sets, so `-s` gives
`error: faux slave address must be xx:xx:xx:xx:xx:xx (got <word>)` and a bad
mask gives `error: target mask must be 0-48 (got 49)`. A capture
file that cannot be created is reported as `error: <file>: <reason>`.

A capture that is losing packets says so. This tool reads its packets one at a
time, and a reply that held part of a record cannot be read as one, so it is
dropped and reported once per run: `warning: a poll reply arrived with 32 of a
packet's 64 bytes and was dropped; the capture is losing packets`. The capture
runs on and ends as it otherwise would, so the warning is the only sign.

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

1 for a command line the tool will not take or with nothing to do, for a
capture file that cannot be created, when the selected Ubertooth is not found
or cannot be opened, when the board refuses the jam mode, a `-t` target, a
sniffing or faux slave mode, or an access address or CRC verify request, and
when a capture stops short because a poll failed. No device is opened for a
refused command line.

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts

crackle(1): https://github.com/mikeryan/crackle

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

`ubertooth-btle` is Copyright (C) 2012-2018 Mike Ryan. This tool is
released under the GPLv2. Refer to COPYING for further details.
