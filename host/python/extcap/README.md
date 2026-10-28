# btle-extcap.py

An external capture program, which lets Wireshark capture BLE advertising
packets from an Ubertooth directly, with no capture file in between. Wireshark
calls it; it runs `ubertooth-btle` and lets it write PCAP data into the fifo
Wireshark opened.

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
   `PATH` (default channel 37)
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
   advertising packet it sees and cannot apply one, so a capture starts
   anyway and a `warning: ` line says the filter was not applied. Wireshark
   shows what the capture call wrote to standard error only when that call
   fails, so a filter can look accepted; filter the packets in Wireshark
   instead

Everything else Wireshark can pass, it passes only to a program whose listing
advertised the feature behind it: `--extcap-reload-option` to one with a
reloadable argument, `--extcap-control-in` and `--extcap-control-out` to one
with toolbar controls, `--extcap-config-option-name` and
`--extcap-config-option-value` to one with a configurable table value. This
script advertises none of them, so those options can only arrive by mistake
and are refused like any other unknown option.

## Exit status

 - `0` :
   the call did what was asked. For `--capture` that includes a capture
   Wireshark ended, either way it ends one: the script passes the signal on
   to `ubertooth-btle`, which dies of it, or `ubertooth-btle` dies of SIGPIPE
   because nothing is reading the fifo any more
 - `1` :
   a usage error, a board that could not be opened, a tool that could not be
   run, or any other failure. That includes a partial listing: if
   `--extcap-interfaces` could not open one board it exits 1 even though it
   named the others, with the reason on standard error. Wireshark throws away
   everything a call that failed printed, so its capture list is then without
   the boards that were fine until that board is freed or unplugged. The one
   case that is not a failure is no board attached: the empty listing answers
   that, and exits 0
 - anything else :
   `ubertooth-btle`'s own exit status, or `128` plus the number of the signal
   that killed it. Wireshark reads a non-zero exit of the `--capture` call as
   a failed capture and shows the error for it

## Diagnostics

Errors and warnings go to standard error, as does the usage line after a bare
call (`error: nothing to do: give --extcap-interfaces, --extcap-dlts,
--extcap-config or --capture`, then the usage line). The script's own standard
output carries only the listings above, the capture data going to the fifo
instead, and nothing when a call fails. During
`--capture` the `ubertooth-btle` it runs inherits that standard output and
writes its own decode of every packet there; Wireshark discards it.

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
