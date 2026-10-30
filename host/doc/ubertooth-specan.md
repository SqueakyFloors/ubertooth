# ubertooth-specan(1) -- a spectrum analyzer for Ubertooth

## SYNOPSIS

    ubertooth-specan [-l <freq>] [-u <freq>] [-g|G] [-d <filename>|-] [-v]
    [-U <index|serial>]
    ubertooth-specan [-U <index|serial>] -V
    ubertooth-specan -h

## DESCRIPTION

ubertooth-specan(1) is a tool for using Ubertooth as a spectrum analyzer
in the 2.4GHz band. It is often used as a helper for GUI tools, such as
ubertooth-specan-ui(1) or Spectools. It can also produce output suitable for
use with the feedgnuplot tool.

Other options allow upper and lower bounds to be set on the frequencies
monitored. Refer to the [OPTIONS][] section for full details.

## OPTIONS

Options:

 - `-l <2402-2480>` :
   lower frequency in MHz (default 2402); the value is required when `-l` is used
 - `-u <2402-2480>` :
   upper frequency in MHz (default 2480); the value is required when `-u` is used
   and must not be lower than the selected lower frequency
 - `-g` :
   write the samples as text for feedgnuplot
 - `-G` :
   write them as text for 3D feedgnuplot
 - `-d <filename>` :
   write them to `<filename>` instead of the screen; `-d -` writes them to
   standard output. Without `-g` or `-G` they are the raw 3-byte records, so
   standard output must not be a terminal. See ARGUMENTS
 - `-v` :
   report the sweep on standard error; see VERBOSE OUTPUT. Give it at most
   once: a second `-v` is refused with
   `error: -v may be given only once`
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE
 - `-V` :
   print the version: the host tools, then the firmware version and build of
   each attached board; see ubertooth(7), COMMON OPTIONS
 - `-h` :
   print the usage text on standard output and exit 0. It takes effect once
   the whole command line has parsed, so a bad option value, a stray word or
   an option given twice is still reported.

## EXAMPLES

To monitor the 2.4GHz band and produce a file suitable for feedgnuplot:

    ubertooth-specan -g -d output.dat

To monitor the upper half of the 2.4GHz band, use:

    ubertooth-specan -l 2440

## ARGUMENTS

`ubertooth-specan` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them. Both answer on standard output, so a command line with both
prints both, the usage text last; the order they were written in makes no
difference.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: lower frequency must be 2402-2480 MHz (got bad)`.

An option the tool does not know, and an option whose value is missing, are
refused the same way, as one `error: ` line on standard error with no usage
text after it: `error: unrecognized option '-Z'` and
`error: option '-d' requires a value`.

`-` means standard output only as the whole value of `-d`. Any other value is a
file name, including one that starts with `-`: `-d -x` writes the file `-x`, and
a forgotten file name such as `-d -v` writes a file called `-v` rather than
streaming samples to the terminal and dropping the `-v`.

`-d` chooses where the samples go and `-g` or `-G` how they are written, so
the two do separate jobs and go together, in either order: `-g -d out.dat`
writes the gnuplot text into `out.dat`, and `-d out.bin` on its own writes the
raw records. `-d` used to create the file and leave it empty while a later
`-g` took the samples to the screen instead.

One sweep is written one way, so `-g` and `-G` in one command are refused
before any device is opened: `error: -G cannot be combined with -g; a sweep is
written in one format`. The second one used to be the one used.

`-d -` is for a pipe or a redirect when the samples are the raw records, which
fill a terminal with control characters and can leave it in a strange state,
so a terminal is refused before any device is opened:
`error: stdout is a tty, refusing to write binary samples to it`, with exit
status 1. ubertooth-btle(1) refuses its binary capture to a terminal the same
way. With `-g` or `-G` the samples are text, and a terminal is welcome to
them.

A refused command line creates and truncates nothing: `-d` names its file
while the command line is read, and it is opened only once every check has
passed. The board is opened and its firmware is checked before that, so a run
refused for either reason leaves no empty file behind either.

## VERBOSE OUTPUT

`-v` reports what the sweep is doing on standard error. It writes nothing to
standard output, so the samples are the same with it and without it, including
the raw records of `-d -`.

Three lines before the sweep starts name the board, the sweep as it will
actually run, and where the samples go:

    Using Ubertooth 1ff0ff1967102caf8334fa5ac52000f5
    Sweeping 2402-2480 MHz in 1 MHz steps, 79 samples per sweep
    Writing raw samples to standard output

The board's serial number is read from the board, so it is the one that was
selected even when `-U` named an enumeration index. A board that does not
answer gives `Using an Ubertooth whose serial number could not be read`
instead, and the sweep goes on.

While the sweep runs, one summary line a second, and one more when it ends,
labeled `Total after` so it is not read as another periodic line:

    After 1.0 s: 12 sweeps, 948 samples, 0 out of range

`sweeps` counts the passes over the range that completed, `samples` the
samples inside the range, and `out of range` the ones dropped because they
were left over from an earlier run, still queued on the board. The line is
printed whether samples arrived or not, so a board that has stopped sending
shows up within a second as counters that stop moving.

## GAPS IN A CAPTURE

A capture that is losing packets says so. The board holds captured packets in a
queue until this tool reads them, and a queue with no room left loses the next
packet. The first time it happens the capture warns `warning: the board's
packet queue filled up and it dropped at least one packet; this capture has a
gap`, and when the capture ends it says how many such gaps there were:
`warning: the capture has 3 gaps where the board's queue filled up and it
dropped packets`. The count is of gaps, not of lost packets: one full queue can
swallow several packets, and the board sends no number. Nothing says which
packets are missing, so a capture with gaps is complete only between them. The
board reports a gap on the first packet that gets through after it, so a
capture that ends in the moment after a gap does not report that last one.

## DIAGNOSTICS

Standard output carries only the samples — whatever `-d` has not sent to a
file, as text or, with no `-g` or `-G`, as the raw records — and the usage text
for `-h`.
Errors, warnings and progress, including `Ubertooth stopped`, go to standard
error. The errors this tool prints itself are one line starting `error: `, for
example `error: /no/such/dir/samples.bin: No such file or directory` for a
`-d` file that cannot be opened. libubertooth's own messages read the same
way, for example `error: no Ubertooth devices found`; a refusal from it can
run to more than one line, the lines after the first being its detail (see
ubertooth(7)).

If the samples cannot be written, `ubertooth-specan` says so once and exits 1,
rather than reporting every failed write and then exiting 0. A write that
fails while the sweep is running ends it there, since a full disk does not
come back. Every write is checked, the raw records and the text formats alike,
because `-d` sends either of them to a file:

    error: writing samples failed: wrote 0 of 3 bytes (<reason>)
    error: writing samples failed: could not write the sample (<reason>)

A write accepted into a buffer can still fail when that buffer is flushed, so
the output is flushed and checked after the sweep as well, whether it went to
a `-d` file, to standard output with `-d -`, or to standard output as text:

    error: writing samples failed: could not flush the samples (<reason>)

One case does not reach either line: when `-d -` writes into a pipe whose
reader has gone, the process is killed by `SIGPIPE` before the failed write
returns, so the shell reports status 141.

A sweep the board would not stop exits 1 as well: the request that takes the
board out of sweep mode was stalled, so the board is left sweeping and no
other tool can open it until it is reset or re-plugged. The samples that
arrived stay written out, and libubertooth names the refused request on
standard error.

A `-d` file is opened once the board has been opened and its firmware
accepted, not before, so a run refused for either reason leaves no empty file
behind; that is also why the message for a `-d` file that cannot be opened
comes after the board has been selected. A terminal on standard output is
still refused while the command line is read.

A run stopped by Ctrl-C before it had opened a board exits 0, with nothing on
standard error: see ubertooth(7), SELECTING A DEVICE.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Dominic Spill.

## COPYRIGHT

ubertooth-specan(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
