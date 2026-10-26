# ubertooth-follow(1) -- active (BlueZ) CLK discovery and follow for a particular UAP/LAP

## SYNOPSIS

    ubertooth-follow [-l LAP] [-u UAP] [-U <index|serial>] [-r file]
    [-q file] [-d file] [-e errors] [-a] [-b hciN] [-w delay]

## DESCRIPTION

Follows a classic Bluetooth (BR/EDR) piconet for a given target address.
It uses a local BlueZ Bluetooth device to assist with clock discovery and an
Ubertooth to follow the piconet's frequency hopping.

Both LAP and UAP identify the target. If they are not supplied, they are read
from the local Bluetooth device — note this may yield the wrong address in some
cases, so supplying the target LAP/UAP explicitly is recommended.

The capture is gated by the board's squelch, which this tool does not set: it
is the default -120 dBm unless `ubertooth-util -z` set another value while the
board was idle, and the firmware resets it on every mode change, so it never
carries over from an earlier capture. See ubertooth-util(1), SQUELCH.

Requires BlueZ (Linux only).

## OPTIONS

 - `-l <LAP>` :
   target LAP (3 bytes / 6 hex digits).
 - `-u <UAP>` :
   target UAP (1 byte / 2 hex digits).
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
   enable AFH (adaptive frequency hopping); experimental, and needs both `-l`
   and `-u`. See AFH below.
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

Because the map is read over a connection to the target, `-a` needs the target
named. `-a` without exactly one `-l` and one `-u` is refused before any file is
created or device opened, with
`error: -a reads the AFH map over a Bluetooth connection to the target, so it needs the target address: give -l once and -u once`.

`-l` and `-u` are not a whole Bluetooth address: the tool connects to
`00:00:<UAP>:<LAP>`, with the two most significant bytes zero, because a
sniffer cannot see them. So `-a` reaches only a target whose address begins
`00:00`. For any other target the connection fails and the command stops with
`error: connecting to 00:00:...: failed`.

The map that comes back is used only when it could be a hop sequence at all:

 - a failed read stops the command (`error: AFH map read failed: ...`), rather
   than following with whatever the map happened to contain. `-a` therefore
   does not work on a controller that does not implement
   HCI_Read_AFH_Channel_Map; run without `-a` on one of those;
 - a reply of AFH off is followed without it:
   `the remote device is not using AFH; following without it`. The board's AFH
   map is cleared in that case, and whenever `-a` is not given, so a map left
   by an earlier run cannot make it hop adaptively anyway;
 - a map with fewer than the 20 channels AFH requires, or none at all, is read
   again — three reads in total, 200ms apart — in case it has not settled. If
   it still reports too few, the command stops and says so;
 - a mode byte that is neither off nor on is refused and printed.

Nothing is set on the Ubertooth until the map has been accepted, so a refused
`-a` leaves the board alone, and the connection the tool made to the target is
dropped on the way out.

## EXAMPLES

Follow a target with a known LAP/UAP, writing a PCAP:

    ubertooth-follow -l 123456 -u 78 -q capture.pcap

Follow the same target with an AFH map (see AFH above):

    ubertooth-follow -l 123456 -u 78 -a

## ARGUMENTS

`ubertooth-follow` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: LAP must be 6 hex digits (got 4466)`.

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
long it lasts

ubertooth-rx(1), ubertooth-afh(1)

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
