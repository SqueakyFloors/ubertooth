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

Interfering with connections is a feature for causing intentional
interference with newly established or long-lived connections. When this
attack succeeds, the BLE connection between the central and peripheral
will be terminated. Pair the `-i` or `-I` flag with `-f` to interfere
with new connections or `-p` to interfere with long-lived connections.
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
   Promiscuous mode: sniff already-established connections
 - `-s <BD ADDR>` : 
   Inject advertising packets using specified BD ADDR

Interference (pair with `-f` or `-p`):

 - `-i` :
   Interfere with one connection and return to idle
 - `-I` :
   Interfere continuously with many connections
 - `-j` :
   Jam a single connection once, then return to idle
 - `-J` :
   Jam connections continuously

Filtering:

 - `-t <BD ADDR>` :
   Limit connection following and interference in follow mode to the
   specified BD ADDR

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
   it cannot corrupt the binary stream; errors remain on stderr. The tool
   refuses to write binary capture data directly to an interactive terminal:
   `error: stdout is a tty, refusing to write binary capture data to it`, with
   exit status 1 and before any device is opened.

Miscellaneous:

 - `-A <37|38|39>` :
   Which advertising channel to use in follow mode (default: 37)
 - `-a [address]` :
   Get (`-a`) or set (`-a <address>`) the access address used in promiscuous
   mode. The address is 1 to 8 hex digits (`-a 8e89bed6`); fewer than 8 means
   leading zeros (`-a 12345` sets `00012345`). Anything else is refused before
   the device is opened. Getting and setting in one command is refused; other
   options such as `-v` still run after a get.
 - `-v [01]` :
   Read (`-v`) or set (`-v 0` or `-v 1`) CRC verification
   (default: 0). Any other value is refused. A set and a read in one command
   both happen, the set first, so the read reports the new value, in whichever
   order they are written; two sets keep the last one. In a sniffing mode both
   run once the mode has started, because entering a mode clears the setting,
   and the firmware then turns verification on and off itself as it recovers a
   connection. A set or a read the board does not answer is reported on
   standard error with exit status 1.
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
`error: choose one of -f, -n or -p, pick one pal`, and a read and a set of the
access address in one command gives
`error: -a cannot both read and set the access address`.

A command line with no work in it is refused the same way. `-U`, `-A`, `-x`,
`-i`/`-I` (and their older spellings `-j`/`-J`) and the capture options only
shape work another option asks for, so a command made up of those alone, or a
bare `ubertooth-btle`, gives
`error: nothing to do: choose a major mode (-f, -n or -p), or -a, -v, -s or -t`,
followed by the usage text, with exit status 1 and before any device is
opened. Both go to standard error, with nothing on standard output, as after
any bad command line; `-h` prints the usage text on standard output.

A refused command line creates and truncates nothing: `-r`, `-q` and `-c` name
their file while the command line is read, and it is opened only once every
check has passed.

## SEE ALSO

crackle(1): https://github.com/mikeryan/crackle

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

`ubertooth-btle` is Copyright (C) 2012-2018 Mike Ryan. This tool is
released under the GPLv2. Refer to COPYING for further details.
