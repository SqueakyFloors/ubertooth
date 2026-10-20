# ubertooth-ducky(1) -- drive an Uberducky to act like a USB Rubber Ducky

## SYNOPSIS

ubertooth-ducky [-q [uuid]] [-b] [-A index] [-a BD_ADDR] [-s] [-U 0-7]

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
 - `-s` :
   Be sarcastic (easter egg; changes the tool's output tone).
 - `-U <0-7>` :
   which Ubertooth device to use.
 - `-h` :
   show help.

## SEE ALSO

ubertooth-btle(1), ubertooth(7)

## COPYRIGHT

Released under the GPLv2. Refer to `COPYING` for further details.
