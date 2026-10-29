# btle-extcap.py

An external capture program, which lets Wireshark capture BLE advertising
packets from an Ubertooth directly, with no capture file in between. Wireshark
calls it; it runs `ubertooth-btle` and lets it write PCAP data into the fifo
Wireshark opened.

The script's manual is `btle-extcap(1)`, from `host/doc/btle-extcap.md`:
installing it, every call Wireshark makes, the exit statuses and the
diagnostics. What follows here is the same ground for a reader in the source
tree.

Written by Mike Kershaw (dragorn) and Mike Ryan, and distributed under the
terms of the GNU GPL, as is the rest of the Ubertooth project.

## Installing it

Copy or link the script into Wireshark's personal extcap directory, which
Wireshark itself names under Help, About Wireshark, Folders ("Personal Extcap
path"); the path differs between Wireshark versions and platforms, and
Wireshark 4.4 and later look in a `wireshark` subdirectory of it:

    ln -s "$PWD/btle-extcap.py" <that path>/

The script must be executable, and `ubertooth-util` and `ubertooth-btle` must
be on the `PATH` Wireshark starts it with. Each attached board then appears in
Wireshark's capture list as "Ubertooth One `<serial>`".

## Options

Wireshark supplies all of these itself; they are listed because the script is
worth running by hand when a board does not appear in Wireshark.

 - `--extcap-interfaces` :
   one `interface` line per attached board, named `ubertooth<serial>`. A
   board whose serial cannot be read is left out, since it has no name to
   offer, and the boards that could be named are still listed. No board
   attached is an empty listing and exit 0; a listing `ubertooth-util` failed
   on exits 1 and reports what it said, whether or not it named a board
 - `--extcap-interface=ubertooth<serial>` :
   the board every other call applies to. The serial is passed to
   `ubertooth-btle -U`, so a board keeps its interface across a reboot or a
   replug, which an enumeration index does not. An `ubertooth<index>` saved
   by an older version still works, because `ubertooth-btle -U` reads an
   all-digit value as an index
 - `--extcap-dlts` :
   the one link type the capture produces,
   `DLT_BLUETOOTH_LE_LL_WITH_PHDR`
 - `--extcap-config` :
   the configurable arguments, which is the advertising channel
 - `--capture --fifo=PATH [--channel=37|38|39]` :
   run `ubertooth-btle` on the selected board and write the capture into
   `PATH` (default channel 37). Wireshark names the fifo it opened; `--fifo=-`
   is standard output, for a capture run by hand
 - `-h`, `--help` :
   the usage line, on standard output

## Wireshark's own options

Wireshark adds options of its own to the calls above. The script accepts and
ignores the two it adds whatever an interface advertises:

 - `--extcap-version[=<version>]` :
   the calling Wireshark's version, appended to the `--extcap-interfaces`
   call by Wireshark 2.9 and later. Nothing here behaves differently for
   different versions. Rejecting it used to leave Wireshark with an empty
   interface list, since the whole listing was refused. The version can be
   written `--extcap-version=5.0`, `--extcap-version 5.0` or left off
   altogether
 - `--extcap-capture-filter=<filter>` :
   a capture filter the user typed. `ubertooth-btle` captures every
   advertising packet it sees and cannot apply one. With an interface and
   nothing else, this is Wireshark's verification call, and the answer is
   `this interface does not apply capture filters; use a display filter`,
   printed on standard output: Wireshark shows what this call writes there in
   the capture filter box, in red, and reads an empty answer as a filter that
   is good. An empty filter, which is what Wireshark verifies when the box is
   empty, is good. Alongside `--capture` the capture starts anyway and a
   `warning: ` line repeats the same sentence

Everything else Wireshark can pass, it passes only to a program whose listing
advertised the feature behind it: `--extcap-reload-option` to one with a
reloadable argument, `--extcap-control-in` and `--extcap-control-out` to one
with toolbar controls, `--extcap-config-option-name` and
`--extcap-config-option-value` to one with a configurable table value. This
script advertises none of them, so those options can only arrive by mistake
and are refused like any other unknown option.

## Exit status

The call either did what was asked, which is `0`, or it did not, which is
`1`. Nothing else is reported: Wireshark needs no more than non-zero for a
failed capture, so the script does not pass `ubertooth-btle`'s own status
through or report the signal that killed it.

 - `0` :
   the call did what was asked. For `--capture` that includes a capture that
   was asked to stop, either way it ends: SIGTERM or SIGINT arrives and the
   script asks `ubertooth-btle` to stop with a SIGTERM, or `ubertooth-btle`
   dies of SIGPIPE because nothing is reading the fifo any more. It also
   includes Wireshark's capture-filter verification call, whose answer is its
   standard output and not its status: a non-zero exit there makes Wireshark
   discard the answer and report "Unable to check capture filter"
 - `1` :
   a usage error, a board that could not be opened, a tool that could not be
   run, a `ubertooth-btle` that failed or was killed, or any other failure.
   That includes a partial listing: if `--extcap-interfaces` could not open
   one board it exits 1 even though it
   named the others, with the reason on standard error. Wireshark throws away
   everything a call that failed printed, so its capture list is then without
   the boards that were fine until that board is freed or unplugged. The one
   case that is not a failure is no board attached: the empty listing answers
   that, and exits 0. Wireshark reads a non-zero exit of the `--capture` call
   as a failed capture and shows the error for it

## Diagnostics

Errors and warnings go to standard error, as does the usage line after a bare
call (`error: nothing to do: give --extcap-interfaces, --extcap-dlts,
--extcap-config or --capture`, then the usage line). The script's own standard
output carries only the listings above and the capture-filter answer, the
capture data going to the fifo instead, and nothing when a call fails. The
`ubertooth-btle` a `--capture` call runs writes its own decode of every
packet to its standard output, which is dropped: nobody asked this call for
it, and 8 seconds of ambient advertising is some 560 kB. Its standard error
is the script's, so a `ubertooth-btle` that fails still says why. Wireshark
keeps the first 1024 bytes of the capture call's standard error and shows
them for the interface when the call fails.

`--fifo=-` is the one capture whose data is on standard output: it is the `-`
every file option here takes, for a capture run by hand rather than one
Wireshark asked for, and `ubertooth-btle` writes the PCAP data there and no
decode.

A command line the script will not take is refused before `ubertooth-btle`
runs, so nothing is written to the fifo:
`error: unrecognized option '--list-dlts'`, in the wording every other tool
here uses for the same mistake, with
`error: option '--fifo' requires a value` for a missing value and, for the two
mistakes only a long option can make,
`error: option '--capture' takes no value` and
`error: option '--extcap-c' is short for more than one option`,
`error: unexpected argument 'extra'` for a word that is not an option or the
value of one, since no call here takes an operand,
`error: --extcap-interface must be ubertooth<serial>, as --extcap-interfaces
lists it`, `error: --capture needs --fifo=PATH` and
`error: advertising channel index must be 37-39 (got 40)`, which is
`ubertooth-btle`'s own wording for the same mistake. A call with no operation
in it, which used to exit 0 having done nothing, gives the same
`error: nothing to do: ...` line as a bare call. Each is that one line:
the usage line follows only a bare `btle-extcap.py`, and `-h` prints it on
standard output. `-h` reports a stray word too, rather than ending the command
before the rest has parsed.

Whatever `ubertooth-util` and `ubertooth-btle` write to standard error is
passed on, including
`error: no Ubertooth found with serial matching '<serial>'` for a board that
has been unplugged since the listing. Wireshark shows what the `--capture` call wrote
there for the interface when the call fails; for the other calls it reads only
standard output, so those lines land on Wireshark's own error output, which is
also what someone running the call by hand sees.

The one line held back is `ubertooth-util`'s `error: no Ubertooth devices
found`, and only when it is the sole error it reported about an empty listing,
since the empty listing says the same thing. Alongside any other `error: `
line it is passed on with the rest.

A tool that is not on the `PATH` Wireshark started the script with is
reported as `error: could not run ubertooth-util: No such file or directory`,
and one that is there but cannot be executed names that reason instead
(`Permission denied`). A `ubertooth-util` that failed without saying why is
reported as `error: ubertooth-util failed (exit status 3)`, since a failure
the user is told about has to name itself.

## See also

`ubertooth-btle` and `ubertooth-util` (`host/doc/ubertooth-btle.md`,
`host/doc/ubertooth-util.md`), and `extcap(4)` for the interface Wireshark
expects.
