# ubertooth-btbr(1) -- experimental Classic Bluetooth connections and monitoring

## SYNOPSIS

    ubertooth-btbr [-U <index|serial>] [-f <freq_offset>] [-b <max_ac_errors>]
    ubertooth-btbr -h

## DESCRIPTION

ubertooth-btbr(1) takes part in Classic Bluetooth itself, rather than only
listening: it can run an inquiry, answer one, page a device, be paged, and
watch a connection being set up. It opens an interactive prompt, `bt> `, and
each command there starts one operation on the board; `?` lists them.

It is **experimental**, and it needs the separate `btbr` firmware image. The
other tools, including ubertooth-rx(1) and ubertooth-btle(1), need
`bluetooth_rxtx`, so a board is set up for one or the other and switching
means reflashing it. See FIRMWARE below.

At startup the tool gives the board the address `11:22:33:44:55:66` and the
local name `Ubertooth`, which is what an inquiring device sees in the Extended
Inquiry Response (EIR). Change the address with the `bdaddr` command.

The host side is the Python module `ubtbr`, which also implements enough of
the Link Manager Protocol (LMP) to set up an unencrypted connection.

## FIRMWARE

Flash the `btbr` image, from a build of `firmware/btbr` or from a release
archive:

    ubertooth-dfu -d firmware/btbr/btbr.dfu -r

and flash `bluetooth_rxtx.dfu` again to go back to the other tools:

    ubertooth-dfu -d firmware/bluetooth_rxtx/bluetooth_rxtx.dfu -r

Add `-U <index|serial>` to either command to pick one board of several; it
works from both images.

Starting `ubertooth-btbr` on a board running `bluetooth_rxtx` fails with
`error: could not set up Ubertooth <serial>: LIBUSB_ERROR_TIMEOUT [-7]`,
followed by a `note: ` naming that firmware and the image to flash.
`bluetooth_rxtx` offers the endpoint this tool writes to but never reads it,
so the tool's first requests sit there until the write times out. The serial
in that line is the board the session got, which is worth comparing with the
one `btbr` was flashed onto.

## OPTIONS

 - `-U <index|serial>` :
   which Ubertooth to use. See SELECTING A DEVICE.
 - `-f <0-65535>` :
   frequency offset, written to the radio's `MOD_OFFSET` register. Sent to the
   board before the prompt opens, and only when given; a run without it leaves
   whatever the firmware set.
 - `-b <0-65535>` :
   how many bit errors a received access code may have and still count as a
   match. A higher value finds more packets and more noise.
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value or a stray word
   is still reported.

Each option may be given once. See DIAGNOSTICS for what a command line it
will not take does.

## SELECTING A DEVICE

`-U <index|serial>` chooses one of several attached boards, as described in
ubertooth(7), SELECTING A DEVICE: an index is a position in the list of
attached Ubertooths (`-U 0` is the first), and a serial-number string selects
that board whatever the enumeration order. A serial may be shortened to its
first few characters, in either case, and an all-digits string is read as an
index, so include a non-digit (`-U 05f` rather than `-U 05`). If those
characters start more than one attached board's serial, the tool lists the
matching serials and selects neither; boards reporting the same serial are
told apart by index instead.

An index names a board without reading any serial, so it disturbs none of the
others. Matching a serial has to read every attached board's, which means
opening each one briefly; a board that does not answer is counted in the
refusal, because the serial given may well be its own.

With more than one board attached and no `-U`, the tool lists them and opens
none:

    error: multiple Ubertooth devices found; select one with -U <index|serial>
      Device 0: 1cc001038088cb956c43ad61c52000f5
      Device 1: 346001058088cb95345bad61c52000f5

It used to open whichever board libusb handed over first, which on such a
host reached an arbitrary one: a board flashed with `btbr` could be ignored
in favour of one still carrying `bluetooth_rxtx`, whose write timeout then
looked like the `btbr` board failing.

A board whose serial number cannot be read is listed as `serial unreadable`
and can still be chosen by index. Reading it means opening the board, so what
makes it fail is an open that fails — on Linux, usually the udev rules not
being installed — or a board that has left the bus. A board whose interface
another program has claimed reads its serial normally and fails later, when
the session starts.

## PROMPT COMMANDS

An address is written `11:22:33:44:55:66` or `112233445566`.

 - `inquiry` :
   discover nearby devices; each response is reported with its EIR
 - `inquiry_scan` :
   answer inquiries, so nearby devices discover this board
 - `page_scan` :
   accept a connection from a device that pages this board
 - `discoverable` :
   `inquiry_scan` and `page_scan` together
 - `page <BD_ADDR>` :
   connect to a device as the central, and keep the connection up
 - `l2ping <BD_ADDR>` :
   page a device and then exchange Logical Link Control and Adaptation
   Protocol (L2CAP) echo packets with it
 - `monitor <BD_ADDR>` :
   watch for a connection being set up to the given peripheral; see MONITOR
   MODE
 - `bdaddr <BD_ADDR>` :
   set this board's address
 - `idle` :
   stop the running command and return the board to standby
 - `exit`, `q`, `x`, Ctrl-D :
   leave the tool
 - `?`, `? <command>` :
   list the commands, or describe one

One command runs at a time. Starting another while one is still running is
refused with a `Not idle` line; `idle` or Ctrl-C stops the running one first.

Ctrl-C at the prompt stops the running command, as `idle` does, and leaves the
prompt up; pressing it again while that command is stopping returns to the
prompt. Before the prompt is up, Ctrl-C ends the tool.

## MONITOR MODE

`monitor` targets the paging phase of a connection, where the central sends
the peripheral a Frequency Hop Synchronization (FHS) packet carrying its full
address and clock. If the Ubertooth receives that FHS before the real
peripheral does, it joins the new piconet ahead of both devices and reports the
packets they exchange, including the unencrypted LMP traffic that opens every
connection. An LMP packet that sets the Adaptive Frequency Hopping (AFH) map
is followed, so the capture keeps up when the piconet narrows its channels.

Give it the address of the peripheral being connected to, then make the
connection from the other device:

    ubertooth-btbr
    bt> monitor 11:22:33:44:55:66

If no FHS appears, the peripheral won the race; try again.

## LIMITATIONS

- The CC2400 radio demodulates Basic Rate only, so Enhanced Data Rate packets
  cannot be received. The packet types handled are ID, NULL, POLL, FHS, DM and
  DH.
- Encryption is not implemented. Packets sent after a connection turns
  encryption on are reported as raw data, and the AFH map stops being
  followed, because the LMP packet carrying it is encrypted too.
- The link layer carries Asynchronous Connection-Less (ACL) traffic only, and
  does not do role switching or the LMP procedures around it.
- As the central, it polls the peripheral hard enough to drain its battery.
- `monitor` is a race against the real peripheral and often takes several
  attempts. Packets are lost: get closer, use a better antenna.

## DIAGNOSTICS

Standard output carries the prompt and the usage text `-h` prints. Everything
else goes to standard error. A command line the tool will not take, a board it
cannot select, open or set up, and anything that goes wrong during a session,
are reported as the one-line `error: `, `warning: ` and `note: ` lines the
other tools use.

The packets a command reports, and the progress it makes, are still log lines
of the form `HH:MM:SS | INFO | ...` on standard error, which is a remaining
deviation from ubertooth(7), OUTPUT AND DIAGNOSTICS: decoded packets belong on
standard output. Tracked as issue
[#735](https://github.com/SqueakyFloors/ubertooth/issues/735).

An option value outside 0-65535, or one that is not a number, is refused
before the board is opened: `error: -f must be 0-65535 (got bad)`, exit
status 1. An address the prompt will not take is one line,
`error: BD_ADDR must be xx:xx:xx:xx:xx:xx (got <word>)`, and leaves the
prompt up.

Every other command line the tool will not take is one `error: ` line on
standard error with no usage text, and exit 1: `error: unrecognized option
'-Z'`, `error: option '-f' requires a value`, `error: unexpected argument
'<word>'`, and `error: give -U once` for an option given twice.

During a session, a board that stops answering is reported and the running
command ends rather than waiting. A board that left the bus mid-command gives

    error: the receive thread stopped: USBErrorPipe: LIBUSB_ERROR_PIPE [-9]
    warning: BTCtlInquiryCmd: could not ask the device to go idle: LIBUSB_ERROR_NO_DEVICE [-4]
    warning: BTCtlInquiryCmd: the device did not go idle, stopping anyway

and a request that failed some other way gives `error: <command> stopped:
<type>: ...`. The prompt stays up in every case, and a command that will not
stop at all leaves the board claimed, which is reported on the way out (see
EXIT STATUS).

With no Ubertooth attached the tool says `error: no Ubertooth devices found`
and exits 1. A selection that names no board, or more than one, is refused
the same way; see SELECTING A DEVICE. A board that cannot be opened is
`error: could not open Ubertooth <serial>: <reason>`, with a `note: ` about
file permissions when the open was refused for want of them. A board another
program is using opens, and is then refused when the session claims its USB
interface: `error: Ubertooth <serial> is in use by another program (libusb
-6)`, the line every tool gives for that; see ubertooth(7), WHEN A BOARD
CANNOT BE USED. A board that takes none of the requests
the session opens with gets `error: could not set up Ubertooth <serial>:
...` and, when the request timed out, a `note: ` naming `bluetooth_rxtx` and
the image to flash: that is nearly always what such a board is carrying,
though a board already carrying `btbr` that stopped answering times out the
same way. See FIRMWARE.

## EXIT STATUS

0 for `-h`, and when the session ended cleanly, which is `exit`, `q`, `x` or
Ctrl-D with nothing left running. 1 for a command line the tool will not take,
when no Ubertooth is found, when `-U` names no board or more than one, when
the one it names cannot be opened, when the board refuses the requests
that set the session up, when Ctrl-C ends the tool before the prompt is up,
and when the session ends with a command that would not stop still holding the
board.

## SEE ALSO

ubertooth-rx(1): passive Classic Bluetooth discovery and following, on the
`bluetooth_rxtx` firmware

ubertooth-dfu(1): flashing the firmware images

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

`firmware/btbr/README.md` and `host/python/ubtbr/README.md` in the source
tree, and the Classic Bluetooth sniffing guide in `docs/`.

## COPYRIGHT

ubertooth-btbr(1) and the `ubtbr` module are Copyright (c) 2020 Etienne
Helluy-Lafont and are released under the GPLv2; the `btbr` firmware is
released under the GPLv3 (`firmware/btbr/LICENSE`). Refer to
[COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for
further details.
