# UBERTOOTH-TX 1 "Project Ubertooth" "User Commands"

## NAME

ubertooth-tx(1) - retransmit symbols dumped with ubertooth-dump

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

## SEE ALSO

ubertooth-dump(1), ubertooth(7)

## COPYRIGHT

Released under the GPLv2. Refer to `COPYING` for further details.
