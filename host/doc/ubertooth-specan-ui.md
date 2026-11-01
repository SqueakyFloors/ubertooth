# ubertooth-specan-ui(1) -- graphical 2.4 GHz spectrum analyzer for Ubertooth

## SYNOPSIS

    ubertooth-specan-ui [-U <index|serial>] [-l <freq>] [-u <freq>]
    [--wifi[=<channel(s)>]] [--padding <MHz>]
    ubertooth-specan-ui [-U <index|serial>] -V
    ubertooth-specan-ui -h

## DESCRIPTION

ubertooth-specan-ui(1) shows the 2.4 GHz band as a live spectrum plot in a
window. It runs ubertooth-specan(1) in the background and draws what it
reports, so it needs an attached Ubertooth running the `bluetooth_rxtx`
firmware, Python 3.8 or newer, and the Python packages `numpy`, `qtpy` and a
Qt binding (`PySide6` or `PyQt5`).

To show the whole band on the only attached Ubertooth, run:

    ubertooth-specan-ui

To pick one of several Ubertooths, run:

    ubertooth-specan-ui -U 0

## OPTIONS

 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-l <2402-2480>` :
   lower frequency in MHz (default 2402)
 - `-u <2402-2480>` :
   upper frequency in MHz (default 2480); must not be lower than `-l`. The
   plot is drawn 2 MHz wider than the scanned range on each side, within
   2400-2483 MHz, so the edge channels can be marked
 - `--wifi[=<channel(s)>]` :
   show the band around one Wi-Fi channel (for example `--wifi=6`) or a range
   of channels (`--wifi=1-11`); `--wifi` alone shows channel 1. Channel 14 is
   centered on 2484 MHz, above the 2480 MHz the board can tune, so `--wifi=14`
   shows the part of that channel below 2480 MHz -- and with a `--padding`
   under 4 it is refused, because then the whole band asked for is above the
   range
 - `--padding <MHz>` :
   extra band shown on each side with `--wifi` (default 10), limited to
   2402-2480 MHz
 - `-V` :
   print the version and end the command, before the window opens: the host
   tools, then the firmware version and build of the board `-U` names, or of
   each attached board. The same answer every other tool gives, including
   with no board attached; see ubertooth(7), COMMON OPTIONS. This tool answers
   it by running ubertooth-util(1) `-V`, so that tool has to be on the `PATH`,
   and is reported as `error: could not run ubertooth-util: No such file or
   directory` when it is not
 - `-h`, `--help` :
   print the usage text on standard output and exit 0.

`-V` and `-h` both end the command, and both answer on standard output, so a
command line with both prints both: the version first and the usage text
last, whichever order they were written in. The status is then `-V`'s, so a
board that could not be read makes a command line carrying `-h` exit 1, with
the usage text printed all the same. Both come after the range checks below,
so a range this tool will not take is reported instead.

The whole command line is read before the window's own modules are imported,
so `-V`, `-h` and a refused range are answered on a machine with no `numpy`
and no Qt binding. Only opening the window needs those.

## DISPLAY

 - White trace: the current sweep.
 - Green trace: the highest level seen recently (max hold).
 - Red dotted lines: the strongest peak, labeled with its frequency and level.
 - Cyan dotted line, labeled "Avg: N dBm": the average of the green trace
   across the band, smoothed so it moves slowly.
 - Yellow and magenta lines: points marked with the left and right mouse
   buttons, with the frequency differences to the peak and to each other.

The axis and the markers show MHz. Pressing `n` shows BLE channel numbers
instead -- the advertising channels 37, 38, and 39, and the data channels 0 to
36 -- and pressing it again goes back to MHz. In channel mode the axis is
marked on the channels themselves, so a line on the graph lines up with the
number under it, and the three advertising channels are always named when they
are in the plot. A marker names the channel
nearest the point it sits on, since the sweep steps 1 MHz where the channels
are 2 MHz apart. The difference between two markers is in MHz in both modes,
written `(+4 MHz)`, because the gap between two channel numbers says nothing
about the gap on the air. The margin either side of the band has no
channel, so a point out there keeps its frequency.

## KEYS AND MOUSE

Press `h` in the window to show this list on screen. Keys work in upper or
lower case. There are two mouse markers, one per button: clicking again moves
that marker. On a Mac trackpad, right click is a two-finger click or
Control-click.

 - `left click` :
   set marker 1 (yellow) at the pointer's frequency and level
 - `right click` :
   set marker 2 (magenta); the frequency differences to marker 1 and to the
   peak are shown
 - `middle click` or `m` :
   show or hide the peak and average markers, and clear the mouse markers
 - `n` :
   label the axis and the markers with BLE channel numbers instead of MHz, or
   back again
 - `c` :
   clear the graph
 - `h` :
   show or hide the key help
 - `q` :
   quit

## DIAGNOSTICS

A range `ubertooth-specan-ui` will not take is refused before the window
opens, with one line starting `error: ` on standard error and exit status 1:
`error: invalid channel range: <value>`,
`error: channel <n> is not a valid wifi channel`,
`error: lower frequency must be <min>-<max> MHz (got <value>)` and the same for
`upper`, `error: frequency of <n> MHz is below minimum frequency of <min>`,
`error: frequency of <n> MHz is above maximum frequency of <max>` and
`error: lower frequency of <n> MHz is above upper frequency of <n> MHz`. They
used to be `ERROR: ...` on standard output.

A command line the argument parser will not read is refused the same way, in
the lines the other tools here give for the same mistakes:
`error: unrecognized option '--bogus'` for an option this tool does not have,
`error: unexpected argument 'extra'` for a word that is neither an option nor
an option's value, and `error: option '-l' requires a value` for an option
given none. The word is quoted as the parser read it, so a run of letters it
could not place is named whole: `error: unrecognized option '-Zx'`. A `--`
ends the options, so what follows it is read as a word however it is spelled,
and `--` on its own asks for nothing beyond the defaults and runs. No usage
text follows any of these lines, which say what was wrong; `-h` prints it.
These command lines used to open the window and start a sweep, the unreadable
part dropped without a word. A value the parser cannot read, as in
`--padding x`, goes the same way but keeps the parser's own words
(`error: argument --padding: invalid int value: 'x'`); it used to print the
usage text and exit 2. `-l` and `-u` are read by the tool itself, so a value
that is not a number is refused in the same words as one outside the band:
`error: lower frequency must be 2402-2480 MHz (got abc)`, as
`ubertooth-specan` gives.

A failure to start `ubertooth-specan` is reported the same way, as
`error: could not open an Ubertooth device` followed by a `note: ` naming the
command, and whatever `ubertooth-specan` itself said on standard error.

A `-V` with no standard output to print the answer to, which is what
`ubertooth-specan-ui -V >&-` leaves, is `error: standard output has gone away`
and exit 1. ubertooth-util(1) is not started there: with that descriptor
closed, the first file it opened would take its number and the answer would be
written into that. A descriptor open for reading only
(`ubertooth-specan-ui -V 1<&0`) is the same line and status, and starts no
ubertooth-util(1) either: the answer is the child's to print, and nothing but
the child's own exit status would say it was refused.

The key help `h` shows goes to standard output, as what the user asked for.
The notes about an unknown or unsupported key are `note: ` lines on standard
error. The usage text `-h` prints is the argument parser's, printed by the
tool: standard output, exit 0. The option itself is the tool's own, so that a
range the tool will not take is reported even beside it, as it is in every
other tool here.

A `-h` with nowhere to print that text is `error: standard output has gone
away` and exit 1: a command that printed nothing did not do what was asked.
That covers both descriptors a shell can leave, one closed before the tool
started (`ubertooth-specan-ui -h >&-`) and one open for reading only
(`ubertooth-specan-ui -h 1<&0`). Left to the argument parser, the first of
those writes the text to standard error instead. A `-V -h` that loses both
halves of its answer says the line once.

With standard error gone, the `error: ` lines above are lost rather than
moved: none of them falls back to standard output, where a reader or a script
is entitled to find only what it asked for. A standard error open for reading
only (`ubertooth-specan-ui -l 2000 2<&0`), closed
(`ubertooth-specan-ui -l 2000 2>&-`), or a pipe whose reader has left
(`ubertooth-specan-ui -l 2000 2>&1 | true`) all go the same way. The exit
status is then all that is left to say what happened, and it is the tool's
own: it used to be 120, from the flush the interpreter makes as it exits.

## SEE ALSO

ubertooth-specan(1), ubertooth-util(1)

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## COPYRIGHT

ubertooth-specan-ui(1) is part of Project Ubertooth and is released under the
GPLv2. Refer to `COPYING` for further details.
