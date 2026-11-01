# ubertooth-btbr(1) -- experimental Classic Bluetooth connections and monitoring

## SYNOPSIS

    ubertooth-btbr [-U <index|serial>] [-f <freq_offset>] [-b <max_ac_errors>]
    ubertooth-btbr [-U <index|serial>] -V
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

Every command that takes a `BD_ADDR` takes the six bytes of an address as
twelve hex digits, in either case, written `11:22:33:44:55:66` or
`112233445566`. Anything else is refused; see DIAGNOSTICS. The `bdaddr=` an
inquiry prints is written the second way, so it can be copied into `page`,
`l2ping`, or `monitor` as it stands.

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
 - `-V` :
   print the version and end the command: the host tools, then the firmware
   version and build of the board `-U` names, or of each attached board. The
   same answer every other tool gives, including with no board attached; see
   ubertooth(7), COMMON OPTIONS. This tool answers it by running
   ubertooth-util(1) `-V`, so that tool has to be on the `PATH`, and is
   reported as `error: could not run ubertooth-util: No such file or
   directory` when it is not. No `btbr` firmware is needed for it: the
   version is read from whatever image the board is carrying. It needs no
   Python `libusb1` package either: the command line is read before the
   module that package ships is imported, so `-V` answers on a host where it
   is not installed. See DIAGNOSTICS.
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value, a stray word or
   an option given twice is still reported. Like `-V`, it needs no Python
   `libusb1` package.

`-V` and `-h` both end the command, and both answer on standard output, so a
command line with both prints both: the version first and the usage text
last, whichever order they were written in. The status is then `-V`'s, so a
board that could not be read makes a command line carrying `-h` exit 1, with
the usage text printed all the same.

Each option may be given once, except `-V` and `-h`, which carry no value and
say the same thing however often they are written. See DIAGNOSTICS for what a
command line it will not take does.

## SELECTING A DEVICE

`-U <index|serial>` chooses one of several attached boards, as described in
ubertooth(7), SELECTING A DEVICE: an index is a position in the list of
attached Ubertooths (`-U 0` is the first), and a serial-number string selects
that board whatever the enumeration order. A serial may be shortened to its
first few characters, in either case, and an all-digits string is read as an
index, so include a non-digit (`-U 19b` rather than `-U 19`). If those
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
      Device 0: aa1100112233445566778899aabbccdd
      Device 1: ee5500112233445566778899aabbccdd

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
   accept a connection from a device that pages this board; a standard
   Classic controller usually gets through in a few seconds, and another
   Ubertooth on the same bench needs its transmit level turned down first,
   as does this one (see TWO BOARDS CLOSE TOGETHER); a board scanning this
   way answers a small L2CAP echo request, so `l2ping` from another Ubertooth
   sees a reply (see LIMITATIONS for how small)
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
 - `tx <format> [<length>] [<count>] [<hex>]` :
   transmit one packet of a chosen basic-rate format on the connection that
   is up; see TRANSMITTING CHOSEN PACKET TYPES
 - `bdaddr <BD_ADDR>` :
   set this board's address
 - `idle` :
   stop the running command and return the board to standby
 - `exit`, `q`, `x`, Ctrl-D :
   leave the tool
 - `?`, `? <command>` :
   list the commands, or describe one

`page`, `l2ping`, and `page_scan` keep trying for as long as the board allows
one attempt: about 45 seconds of paging, a minute of scanning. A connection
that comes up and then carries nothing costs that attempt only the second link
supervision takes to notice it, after which the board goes back to paging or
scanning and says `Paging resumed` or `Page scan resumed`. That happens when
some other board nearby answers a page and is taken for the device being
paged, or when the paged device's acknowledgment does not reach the pager.

A word that is none of these is refused, and the prompt stays up; see
DIAGNOSTICS.

One command runs at a time. Starting another while one is still running is
refused with a `Not idle` line; `idle` or Ctrl-C stops the running one first.

Ctrl-C at the prompt stops the running command, as `idle` does, and leaves the
prompt up; pressing it again while that command is stopping returns to the
prompt. Before the prompt is up, Ctrl-C ends the tool.

## TWO BOARDS CLOSE TOGETHER

Paging one Ubertooth from another needs both of them turned down first. At the
transmit level the radio starts at, two boards a few centimeters apart drive
each other's receiver far past the level it can demodulate. The page itself
still gets through, because a page scan sits on one frequency for milliseconds
at a time, but the short receive windows that follow it find nothing: the
paged board comes up as a peripheral, the pager never hears the
acknowledgment, and the connection ends a second later having carried no
packet.

Set both boards to the lowest level before paging one from the other:

    ubertooth-util -U <serial> -a 0

`btbr` keeps whatever level the register holds, so this lasts until the board
is flashed again. The lowest level is about 20 dB down: plenty across a bench
and not enough for a device across a room, so put it back to 7 before paging
anything else. Moving the two boards apart, or putting an attenuator on one
antenna, does the same job.

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

## TRANSMITTING CHOSEN PACKET TYPES

A real stack sends DM1, DH1, and their longer relatives and almost nothing
else, so the packet types below are hard to find on the air at all. `tx` puts
one of them there: with a connection up, it hands the board a packet built to
that format, and the board transmits it in its next transmit slot.

    ubertooth-btbr
    bt> page C8:85:41:2A:1B:2A
    bt> tx aux1 29
    bt> tx ev3 17 20
    bt> tx hv3
    bt> tx dh5 339 5 00ff00ff...

The formats, with the data bytes each carries. A data length excludes the
payload header the format may have, which `tx` writes itself, and the 2-byte
CRC it may have, which the board adds:

 - `dm1` 0-17, `dh1` 0-27, `dm3` 0-121, `dh3` 0-183, `dm5` 0-224, `dh5` 0-339 :
   the asynchronous data packets a connection carries anyway, here at a length
   of your choosing
 - `aux1` 0-29 :
   like a `dh1` but with no CRC, so nothing detects a bit error in it
 - `dv` 0-9 :
   10 bytes of voice, then a data field built like a `dm1`'s
 - `hv1` 10, `hv2` 20, `hv3` 30 :
   the synchronous voice packets, each one fixed length with no CRC, under
   rate 1/3, rate 2/3, and no error correction in turn
 - `ev3` 1-30, `ev4` 1-120, `ev5` 1-180 :
   the enhanced synchronous packets, which carry a CRC and a length that is
   agreed when the link is set up rather than written in the packet

`<length>` defaults to the format's maximum, `<count>` to 1. `<count>` is how
many of the board's transmit slots to spend on the packet, which is the way to
make one easy for a receiver to catch; the packet is not acknowledged and not
retransmitted, so a peer that ignores it costs nothing.

`<hex>` is the data bytes, as an even number of hex digits of exactly
`<length>` bytes. Without it the data is a counted pattern, `00 01 02 ... ff`
and over again. Either way the line `tx` prints on standard output is what
went on the air:

    TX pkt: ev3 len=17 slots=20 data=000102030405060708090a0b0c0d0e0f10

`hv3` and `ev3` go out with the same TYPE code in the packet header, which is
what the two mean on a synchronous and on an enhanced synchronous link
(Bluetooth Core, Vol 2 Part B, Table 6.2). They differ in the payload: an
`hv3` is 30 bytes with nothing to check them, an `ev3` any agreed length with
a CRC. A receiver has to tell them apart from that, which is why both are here.

A length the format will not carry is refused before anything is sent, and so
is a `tx` with no connection up: there is no transmit slot without one. The
peer is free to ignore or reject any of these packets, and a real controller
will reject most of them, because nothing negotiated a synchronous link; the
packet is on the air either way, which is the point.

## LIMITATIONS

- The CC2400 radio demodulates Basic Rate only, so Enhanced Data Rate packets
  cannot be received. The packet types received are ID, NULL, POLL, FHS, DM and
  DH; `tx` transmits more than that, but a packet of one of those extra
  formats arriving here is not decoded, because a TYPE code alone does not say
  which format a synchronous packet is and this firmware has no synchronous
  link to tell it.
- Encryption is not implemented. Packets sent after a connection turns
  encryption on are reported as raw data, and the AFH map stops being
  followed, because the LMP packet carrying it is encrypted too.
- An L2CAP echo request is answered when the whole request arrived in one
  packet and the reply fits one too, which leaves room for 9 bytes of echo
  data. `l2ping` here sends 4 and gets a reply. A larger request is not
  reassembled from the fragments it arrives in and is not answered, so a
  standard host's `l2ping`, which sends 44 bytes of data by default, gets no
  reply unless it is asked for a smaller payload. The packets it sends are
  still reported, as every received packet is.
- The link layer carries Asynchronous Connection-Less (ACL) traffic only, and
  does not do role switching or the LMP procedures around it. A peer asking to
  send multi-slot packets is refused as well, and the feature mask this end
  sends does not offer them, so a connection stays on single-slot ones: the
  firmware decodes the multi-slot kinds, but no capture has exercised that
  over the air. A request for any other procedure there is no handler for is
  refused too, rather than left unanswered, so the peer can get on with the
  link instead of waiting out a 30-second LMP response timeout; a PDU that
  expects no answer is ignored. Either way the opcode is named in a `warning:`
  line, once per opcode for the life of the connection.
- A peer may refuse any of the five things a connection opens by asking it
  for: its features, extended features, version, name, and encryption key
  size mask. Each refusal is reported with the reason the peer gave, and the
  next request goes out as it would after an answer. A peer that refuses the
  connection request those five lead to is declining the connection itself.
  That is reported in a `warning:` line, and the link is then closed at once:
  the command hangs up and ends, rather than holding a connection the peer
  has already turned down.
- A PDU too short to carry an opcode is reported in a `warning:` line and the
  connection goes on. There is nothing to answer in one: an escape opcode
  says the opcode is in the PDU's second byte, and a PDU of one byte has
  none.
- A PDU that does carry its opcode but ends before its parameters is answered
  instead: the name request, the extended features request, and the AFH set
  are refused with invalid LMP parameters, and a short slot offset is ignored,
  since that PDU expects no answer. `monitor` has no peer to answer, so it
  reports such a PDU and leaves the AFH map as it was.
- A message from the board the tool cannot read is dropped and the session
  goes on with the next one: a packet that ends before what it says it holds,
  one of a type the tool does not decode, or a message whose own header is
  wrong. It is reported in a `warning:` line naming what arrived, said once
  per reason for the life of the session, since a board sending one such
  message usually sends many. A message whose framing the tool cannot follow
  at all is a different thing and still ends the thread reading the board;
  see EXIT STATUS.
- As the central, it polls the peripheral hard enough to drain its battery.
- `monitor` is a race against the real peripheral and often takes several
  attempts. Packets are lost: get closer, use a better antenna.

## OUTPUT

The packets a command decodes go to standard output, one to a line, each
carrying the time it was printed:

    19:18:58 RX FHS: FHS: bdaddr=112233445566 parity=6c38a7bd cls=5a020c ltaddr=0 clk=365c

That time is the only timing a session has. An inquiry response is the
response itself and the EIR fields under it, each as a timed line of its own:

    19:19:03 ACL(llid=L2CAP_START, flow=1, len=11): 0a 09 55 62 65 72 74 6f 6f 74 68
    19:19:03 COMPLETE_LOCAL_NAME: 'Ubertooth'

A session holding a connection -- `page`, `l2ping`, `page_scan`,
`discoverable` -- reports each LMP packet the same way, with the baseband
header and the PDU named and in hex. It also traces the same exchange on
standard error, where the link manager adds the state it was in and the
seconds since the connection came up; that trace is a diagnostic, and the
capture is what standard output holds.

A connection the peer hangs up ends with the reason the peer gave, on standard
error with the rest of the link manager's trace:

    Peer detached: reason 0x13, the peer's user ended the connection

The prompt, the `Type ? to list commands` line and what `?` and `? <command>`
print are on standard error, with the progress a command makes, as plain lines
carrying neither the time nor a level. The prompt has no newline of its own,
so the first progress line after it lands on its end, as it does on a
terminal:

    Type ? to list commands
    bt> Starting BTCtlInquiryCmd
    Inquiry state Ready
    Inquiry done

Every line of a capture can therefore be read by the time in front of it,
whatever the user types while it runs: the prompt carries no newline of its
own, and on standard output it would land on the front of the next packet,
while `?` is the tool describing itself rather than capture data. The usage
text `-h` prints is on standard output, which is the whole of what that run
was asked for.

A session at a terminal leaves the prompt to the terminal's line editor, which
draws it on the terminal as it always has and keeps the cursor right while a
line is edited. Nothing is reading standard output there, so nothing is in the
way.

## DIAGNOSTICS

Standard output carries the packets a command decodes and the usage text `-h`
prints; see OUTPUT. Everything else, the intro line, what `?` prints and the
prompt of a session nobody is editing at a terminal included, goes to standard
error. A command line the tool will not take, a board it cannot select, open
or set up, and anything that goes wrong during a session, are reported as the
one-line `error: `, `warning: ` and `note: ` lines the other tools use.

A line the session's character encoding cannot carry is dropped rather than
ending the command. A device name arrives as whatever bytes the air sent, so a
session that is not running under UTF-8 meets one it cannot write, and the
replacement character a name cut mid-character decodes to does the same. The
line goes, the rest of the message around it is printed, and the loss is said
once per stream:

    warning: a line could not be written in standard output's character encoding (ascii), so it was dropped
    note: run the session under an encoding that carries the text, for example PYTHONIOENCODING=utf-8

A decoded line lost that way is a packet that was asked for and not printed,
so the session exits 1 (see EXIT STATUS). A diagnostic lost that way is not,
and leaves the status alone.

A message from the board the session could not read is one of those, naming
what arrived:

    warning: a message from the device was dropped: FHS packet of 17 bytes, too short for the 18 an FHS carries

The capture goes on without it; see LIMITATIONS. An inquiry response whose
EIR has a field running past the end of it is `warning: malformed EIR: ...`,
once per command rather than once per session: what is wrong there belongs to
whichever device answered, and the next `inquiry` may hear another.

A host without the Python `libusb1` package can still ask what is installed:
`-V`, `-h` and a command line the tool will not take are all answered before
the `usb1` module that package ships is imported. A call that opens a board
does need it, and says so in one line rather than a traceback:

    error: could not start the session: No module named 'usb1'
    note: a session needs the Python libusb1 package, which ships the usb1 module; install it with "pip install libusb1"

An option value outside 0-65535, or one that is not a number, is refused
before the board is opened: `error: -f must be 0-65535 (got bad)`, exit
status 1. An address the prompt will not take is one line,
`error: BD_ADDR must be xx:xx:xx:xx:xx:xx (got <word>)`, and leaves the
prompt up. Only the two spellings above are taken, so `bdaddr 11:22:33` is an
error rather than the address `00:00:11:22:33`, and so are a twelve-digit word
with the colons anywhere else (`11:2233:4455:66`), another separator
(`11-22-33-44-55-66`), and the forms Python would read as a number
(`0x112233445566`, `11_2233445566`). A `? <command>` naming no command is
`error: no help on '<word>'`, and so does. A word that is no command at all is
`error: no such command '<word>'`, naming the first word of the line, and
leaves the prompt up as well. None of the three changes the session's exit
status.

Every other command line the tool will not take is one `error: ` line on
standard error with no usage text, and exit 1: `error: unrecognized option
'-Z'`, `error: option '-f' requires a value`, `error: unexpected argument
'<word>'`, and `error: option '-U' given more than once` for an option given
twice, the line every tool uses (see ubertooth(7), OPTION VALUES).

During a session, a board that stops answering is reported and the running
command ends rather than waiting. A board that left the bus mid-command gives

    error: the receive thread stopped: USBErrorPipe: LIBUSB_ERROR_PIPE [-9]
    warning: BTCtlInquiryCmd: could not ask the device to go idle: LIBUSB_ERROR_NO_DEVICE [-4]
    warning: BTCtlInquiryCmd: the device did not go idle, stopping anyway

and a request that failed some other way gives `error: <command> stopped:
<type>: ...`. The prompt stays up in every case, and a command that will not
stop at all leaves the board claimed, which is reported on the way out (see
EXIT STATUS).

Standard output going away is one line, `error: standard output has gone
away`, said once however many writes find it gone. A capture piped into
something that stops reading, as `ubertooth-btbr | head -5` does, therefore
leaves the prompt up with nothing more printed, and the session ends with
exit 1; so does a `-h` or `-V` whose answer could not be delivered, whether
its reader left or there was no standard output to print to at all, as
`ubertooth-btbr -h >&-` leaves. A session asked for with no standard output,
or with one that was not opened to be written, as `ubertooth-btbr 1<&0`
leaves, is refused in that same line, before any board is opened: the stream
carries the capture, and the prompt cannot be read without it either. A
session that had printed nothing when the reader left has lost nothing and
ends as it otherwise would. If standard
error has gone the same way, the exit status is all that is left to say it
with.

Standard error going away costs the tool its diagnostics and nothing else.
One open for reading only (`ubertooth-btbr --bogus 2<&0`), closed
(`ubertooth-btbr --bogus 2>&-`), or a pipe whose reader has left
(`ubertooth-btbr --bogus 2>&1 | true`) all go the same way: the lines are
dropped and the exit status is the command's own. A session keeps its capture
through it, wherever standard output is going: a warning in the middle of one
used to end it and take the rest of the capture with it, and a refused
command line used to exit 120 rather than 1, from the flush the interpreter
makes as it exits. A session with that stream closed used to be refused
before the prompt was read. A child the tool starts for `-V` is handed the
bin for that stream rather than a closed descriptor, whose number the first
file it opened would take.

With no Ubertooth attached the tool says `error: no Ubertooth devices found`
and exits 1. A selection that names no board, or more than one, is refused
the same way; see SELECTING A DEVICE. A board that cannot be opened is
`error: could not open Ubertooth <serial>: <reason>`, with a `note: ` about
file permissions when the open was refused for want of them. A board another
program is using opens, and is then refused when the session claims its USB
interface: `error: Ubertooth <serial> is in use by another program (libusb
-6)`, the line every tool gives for that. Which libusb code it carries depends
on the system, `-6` on Linux and `-3` on macOS; see ubertooth(7), WHEN A BOARD
CANNOT BE USED. A board that takes none of the requests
the session opens with gets `error: could not set up Ubertooth <serial>:
...` and, when the request timed out, a `note: ` naming `bluetooth_rxtx` and
the image to flash: that is nearly always what such a board is carrying,
though a board already carrying `btbr` that stopped answering times out the
same way. See FIRMWARE.

## EXIT STATUS

0 for `-h`, and when the session ended cleanly, which is `exit`, `q`, `x` or
Ctrl-D with nothing left running and everything asked for printed. 1 for a
command line the tool will not take, when standard output went away before
all that was asked for had been printed, when a decoded line was dropped
because the session's character encoding could not carry it (see
DIAGNOSTICS),
when a session was asked for and the Python `libusb1` package is not
installed, when no Ubertooth is found, when `-U` names no board or more than one, when
the one it names cannot be opened or is in use by another program, when the
board refuses the requests
that set the session up, when Ctrl-C ends the tool before the prompt is up,
when the session ends with a command that would not stop still holding the
board, and when the thread reading the board stopped on a failure.

The last of those is a board that left the USB bus, stopped answering, or sent
a message whose framing this tool could not follow, with the session still up:
the thread reading it ends there, nothing restarts it, and no message arrives
for the rest of the session. The board was power-cycled under a running
`inquiry` when this was found, and the session reported every step correctly
and then exited 0, so a script could not tell it from a run where every
command worked. What
ended that thread is said as it happens, `error: the receive thread stopped:
<type>: ...`, and again on the way out as a `warning: `.

A single command the board refused is not that. It is reported on standard
error (see DIAGNOSTICS), the prompt stays up, and the session goes on and
ends with the status its own ending gives it, the way a shell reports a failed
command without failing itself. One message the tool could not read is not
that either: it is dropped and the session goes on, which costs the capture
that message and nothing else (see LIMITATIONS).

## SEE ALSO

ubertooth-rx(1): passive Classic Bluetooth discovery, and following where the
clock search reaches a clock; THE CLOCK SEARCH AND ADAPTED HOPPING says when
it does not

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
