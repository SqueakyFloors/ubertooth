# ubertooth-tx(1) -- retransmit symbols dumped with ubertooth-dump

## SYNOPSIS

ubertooth-tx [-l LAP] [-u UAP] [-t SECONDS] [-U 0-7] [-V]

## DESCRIPTION

Intended to retransmit symbols captured with ubertooth-dump toward a target
piconet identified by LAP/UAP.

**Status: non-functional.** The tool currently performs no useful transmission
(its own usage output warns "This tool currently does nothing"). It is retained
as a placeholder/experimental entry point. Do not rely on it.

## OPTIONS

 - `-l <LAP>` :
   target LAP (3 bytes / 6 hex digits).
 - `-u <UAP>` :
   target UAP (1 byte / 2 hex digits).
 - `-t <seconds>` :
   timeout; 0 means no timeout (default: 0).
 - `-U <0-7>` :
   which Ubertooth device to use.
 - `-V` :
   print version information.

## TRANSMIT POWER

Transmitting uses the CC2400 power level set with `ubertooth-util -a` and leaves
it unchanged. A board that has not been given a level transmits at 7 (0 dBm),
the CC2400's reset value. These levels are at the CC2400 radio; the Ubertooth
One's RF amplifier changes the actual output after that.

## ARGUMENTS

`ubertooth-tx` takes options only. A word that is not an option, including a
bare `-`, is refused before any device is opened and named:
`error: unexpected argument '<word>'`, with exit status 1.

`-h` and `-V` end the command only once the whole command line has
parsed, so a stray word or a bad option value is reported even
alongside them.

An option value must be a whole value in its range, or it is refused the same
way instead of being read as 0, for example
`error: LAP must be 6 hex digits (got 4466)`.

## SEE ALSO

ubertooth-dump(1), ubertooth(7)

## COPYRIGHT

Released under the GPLv2. Refer to [COPYING](https://github.com/SqueakyFloors/ubertooth/blob/main/COPYING) for further details.
