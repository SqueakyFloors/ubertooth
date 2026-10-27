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

One of `-l` and `-u` without the other names no piconet, and the tool does not
refuse it today: it reads the address from the local Bluetooth device as if no
target had been given, and follows that instead of what was asked for.

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
   target LAP (3 bytes / 6 hex digits), given once. Not with `-t`.
 - `-u <UAP>` :
   target UAP (1 byte / 2 hex digits), given once. Not with `-t`.
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-r <filename>` :
   capture packets to a PCAPNG file.
 - `-q <filename>` :
   capture packets to a PCAP file.
 - `-d <filename>` :
   dump received symbols to the named file.
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
connection fails and the command stops with
`error: connecting to 00:00:...: failed`. Give the target as `-t` and the
connection is made to the address as it is.

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
address really does begin `00:00`; against any other it stops with
`error: connecting to 00:00:33:44:55:66 failed`. Use `-t` for a real target:

    ubertooth-follow -l 445566 -u 33 -q capture.pcap

## ARGUMENTS

`ubertooth-follow` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

An option the tool does not know is refused the same way, as one line on
standard error with no usage text after it: `error: unrecognized option '-Z'`.
An option whose value is missing reads `error: option '-l' requires a value`.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: LAP must be 6 hex digits (got 4466)` and
`error: target address must be xx:xx:xx:xx:xx:xx (got 11:22:33:44:55)`.

Two more kinds of command line are refused before any device is opened, each
with its one line:

 - `-t`, `-l` or `-u` given twice, `error: give -t once`, `error: give -l once`,
   `error: give -u once`: the tool does not pick between two answers to the
   same question;
 - `-t` with `-l` or `-u`,
   `error: -t gives the whole target address, so it cannot be combined with -l or -u`.

What the tool is following — the target address, or the LAP and UAP it read
from the local device — is reported on standard error, with the rest of the
tool's progress. Standard output carries the decoded packets, and, with `-a`,
the accepted channel map, which libbtbb prints itself.

A refused command line creates and truncates nothing: `-r`, `-q` and `-d` name
their file while the command line is read, and it is opened only once every
check has passed.

## EXIT STATUS

A follow that was interrupted (Ctrl-C) exits 0 with whatever it captured, and
so does `-h`. A refused command line exits 1 (see ARGUMENTS), as does a file
that cannot be opened and a target the tool could not set up a follow on.

A follow whose USB stream fails part-way also exits 1: the board stopped
answering, or dropped off the bus, so the capture stops there and holds only
what arrived before it. The failure is reported on standard error as it
happens, as `error: receive transfer failed: <reason>`. A board that never
hands the transfer back at all exits 1 too, reported by the `warning:` line
ubertooth(7) describes rather than that error.

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts, and CARRY-OVER BETWEEN RUNS what else a board keeps from one
command to the next

ubertooth-rx(1), ubertooth-afh(1)

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
