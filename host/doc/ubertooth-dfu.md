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

Every wait for the bootloader is bounded. Before each operation the tool asks
the bootloader for its DFU state and drives it to the idle state, aborting a
half-finished transfer or clearing an error state as needed. A bootloader that
does not answer that question, or that never becomes idle, ends the command
with a line naming what it last reported and exit status 1; a board that
answers nothing at all usually needs a USB power cycle.

`-d` checks the whole image before the board is asked to leave its
application: the 16-byte DFU suffix and its length, the DFU version, the `UFD`
signature, a non-empty payload, the suffix CRC, and the suffix `idVendor` and
`idProduct`, which must name this device (`1d50:6003`) or be the `0xffff`
wildcard. Then the firmware info block every image built from this tree
carries, at a fixed offset just past the vector table, which says which image
the file holds and which revision it was built from:

    Checking firmware signature
    Image: bluetooth_rxtx git-ed30131d* (API:<version>)

The API version on that line comes from the block, so for `btbr` it reads this
tree's version while `ubertooth-util -v` reports that board as 1.06: the `btbr`
image is built from this tree but reports a fixed USB `bcdDevice` of its own,
which is what the other tools check. This tree's version moves with the
firmware, so the samples here write it as `<version>`; `COMPATIBILITY.md` names
the value this tree requires.

A properly signed DFU file with no readable block there is not an image from
this tree, so it is refused rather than written:

    error: other-device.dfu: there is no firmware info block here at offset 0x100; firmware will not be updated

A file that fails any of these is reported and the command ends
with exit status 1 without opening a device, so the board keeps running its
firmware. If a later step fails once the board is already in DFU and `-r` was
given, the tool detaches it anyway, reopening the DFU device on the same
physical path when the failure closed the handle, so the board returns to its
application. A board that never answered in DFU at all cannot be detached, and
then needs another `ubertooth-dfu` run or a power cycle. A reset the bootloader
refuses, and one it accepts and then does not act on, are both reported and
fail the command instead of exiting 0 with the board still in DFU.

`-d` is bounded by the flash the board has as well. The part identification
number is read while the board is still running its application, as it is for
`-u`, and an image whose blocks would not all fit above the bootloader is
refused there, before the board is asked for DFU:

    error: too-big.dfu needs 114944 bytes of flash; part 0x25011722 (LPC1754) has 114688 above the bootloader; firmware will not be updated

An image that fits says so, in the same terms as the read-back (progress, so
on standard error, like every line quoted in this page except `Verified
firmware revision`):

    The image needs 31488 bytes, 0x4000-0xbaff: part 0x25011722 (LPC1754) has 128 KiB of flash

The whole file is written, the 16-byte DFU suffix included, and the last block
is padded out, so the figure compared with the part's flash is the file rounded
up to a whole 256-byte block. A part this host has no entry for, and one that
will not report itself at all, which includes every board already in the
bootloader, leave nothing to check against: both say so and write the image as
before.

That check is the only one this tool can act on, because it never reads the
board's answer after the stall. A bootloader built from this tree refuses a
block with no flash behind it, bounded the same way as the read-back described
under READING FLASH BACK below, so it stops at the top of the part's flash and
says `ERRADDRESS` for the blocks above it; older bootloaders have no upper
bound and instead ask the microcontroller's programming interface to erase and
program a sector the part does not have, which fails. Either way everything
sent before that point is already written, and all this tool sees of the
refusal is the stall, which it reports as
`error: control message unsupported`: the
board is left running an application missing its tail, after a message that
says nothing about flash sizes. It is the same cause as the read-back bound:
asking for flash the part has not got.

What the detach request returns says nothing about where the board ended up,
in either direction. It can fail because the bootloader resets while the
request is still in flight, which on macOS is what every reset that worked
looks like, and it can complete on a bootloader that acknowledges it and stays
where it is. So no status from that request is read as a verdict: the tool
looks for the DFU device on the same physical path again, and reports the
board as detached only once it has gone. The run then carries on to the
`-d ... -r` verification, which decides whether the flash succeeded.

The question that check asks is whether the device left its path, not where it
ended up, so it has a blind spot.

A board that resets straight back into the bootloader, with the
expansion-header bridge described under MANUALLY ENTERING BOOTLOADER still in
place, may be reported as detached: whether the gap in which it is absent falls
inside one of the 50 ms polls decides the answer. With `-d ... -r` the
verification that follows catches it, because no application comes back on the
expected revision; a bare `-r` or `-u ... -r` has only this check.

If a transfer fails, in either direction, the tool closes the stale handle,
reopens and claims the DFU device on the same physical path, restores its DFU
state, and retries the whole transfer once from its first block. A transfer
that fails twice ends the command with exit status 1. With `-d ... -r`,
the revision from the image's firmware info block is what the board must
report back: the run does not return success until the application on that
same path reports exactly that revision.

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

`-s` replaces `<file>.dfu` only once the whole signed image is ready, writing
it through a temporary file beside the target as `-u` does for its dump (see
[READING FLASH BACK][]). If the signing does not get that far, for any reason
(an input it refuses, a `-d` image refused in the same command, a read or a
write that fails), an earlier `<file>.dfu` is left untouched and none is
created; whichever way the command ends on its own it leaves no temporary file
behind, and only a signal that kills it can. The image it writes is the one it
always wrote: the input, unchanged, with the 16-byte suffix appended, keeping
the permissions of the file it replaced.

What happens after the signed file is in place is a separate matter. `-s`
alongside `-d`, `-u` or `-r` signs first and then goes to the board, so a
command that fails there, with a device it cannot open or a flash that does
not take, exits 1 with `<file>.dfu` already written.

Writing it that way means `-s`, like `-u`, needs to be able to create a file in
the output's directory, not just write the output itself: signing into a
read-only directory fails even where the `<file>.dfu` it would replace is
writable. The new file is put in the target's place rather than written through
it, so a `<file>.dfu` that is a symlink, or one with other hard links to it, is
replaced by the signed image instead of having the image written into what it
points at.

Both read their whole file into memory, so what the file is comes first. A
directory, a FIFO or a device node is refused (`error: <file> is not a regular
file`): its length says nothing about what a read would return. So is a file
too large to be an image (`error: <file> is <n> bytes; an Ubertooth image is
at most 507920`). `-d` takes at most 507920 bytes, which is the flash above
the bootloader on the largest part this host knows plus the 16-byte suffix,
and `-s` takes 507904, the same less the suffix it adds, so the file it writes
is one `-d` still accepts. Either refusal ends the command with exit status 1
before any device is opened, and with `-s` before the output file is created.

A command that gives both `-s` and `-d` checks both files it only reads before
it creates anything, so an image it refuses costs the signed file nothing:

    $ ubertooth-dfu -s firmware.bin -d not-an-image.dfu
    Checking firmware signature
    error: not-an-image.dfu: Unknown DFU suffix length: 115; firmware will not be updated

`firmware.dfu` is neither created nor replaced.

In the DFU bootloader, the TX, RX, and USR LEDs will turn on and off in
sequence. If this does not occur, you can try to manually force the
device to boot into the bootloader. Refer to [MANUALLY ENTERING
BOOTLOADER][].

## OPTIONS

Major modes:

 - `-d <firmware.dfu>` :
   Download - write DFU file to device
 - `-u <output.bin>` :
   Upload - read the application flash region, as much of it as the part has,
   into a raw file (see [READING FLASH BACK][])
 - `-r` :
   Reset device after performing operation

Miscellaneous:

 - `-s <file.bin>` :
   Add DFU suffix to binary firmware file
 - `-U <index|serial>` :
   select the application-mode Ubertooth by enumeration index or serial
   number; see ubertooth(7), SELECTING A DEVICE

## READING FLASH BACK

`-u <output.bin>` is not the inverse of `-d`: it writes a raw dump of the
application flash region, from 0x4000 to the end of the part's flash, with no
DFU suffix, so `-d` refuses the result. The dump holds the image that was
written (its own suffix included, padded with 0xff to the next 256-byte
block), followed by whatever the rest of the region contains, normally 0xff.

How long that is depends on the part, which differs between boards, so the
command says what it is about to read, on standard error with the rest of its
progress:

    Reading back 114688 bytes, 0x4000-0x1ffff: part 0x25011722 (LPC1754) has 128 KiB of flash

A part with 128 KiB of flash gives 114688 bytes, one with 256 KiB gives 245760.
The size comes from the part identification number, which is only readable
while the board is still running its application: when the board is
already in the bootloader, or will not report it, the dump covers the 114688
bytes above the bootloader that the firmware is linked for, and the line says
so in place of a part. There is no option for a longer dump, so on a larger
part that was already in the bootloader the shorter dump is all there is; let
the tool enter DFU itself to get the whole region. The assumption has a limit
the other way too: a board with a smaller part than that, already in the
bootloader so its part cannot be read, would still be asked for more flash
than it has. A bootloader built from this tree refuses such a request and
carries on, but a board carries whatever bootloader was flashed on it, and an
older one cannot survive being asked: it answers a read from any address below
512 KiB, and a read with no flash behind it faults it until the board is
power-cycled.

To compare a dump with the image it came from, compare the first bytes and
check that the rest is padding, for example:

    ubertooth-dfu -U <serial> -u readback.bin -r
    cmp -n $(stat -f%z bluetooth_rxtx.dfu) bluetooth_rxtx.dfu readback.bin

On Linux use `stat -c%s`. The output file is only replaced once a complete dump
has arrived: a read-back that fails part-way leaves an earlier file of the same
name untouched, names the block that would not read, and exits 1. Like `-d`, it
is retried once from the first block before the command gives up. Whichever way
the command ends on its own, including a `-d` image it refuses, it leaves no
temporary file beside the output; only a signal that kills it can.

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
alongside it. `-h` is what the user asked for, so its usage text goes to
standard output and it exits 0.

An option the tool does not know is refused the same way, as one line on
standard error with no usage text after it: `error: unrecognized option '-Z'`.
An option whose value is missing reads `error: option '-d' requires a value`.

A file the tool cannot open, and an image it will not write, are each refused
with one line on standard error, beginning `error: ` and naming the file. The
`Checking firmware signature` line that precedes an image refusal is progress,
so it is on standard error as well and the two stay in order whatever either
stream is piped into.

A command line with no function in it does nothing, so it is refused with
`error: nothing to do: give -d to write an image, -u to read one back, -s to
sign one, or -r to reset` on standard error and exit status 1, before any
device is opened; it used to exit 0 having said and done nothing. A bare
`ubertooth-dfu`, with no arguments at all, prints the usage text after that
line; every other refused command line is the `error: ` line alone.

A refused command line creates and truncates nothing, and leaves no temporary
file: `-s`, `-u` and `-d` name their file while the command line is read, and
the files are opened only once every check has passed. The files that are only
read, the `-d` image and the `-s` input, are opened and checked first, since
they are the last things that can refuse the command; neither output file is
created until they have passed. Whether `-s` was given a `.bin` is a check on
the name, so it is still reported alongside `-h`.

## EXIT STATUS

0 when the command did what was asked, including `-h`: the image written and,
with `-r`, verified on the board that came back; the dump complete and in
place; the signed `<file>.dfu` written; the board out of the bootloader.

1 for everything else, and there is no other status. That covers a command
line the tool will not take or that has nothing to do, a file it cannot read
or create, an image it refuses, a selected board that is missing or that
something else is using, a bootloader that will not answer or will not become
idle, a transfer that failed both its attempts, a reset that left the board in
DFU, and a revision after the reset that is not the image's.

## DIAGNOSTICS

Standard output carries the usage text for `-h`, and otherwise one line, from
`-d <image> -r` alone:

    Verified firmware revision: git-ed30131d

That is the revision the board reported once it was running the new firmware,
which is the only thing this command reads back, so it is the result and a
script can read it on its own. Nothing else is ever written to standard
output: the dump of `-u` goes to the file it names, and the signed image of
`-s` to `<file>.dfu`, and a run that only writes firmware or resets the board
says nothing there at all.

Everything else is on standard error, so a script reading that one line is not
fed the progress of a flash that takes a few seconds:

    Checking firmware signature
    Image: bluetooth_rxtx git-ed30131d (API:<version>)
    DFU expected revision: git-ed30131d
    The image needs 31488 bytes, 0x4000-0xbaff: part 0x25011722 (LPC1754) has 128 KiB of flash
    Switching selected device to DFU mode...
    ........................................
    Detached

`-u` prints `Reading back <n> bytes, ...` there in the same way, with one dot
per 256-byte block read, and `DFU +<seconds>s: <stage>` lines trace what the
command spent its time on.

The errors this tool prints itself are one line each beginning `error: `, for
example `error: detach failed: the board is still in DFU`,
`error: upload failed: block 164 was not read`,
`error: firmware update failed after 2 attempt(s): ...` and
`error: control message unsupported`; the lines about a part this host cannot
identify begin `warning: `. Lines from elsewhere do not follow that yet: the
libusb status reports (`libUSB Error: ...`) and the libubertooth messages that
have not been converted, such as `could not open Ubertooth device`.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Mike Ryan.

## COPYRIGHT

ubertooth-dfu(1) is Copyright (c) 2010-2017. This tool is released under
the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
