# ubertooth-follow(1) -- active (BlueZ) CLK discovery and follow for a particular UAP/LAP

## SYNOPSIS

ubertooth-follow [-l LAP] [-u UAP] [-U 0-7] [-r file] [-q file] [-d file] [-e errors] [-a] [-b hciN] [-w delay]

## DESCRIPTION

Follows a classic Bluetooth (BR/EDR) piconet for a given target address.
It uses a local BlueZ Bluetooth device to assist with clock discovery and an
Ubertooth to follow the piconet's frequency hopping.

Both LAP and UAP identify the target. If they are not supplied, they are read
from the local Bluetooth device — note this may yield the wrong address in some
cases, so supplying the target LAP/UAP explicitly is recommended.

Requires BlueZ (Linux only).

## OPTIONS

 - `-l <LAP>` :
   target LAP (3 bytes / 6 hex digits).
 - `-u <UAP>` :
   target UAP (1 byte / 2 hex digits).
 - `-U <0-7>` :
   which Ubertooth device to use.
 - `-r <filename>` :
   capture packets to a PCAPNG file.
 - `-q <filename>` :
   capture packets to a PCAP file.
 - `-d <filename>` :
   dump received symbols to the named file.
 - `-e <0-5>` :
   maximum access-code errors to tolerate (default: 2).
 - `-a` :
   enable AFH (adaptive frequency hopping).
 - `-b <hciN>` :
   local Bluetooth device to use (default: hci0).
 - `-w <timeslots>` :
   USB delay in 625us timeslots (default: 5).
 - `-h` :
   show help.

## EXAMPLES

Follow a target with a known LAP/UAP, writing a PCAP:

    ubertooth-follow -l 0x123456 -u 0x78 -q capture.pcap

## ARGUMENTS

`ubertooth-follow` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: LAP must be 6 hex digits (got 4466)`.

## SEE ALSO

ubertooth-rx(1), ubertooth-afh(1), ubertooth(7)

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
