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

Note: the `-a` option (initial AFH map) is currently non-functional; its
handler is disabled in the source and the flag has no effect.

## OPTIONS

 - `-l <LAP>` :
   LAP of target piconet (3 bytes / 6 hex digits)
 - `-u <UAP>` :
   UAP of target piconet (1 byte / 2 hex digits)
 - `-m <int>` :
   threshold for channel removal: a channel is marked unused once this many packets
   arrive without it being seen; 1 or more (default: 5)
 - `-r` :
   print AFH channel map once every second (default: print on update)

Other options:

 - `-t <seconds>` :
   timeout for initial AFH map detection (not required); see DETECTION AND
   MONITORING. No effect with `-r`.
 - `-e <0-5>` :
   maximum access code errors (default: 2, range: 0-5)
 - `-V` :
   print version information
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE

## DETECTION AND MONITORING

`ubertooth-afh` does not stop on its own. It keeps monitoring the piconet and
reporting changes to the map until it is interrupted (Ctrl-C), both with and
without `-t`.

While it listens, the board hops across the channels the piconet has not been
seen on, and the tool tells the board to stop visiting a channel once a packet
from the piconet has arrived there. That is how the map fills in: a channel is
reported used only when a packet is received on it.

`-t <seconds>` only adds an initial detection phase: the tool listens for that
many seconds, prints the map it found, and then starts monitoring. If that
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
once, waits as long as it takes, and prints `+ channel N is used now` as the
piconet's packets arrive. `-t` is for a bounded check that gives up when
nothing is heard in time.

`-r` has its own loop, which prints the map once a second from the start and
ignores `-t`.

## ARGUMENTS

`ubertooth-afh` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: timeout must be a whole number of seconds, 0 for none (got abc)`.

Both halves of the piconet address are required, and a command line missing
either is refused before any device is opened:
`error: -l <LAP> and -u <UAP> are both required: the map is detected for one
piconet`.

## DIAGNOSTICS

Standard output carries only the detected channel maps. Errors, warnings and
progress go to standard error, each error as one line starting `error: `, so a
script reading the maps sees nothing else.

A command line the tool will not take is reported as that one `error: ` line
followed by the usage text, both on standard error. An option the tool does
not know reads `error: unrecognized option '-Z'`; one whose value is missing
reads `error: option '-l' requires a value`.

`-h` is what the user asked for, so the usage text goes to standard output and
nothing goes to standard error.

## EXIT STATUS

`ubertooth-afh` does not stop on its own, so it normally exits 0 after being
interrupted (Ctrl-C). `-h` also exits 0.

It exits 2 when the `-t` detection phase received no packet from the piconet
(see DETECTION AND MONITORING). The number of seconds in that message is the
`-t` value, not the time actually spent listening, which is shorter if the
tool was interrupted during the phase.

It exits 1 without opening a device for a bad command line (a missing UAP or
LAP, an option value out of range, or a word that is not an option), and when
the selected Ubertooth is not found. Without `-r`, it also exits 1 when the
receive could not be started at all; `-r` does not report that and still
exits 0.

## SEE ALSO

ubertooth-rx(1): a tool for finding UAP and LAP

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-afh(1) is Copyright (c) 2015-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
