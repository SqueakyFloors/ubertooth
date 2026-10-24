# ubertooth-ducky(1) -- drive an Uberducky to act like a USB Rubber Ducky

## SYNOPSIS

ubertooth-ducky [-q [uuid]] [-b] [-A index] [-a BD_ADDR] [-s]
[-U <index|serial>]

## DESCRIPTION

Companion tool for Uberducky, which uses an Ubertooth to inject BLE HID
("Rubber Ducky"-style keystroke) payloads. See the Uberducky project for the
payload format and setup:

    https://github.com/mikeryan/uberducky

If no Bluetooth address is given, a random BD_ADDR is used (read from
/dev/urandom).

## OPTIONS

 - `-q [uuid]` :
   quack — trigger the configured payload (optional target uuid).
 - `-b` :
   signal the Uberducky to enter bootloader (DFU) mode.
 - `-A <index>` :
   advertising channel index (default: 38).
 - `-a <BD_ADDR>` :
   Bluetooth address to use (default: random).
 - `-U <index|serial>` :
   select an Ubertooth by USB enumeration index or stable serial-number string.
 - `-h` :
   show help.

## TRANSMIT POWER

Injection advertises at the CC2400 power level set with `ubertooth-util -a` and
leaves it unchanged. A board that has not been given a level transmits at 7
(0 dBm), the CC2400's reset value.
These levels are at the CC2400 radio; the Ubertooth One's RF amplifier changes
the actual output after that.

## SEE ALSO

ubertooth-btle(1), ubertooth(7)

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
