# UBERTOOTH-FOLLOW 1 "Project Ubertooth" "User Commands"

## NAME

ubertooth-follow(1) - active (BlueZ) CLK discovery and follow for a particular UAP/LAP

## SYNOPSIS

ubertooth-follow [-l LAP] [-u UAP] [-U 0-7] [-r file] [-q file] [-e errors] [-a] [-b hciN] [-w delay]

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
   target LAP, in hexadecimal.
 - `-u <UAP>` :
   target UAP, in hexadecimal.
 - `-U <0-7>` :
   which Ubertooth device to use.
 - `-r <filename>` :
   capture packets to a PCAPNG file.
 - `-q <filename>` :
   capture packets to a PCAP file.
 - `-e <max_ac_errors>` :
   maximum access-code errors to tolerate.
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

## SEE ALSO

ubertooth-rx(1), ubertooth-afh(1), ubertooth(7)

## COPYRIGHT

Released under the GPLv2. Refer to `COPYING` for further details.
