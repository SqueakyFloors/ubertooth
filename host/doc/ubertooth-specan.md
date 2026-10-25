# ubertooth-specan(1) -- a spectrum analyzer for Ubertooth

## SYNOPSIS

    ubertooth-specan [-l <freq>] [-u <freq>] [-g|G] [-d <filename>|-]

## DESCRIPTION

ubertooth-specan(1) is a tool for using Ubertooth as a spectrum analyzer
in the 2.4GHz band. It is often used as a helper for GUI tools, such as
ubertooth-specan-ui(1) or Spectools. It can also produce output suitable for
user with the feedgnuplot tool.

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
   format output for feedgnuplot
 - `-G` :
   format output for 3D feedgnuplot
 - `-d <filename>` :
   write the binary samples to `<filename>`; `-d -` writes them to standard
   output, which must not be a terminal. See ARGUMENTS
 - `-v` :
   print verbose output to stderr
 - `-U <index|serial>` :
   select an Ubertooth by enumeration index or serial number; see
   ubertooth(7), SELECTING A DEVICE

## EXAMPLES

To monitor the 2.4GHz band and produce a file suitable for feedgnuplot:

   ubertooth-specan -g -d output.dat

To monitor the upper half of the 2.4GHz band, use:

   ubertooth-specan -l 2440

## ARGUMENTS

`ubertooth-specan` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` ends the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside it.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: lower frequency must be 2402-2480 MHz (got bad)`.

`-` means standard output only as the whole value of `-d`. Any other value is a
file name, including one that starts with `-`: `-d -x` writes the file `-x`, and
a forgotten file name such as `-d -v` writes a file called `-v` rather than
streaming samples to the terminal and dropping the `-v`.

`-d -` is for a pipe or a redirect. The samples are raw 3-byte records, which
fill a terminal with control characters and can leave it in a strange state, so
a terminal is refused before any device is opened:
`error: stdout is a tty, refusing to write binary samples to it`, with exit
status 1. ubertooth-btle(1) refuses its binary capture to a terminal the same
way.

A refused command line creates and truncates nothing: `-d` names its file while
the command line is read, and it is opened only once every check has passed.

## DIAGNOSTICS

Standard output carries only the samples — the text lines in the default, `-g`
and `-G` modes, or the raw records with `-d -` — and the usage text for `-h`.
Errors, warnings and progress, including `Ubertooth stopped`, go to standard
error. The errors this tool prints itself are one line starting `error: `, for
example `error: /no/such/dir/samples.bin: No such file or directory` for a
`-d` file that cannot be opened. The unknown-option line from the option
parser and libubertooth's own messages, such as
`could not open Ubertooth device`, do not follow that yet.

If the samples cannot be written, `ubertooth-specan` says so once and exits 1,
rather than reporting every failed write and then exiting 0. A write that
fails while the sweep is running ends it there, since a full disk does not
come back:

    error: writing samples failed: wrote 0 of 3 bytes (<reason>)

A write accepted into a buffer can still fail when that buffer is flushed, so
the output is flushed and checked after the sweep as well, whether it went to
a `-d` file, to standard output with `-d -`, or to standard output as text:

    error: writing samples failed: could not flush the samples (<reason>)

One case does not reach either line: when `-d -` writes into a pipe whose
reader has gone, the process is killed by `SIGPIPE` before the failed write
returns, so the shell reports status 141.

## SEE ALSO

ubertooth(7): overview of Project Ubertooth; OPTION VALUES describes how
option values are written

## AUTHOR

This manual page was written by Dominic Spill.

## COPYRIGHT

ubertooth-specan(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
