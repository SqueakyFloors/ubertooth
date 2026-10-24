# ubertooth-dfu(1) -- Device firmware update for Ubertooth

## SYNOPSIS

To update firmware, run:

    ubertooth-dfu -d bluetooth_rxtx.dfu -r

## DESCRIPTION

ubertooth-dfu(1) is a utility for updating firmware on Ubertooth. It can
also download the currently running firmware from the device.

Running the upload or download command automatically resets the device
into the firmware update bootloader. The USB ID of the Ubertooth will
change in the bootloader, and this may cause problems when attempting to
update firmware from inside a VM.

The application-to-DFU transition checks the device command, closes the
application handle without sending a conflicting stop command, and follows
the selected device by its physical USB bus/port path. If DFU does not appear,
the tool makes two bounded five-second entry attempts when the application
device is available again. Flushed monotonic stage timing identifies time spent
in entry, programming, reset, and cleanup. It refuses to choose between
multiple devices already in DFU mode.

`-d` checks the whole image before the board is asked to leave its
application: the 16-byte DFU suffix and its length, the DFU version, the `UFD`
signature, a non-empty payload, the suffix CRC, and the suffix `idVendor` and
`idProduct`, which must name this device (`1d50:6003`) or be the `0xffff`
wildcard. With `-r` the expected firmware revision is read from the image at
the same time. A file that fails any of these is reported and the command ends
with exit status 1 without opening a device, so the board keeps running its
firmware. If a later step fails once the board is already in DFU and `-r` was
given, the tool detaches it anyway, reopening the DFU device on the same
physical path when the failure closed the handle, so the board returns to its
application. A board that never answered in DFU at all cannot be detached, and
then needs another `ubertooth-dfu` run or a power cycle.

If a firmware programming transfer fails, the tool closes the stale handle,
reopens and claims the DFU device on the same physical path, restores its DFU
state, and retries the complete image once from block zero. With `-d ... -r`,
it extracts the expected revision from the image and does not return success
until the application on that same path reports the exact revision. Images
used with automatic verification must contain the normal
`ubertooth <revision> (` compile-information marker.

With multiple Ubertooths, flash them sequentially using `-U <serial>` and
verify each device before starting the next. DFU mode does not expose a serial
number, so an explicit selector never adopts an unidentified device that was
already in DFU when the command started. Simultaneous flashing is not yet
supported or validated.

`-s` and `-d` only read the file they are given, so a firmware image in a
read-only location can be signed or flashed. `-s <file.bin>` writes
`<file>.dfu`, and refuses an input that already ends in a DFU suffix: signing
it again would bury the first suffix in the firmware payload. Nothing is
written in that case.

In the DFU bootloader, the TX, RX, and USR LEDs will turn on and off in
sequence. If this does not occur, you can try to manually force the
device to boot into the bootloader. Refer to [MANUALLY ENTERING
BOOTLOADER][].

## OPTIONS

Major modes:

 - `-d <firmware.dfu>` :
   Download - write DFU file to device
 - `-u <output.bin>` :
   Upload - read the application flash region into a raw file (see [READING
   FLASH BACK][])
 - `-r` :
   Reset device after performing operation

Miscellaneous:

 - `-s <file.bin>` :
   Add DFU suffix to binary firmware file
 - `-U <index|serial>` :
   Select the application-mode Ubertooth by enumeration index or serial number

## READING FLASH BACK

`-u <output.bin>` is not the inverse of `-d`: it writes a raw 245760-byte
(240 KiB) dump of the application flash region, 0x4000 to 0x3ffff, with no DFU
suffix, so `-d` refuses the result. The dump holds the image that was written
(its own suffix included, padded with 0xff to the next 256-byte block),
followed by whatever the rest of the region contains, normally 0xff.

To compare a dump with the image it came from, compare the first bytes and
check that the rest is padding, for example:

    ubertooth-dfu -U <serial> -u readback.bin -r
    cmp -n $(stat -f%z bluetooth_rxtx.dfu) bluetooth_rxtx.dfu readback.bin

On Linux use `stat -c%s`. The output file is only replaced once a complete dump
has arrived: a read-back that fails part-way leaves an earlier file of the same
name untouched.

## MANUALLY ENTERING BOOTLOADER

If the device does not enter the bootloader when running the upload or
download commands, it is possible to force the device to boot into that
mode when it is inserted. To do so, you will need to jumper two pins on
the expansion header.

The expansion header is a 2x3 pin header between the two largest chips
on the board, about halfway between the USB connector and antenna. It is
labeled "EXPAND" on the back side of the board. Holding the board so USB
is pointed up, on the front side of the board the expansion header looks
like this:

      +-------+
    6 + O | O | 3
      +---+---+
    5 + O | O | 2
      +---+---+
    4 + O | X | 1
      +---+---+

PIN #1, marked X, is square.

With Ubertooth unplugged, bridge pins 1 and 3, then plug the device in
while keeping the bridge in place. Wait for the distinctive LED sequence or
confirm that USB ID `1d50:6003` is present. The bootloader samples the entry pin
only during reset, so once DFU is active you may remove the bridge without
exiting DFU. Run `ubertooth-dfu` promptly because pin-entry DFU times out after
a few seconds if no DFU request begins.

Remove the bridge before the final reset if you want the application firmware
to boot. If it remains installed, the next reset will sample the entry pin
again and return to the bootloader. If removing a live bridge is inconvenient,
finish the flash, unplug the device, remove the bridge, and reconnect it.

## ARGUMENTS

`ubertooth-dfu` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-dfu(1) is Copyright (c) 2010-2017. This tool is released under
the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
