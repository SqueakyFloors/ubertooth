# ubertooth-specan(1) -- a spectrum analyzer for Ubertooth

## SYNOPSIS

ubertooth-specan [-l <freq>] [-u <freq>] [-g|G]

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
 - `-d<filename>` :
   output to file <filename>
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

## SEE ALSO

ubertooth(7): overview of Project Ubertooth

## AUTHOR

This manual page was written by Dominic Spill.

## COPYRIGHT

ubertooth-specan(1) is Copyright (c) 2010-2017. This tool is released under the
GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
