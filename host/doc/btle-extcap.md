# btle-extcap.py(1) -- capture BLE advertising from an Ubertooth in Wireshark

## SYNOPSIS

    btle-extcap.py --extcap-interfaces
    btle-extcap.py --extcap-interface=ubertooth<serial> --extcap-dlts
    btle-extcap.py --extcap-interface=ubertooth<serial> --extcap-config
    btle-extcap.py --extcap-interface=ubertooth<serial> --capture --fifo=PATH
    [--channel=37|38|39]
    btle-extcap.py -V
    btle-extcap.py -h

## DESCRIPTION

btle-extcap.py(1) is an external capture program, which lets Wireshark
capture BLE advertising packets from an Ubertooth directly, with no capture
file in between. Wireshark calls it; it runs ubertooth-btle(1) and lets that
tool write PCAP data into the fifo Wireshark opened.

It is not a tool to run by hand in normal use. Install it where Wireshark
looks for extcap programs (see INSTALLATION), and each attached board appears
in Wireshark's capture list as "Ubertooth One `<serial>`". Running the calls
below by hand is how to find out why a board does not appear there.

The script needs ubertooth-util(1) and ubertooth-btle(1) on the `PATH`
Wireshark starts it with: it lists the attached boards with the first and
captures with the second. The board needs the `bluetooth_rxtx` firmware, as
it does for ubertooth-btle(1) itself.

## INSTALLATION

The script is not installed by `make install`: Wireshark loads extcap
programs from its own directory, which differs between Wireshark versions and
platforms. Copy or link the script into the personal extcap directory
Wireshark names under Help, About Wireshark, Folders ("Personal Extcap
path"); Wireshark 4.4 and later look in a `wireshark` subdirectory of it:

    ln -s "$PWD/host/python/extcap/btle-extcap.py" <that path>/

The script must be executable. Restart Wireshark, or refresh the interface
list, and the boards appear.

## OPTIONS

Wireshark supplies all of these itself. They are listed because the script is
worth running by hand when a board does not appear in Wireshark.

 - `--extcap-interfaces` :
   one `interface` line per attached board, named `ubertooth<serial>`. A board
   whose serial cannot be read is left out, since it has no name to offer, and
   the boards that could be named are still listed. No board attached is an
   empty listing and exit 0; a listing ubertooth-util(1) failed on exits 1 and
   reports what it said, whether or not it named a board
 - `--extcap-interface=ubertooth<serial>` :
   the board every other call applies to. The serial is passed to
   `ubertooth-btle -U`, so a board keeps its interface across a reboot or a
   replug, which an enumeration index does not. An `ubertooth<index>` saved by
   an older version still works, because `ubertooth-btle -U` reads an
   all-digit value as an index
 - `--extcap-dlts` :
   the one link type the capture produces,
   `DLT_BLUETOOTH_LE_LL_WITH_PHDR`
 - `--extcap-config` :
   the configurable arguments, which is the advertising channel
 - `--capture --fifo=PATH [--channel=37|38|39]` :
   run ubertooth-btle(1) on the selected board and write the capture into
   `PATH` (default channel 37). Wireshark names the fifo it opened;
   `--fifo=-` is standard output, for a capture run by hand
 - `--extcap-version[=<version>]` :
   the calling Wireshark's version, appended to the `--extcap-interfaces`
   call by Wireshark 2.9 and later. Accepted and ignored: nothing here
   behaves differently for different versions. The value may be written
   `--extcap-version=5.0`, `--extcap-version 5.0` or left off altogether
 - `--extcap-capture-filter=<filter>` :
   a capture filter the user typed. ubertooth-btle(1) captures every
   advertising packet it sees and cannot apply one, so a non-empty filter is
   reported invalid; see CAPTURE FILTERS
 - `-V` :
   print the version and end the call: the host tools, then the firmware
   version and build of each attached board. The same answer every other tool
   gives, including with no board attached; see ubertooth(7), COMMON OPTIONS.
   The script answers it by running ubertooth-util(1) `-V`, which it needs on
   the `PATH` as it does for the listing, and reports one that is not there as
   `error: could not run ubertooth-util: No such file or directory`. The
   script has no `-U`, so every attached board is reported, and the call ends
   before `--extcap-interface` is read. Wireshark never makes this call
 - `-h`, `--help` :
   print the usage line on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a stray word or an unknown option is
   still reported

`-V` and `-h` both end the call, and both answer on standard output, so a
call with both prints both: the version first and the usage line last,
whichever order they were written in. The status is then `-V`'s, so a board
that could not be read makes a call carrying `-h` exit 1, with the usage line
printed all the same. Either option ends the call ahead of every other one,
so `--extcap-interfaces -h` prints the usage line and lists nothing.

Everything else Wireshark can pass, it passes only to a program whose listing
advertised the feature behind it: `--extcap-reload-option` to one with a
reloadable argument, `--extcap-control-in` and `--extcap-control-out` to one
with toolbar controls, `--extcap-config-option-name` and
`--extcap-config-option-value` to one with a configurable table value. This
script advertises none of them, so those options can only arrive by mistake
and are refused like any other unknown option.

## CAPTURE FILTERS

Wireshark verifies a capture filter by calling the program with the interface
and `--extcap-capture-filter=<filter>` and no `--capture`, and shows on the
filter box whatever that call writes to standard output.

This interface applies no capture filter, so a non-empty filter is answered
with `this interface does not apply capture filters; use a display filter` on
standard output, and Wireshark shows that sentence in the filter box in red.
An empty filter, which is what Wireshark verifies when the box is empty, is
good: the answer is an empty standard output. Either way the call exits 0,
because a non-zero exit would make Wireshark discard the answer and report
"Unable to check capture filter" instead of showing it.

A filter given alongside `--capture` does not stop the capture: it starts
anyway, with a `warning: ` line repeating the same sentence.

## EXAMPLES

Check that the boards Wireshark would list are the ones attached:

    btle-extcap.py --extcap-interfaces

Capture on one board by hand, writing PCAP to standard output:

    btle-extcap.py --extcap-interface=ubertooth<serial> --capture --fifo=- \
        --channel=38 > advertising.pcap

## DIAGNOSTICS

Errors and warnings go to standard error, as does the usage line after a bare
call (`error: nothing to do: give --extcap-interfaces, --extcap-dlts,
--extcap-config or --capture`, then the usage line). The script's own standard
output carries only the listings and the capture-filter answer, the capture
data going to the fifo instead, and nothing when a call fails.

The ubertooth-btle(1) a `--capture` call runs writes its own decode of every
packet to its standard output, which is dropped: nobody asked this call for
it, and 8 seconds of ambient advertising is some 560 kB. Its standard error is
the script's, so a ubertooth-btle(1) that fails still says why. Wireshark
keeps the first 1024 bytes of the capture call's standard error and shows them
for the interface when the call fails.

`--fifo=-` is the one capture whose data is on standard output: it is the `-`
every file option here takes, for a capture run by hand rather than one
Wireshark asked for, and ubertooth-btle(1) writes the PCAP data there and no
decode.

A command line the script will not take is refused before ubertooth-btle(1)
runs, so nothing is written to the fifo: `error: unrecognized option
'--list-dlts'`, in the wording every other tool here uses for the same
mistake, with `error: option '--fifo' requires a value` for a missing value
and, for the two mistakes only a long option can make, `error: option
'--capture' takes no value` and `error: option '--extcap-c' is short for more
than one option`. A word that is not an option or the value of one is `error:
unexpected argument 'extra'`, since no call here takes an operand. The rest
are `error: --extcap-interface must be ubertooth<serial>, as
--extcap-interfaces lists it`, `error: --capture needs --fifo=PATH` and
`error: advertising channel index must be 37-39 (got 40)`, which is
ubertooth-btle(1)'s own wording for the same mistake. A call with an interface
and no operation in it gives the same `error: nothing to do: ...` line as a
bare call. Each is that one line: the usage line follows only a bare
`btle-extcap.py`, and `-h` prints it on standard output.

Whatever ubertooth-util(1) and ubertooth-btle(1) write to standard error is
passed on, including `error: no Ubertooth found with serial matching
'<serial>'` for a board that has been unplugged since the listing. Wireshark
shows what the `--capture` call wrote there for the interface when the call
fails; for the other calls it reads only standard output, so those lines land
on Wireshark's own error output, which is also what someone running the call
by hand sees.

The one line held back is ubertooth-util(1)'s `error: no Ubertooth devices
found`, and only when it is the sole error it reported about an empty listing,
since the empty listing says the same thing. Alongside any other `error: `
line it is passed on with the rest.

A tool that is not on the `PATH` Wireshark started the script with is reported
as `error: could not run ubertooth-util: No such file or directory`, and one
that is there but cannot be executed names that reason instead (`Permission
denied`). A ubertooth-util(1) that failed without saying why is reported as
`error: ubertooth-util failed (exit status 3)`, since a failure the user is
told about has to name itself.

A `-V` with no standard output to print the answer to, which is what
`btle-extcap.py -V >&-` leaves, is `error: standard output has gone away` and
exit 1. ubertooth-util(1) is not started there: with that descriptor closed,
the first file it opened would take its number and the answer would be
written into that. A descriptor open for reading only
(`btle-extcap.py -V 1<&0`) is the same line and status, and starts no
ubertooth-util(1) either: the answer is the child's to print, and nothing but
the child's own exit status would say it was refused.

An answer whose standard output has gone is that same line and exit 1,
whichever call was answering: the usage line `-h` prints, the interface
listing, the DLT, the configuration, and a capture filter's verdict. It
covers both descriptors a shell can leave, one closed before the script
started (`btle-extcap.py -h >&-`) and one open for reading only
(`btle-extcap.py -h 1<&0`), where the line reaches the buffer and the flush
is what fails. A `-V -h` that loses both halves of its answer says it once.
An answer with nothing in it loses nothing and is not reported: an interface
listing with no board to name, and the empty answer that says a capture
filter is good, still exit 0 with that stream gone.

With standard error gone, the `error: ` lines above are lost rather than
moved: none of them falls back to standard output, which Wireshark reads as
the answer to the call. That covers both descriptors a shell can leave there
as well, one closed (`btle-extcap.py --bogus 2>&-`) and one open for reading
only (`btle-extcap.py --bogus 2<&0`). The exit status is then all that is
left to say what happened, and it is the script's own.

## EXIT STATUS

The call either did what was asked, which is 0, or it did not, which is 1.
Nothing else is reported: Wireshark needs no more than non-zero for a failed
capture, so the script does not pass ubertooth-btle(1)'s own status through or
report the signal that killed it.

 - `0` :
   the call did what was asked. For `--capture` that includes a capture that
   was asked to stop, either way it ends: SIGTERM or SIGINT arrives and the
   script asks ubertooth-btle(1) to stop with a SIGTERM, or ubertooth-btle(1)
   dies of SIGPIPE because nothing is reading the fifo any more. It also
   includes the capture-filter verification call, whose answer is its standard
   output and not its status; see CAPTURE FILTERS
 - `1` :
   a usage error, a board that could not be opened, a tool that could not be
   run, a ubertooth-btle(1) that failed or was killed, or any other failure.
   That includes a partial listing: if `--extcap-interfaces` could not open
   one board it exits 1 even though it named the others, with the reason on
   standard error. Wireshark throws away everything a call that failed
   printed, so its capture list is then without the boards that were fine
   until that board is freed or unplugged. The one case that is not a failure
   is no board attached: the empty listing answers that, and exits 0

## SEE ALSO

ubertooth-btle(1): the tool this script runs, and what its capture contains

ubertooth-util(1): the board listing this script reads

ubertooth(7): overview of Project Ubertooth

`extcap(4)`, for the interface Wireshark expects, and
`host/python/extcap/README.md` in the source tree

## AUTHOR

Written by Mike Kershaw (dragorn) and Mike Ryan.

## COPYRIGHT

btle-extcap.py(1) is part of Project Ubertooth and is released under the
GPLv2. Refer to
[COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for
further details.
