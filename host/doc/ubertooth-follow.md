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
   enable AFH (adaptive frequency hopping); needs both `-l` and `-u`, see AFH
   below.
 - `-b <hciN>` :
   local Bluetooth device to use (default: hci0).
 - `-w <timeslots>` :
   USB delay in 625us timeslots (default: 5).
 - `-h` :
   show help.

## AFH

`-a` reads the target's AFH channel map from the target itself, over a
Bluetooth connection the local BlueZ device makes to it. It therefore needs the
target address: `-a` without both `-l` and `-u` is refused before any device is
opened, with
`error: -a reads the AFH map from the target over a Bluetooth connection, so it needs the target address: give both -l and -u`.

A map is used only when it is one the target can be hopping on:

 - a failed read stops the command (`error: AFH map read failed: ...`), rather
   than following with whatever the map happened to contain;
 - a target that reports AFH off is followed without it:
   `the remote device is not using AFH; following without it`;
 - a map that reports fewer than the 20 channels AFH requires, or none at all,
   is read again (up to three times, 200ms apart) in case it has not settled.
   If it still reports too few, the command stops and says so.

Either way nothing is set on the Ubertooth until the map has been accepted, so
a refused `-a` leaves the board alone.

## EXAMPLES

Follow a target with a known LAP/UAP, writing a PCAP:

    ubertooth-follow -l 123456 -u 78 -q capture.pcap

Follow the same target on its AFH hop sequence:

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

## SEE ALSO

ubertooth-util(1): sets the receive squelch (`-z`); SQUELCH describes how
long it lasts

ubertooth-rx(1), ubertooth-afh(1)

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
