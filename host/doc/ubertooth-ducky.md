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
 - `-A <37|38|39>` :
   advertising channel index (default: 38).
 - `-a <BD_ADDR>` :
   Bluetooth address to use, `xx:xx:xx:xx:xx:xx` (default: random).
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-V` :
   print the version: the host tools, then the firmware version and build of
   each attached board; see ubertooth(7), COMMON OPTIONS
 - `-h` :
   show help.

## TRANSMIT POWER

Injection advertises at the CC2400 power level set with `ubertooth-util -a` and
leaves it unchanged. A board that has not been given a level transmits at 7
(0 dBm), the CC2400's reset value.
These levels are at the CC2400 radio; the Ubertooth One's RF amplifier changes
the actual output after that.

## ARGUMENTS

`ubertooth-ducky` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: advertising channel index must be 37-39 (got 40)`.

The optional value of `-q` may be attached or given as the next word
(`-q <uuid>`); a following option is never taken as a value.

`-q` or `-b` is the work, so a command line with neither is refused before
any device is opened: `error: nothing to do: give -q to quack, or -b to
signal the bootloader`. It used to open the selected board, check its
firmware, quack nothing and exit 0.

## DIAGNOSTICS

What `ubertooth-ducky` does happens over the air, so it writes no result:
everything it prints, including the duck and the "Quacking ... as ..." line,
goes to standard error, and standard output stays empty. Each error is one
line starting `error: `, for example
`error: cannot read the UUID 'nonsense'`.

A request the board refuses ends the command there, named in one line, for
example `error: could not set the advertising channel`; libubertooth has
already said why. A refused request is not proof that the board never got it,
so the message does not claim one way or the other.

A command line the tool will not take is reported as that one `error: ` line
on standard error, with no usage text after it: the line describes the
problem, and `-h` prints the usage text for a reader who wants it. An option
the tool does not know reads `error: unrecognized option '-Z'`; one whose
value is missing reads `error: option '-U' requires a value`.

`-h` is what the user asked for, so the usage text goes to standard output
and nothing goes to standard error. A bare `ubertooth-ducky`, with no arguments
at all, is the one refusal the usage text does follow, on standard error with
the `error: ` line.

## EXIT STATUS

0 when the command did what was asked, including `-h`: for `-q` and `-b`, when
the board took every request the advertisement needs. 1 for a command line the
tool will not take or with nothing to do, for a UUID it cannot read, when the
selected Ubertooth is not found or cannot be opened, and when the board
refuses one of those requests.

1 as well when the board refuses the request that stops the advertisement at
the end of the two seconds: the board is left advertising, and no other tool
can open it until it is reset or re-plugged. libubertooth names the refused
request on standard error.

`-V` reads the attached board, so it exits 1 when a board is attached and
cannot be read, or when the USB bus cannot be listed at all; see ubertooth(7),
COMMON OPTIONS.

## SEE ALSO

ubertooth-btle(1)

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
