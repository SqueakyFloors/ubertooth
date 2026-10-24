# ubertooth-specan-ui(1) -- graphical 2.4 GHz spectrum analyzer for Ubertooth

## SYNOPSIS

`ubertooth-specan-ui` [-U <index|serial>] [-l <freq>] [-u <freq>]
[--wifi[=<channel(s)>]] [--padding <MHz>]

## DESCRIPTION

ubertooth-specan-ui(1) shows the 2.4 GHz band as a live spectrum plot in a
window. It runs ubertooth-specan(1) in the background and draws what it
reports, so it needs an attached Ubertooth running the `bluetooth_rxtx`
firmware and the Python packages `numpy`, `qtpy` and `PySide6`.

To show the whole band on the only attached Ubertooth, run:

    ubertooth-specan-ui

To pick one of several Ubertooths, run:

    ubertooth-specan-ui -U 0

## OPTIONS

 - `-U <index|serial>` :
   select the Ubertooth by USB enumeration index or by serial number (see
   ubertooth-util(1), SELECTING A DEVICE)
 - `-l <2402-2480>` :
   lower frequency in MHz (default 2402)
 - `-u <2402-2480>` :
   upper frequency in MHz (default 2480); must not be lower than `-l`. The
   plot is drawn 2 MHz below and 3 MHz above the scanned range, within
   2400-2483 MHz, so the edge channels are not on the window border
 - `--wifi[=<channel(s)>]` :
   show the band around one Wi-Fi channel (for example `--wifi=6`) or a range
   of channels (`--wifi=1-11`); `--wifi` alone shows channel 1
 - `--padding <MHz>` :
   extra band shown on each side with `--wifi` (default 10), limited to
   2402-2480 MHz

## DISPLAY

 - White trace: the current sweep.
 - Green trace: the highest level seen recently (max hold).
 - Red dotted lines: the strongest peak, labelled with its frequency and level.
 - Cyan dotted line, labelled "Avg: N dBm": the average of the green trace
   across the band, smoothed so it moves slowly.
 - Yellow and magenta lines: points marked with the left and right mouse
   buttons, with the frequency differences to the peak and to each other.

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
 - `c` :
   clear the graph
 - `h` :
   show or hide the key help
 - `q` :
   quit

## SEE ALSO

ubertooth-specan(1), ubertooth-util(1), ubertooth(7)

## COPYRIGHT

ubertooth-specan-ui(1) is part of Project Ubertooth and is released under the
GPLv2. Refer to `COPYING` for further details.
