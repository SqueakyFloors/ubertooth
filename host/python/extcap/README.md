# btle-extcap.py

An external capture program, which lets Wireshark capture BLE advertising
packets from an Ubertooth directly, with no capture file in between. Wireshark
calls it; it runs `ubertooth-btle` and lets it write PCAP data into the fifo
Wireshark opened.

Written by Mike Kershaw (dragorn) and Mike Ryan, and distributed under the
terms of the GNU GPL, as is the rest of the Ubertooth project.

## Where the documentation is

The reference is the script's manual page, `btle-extcap(1)`, written in
[host/doc/btle-extcap.md](../../doc/btle-extcap.md): installing it, every call
Wireshark makes and the options it passes, the exit statuses, and the
diagnostics. Read it with `man btle-extcap` once the tools are installed.

For the one-page user guide — installing the plugin, starting a capture, and
using it from `tshark` — see
[Using Ubertooth with Wireshark](../../../docs/source/using_ubertooth_with_wireshark.rst).

## Installing it

Copy or link the script into Wireshark's personal extcap directory, which
Wireshark itself names under Help, About Wireshark, Folders ("Personal Extcap
path"); the path differs between Wireshark versions and platforms, and
Wireshark 4.4 and later look in a `wireshark` subdirectory of it:

    ln -s "$PWD/btle-extcap.py" <that path>/

The script must be executable, and `ubertooth-util` and `ubertooth-btle` must
be on the `PATH` Wireshark starts it with. Each attached board then appears in
Wireshark's capture list as "Ubertooth One `<serial>`".

## Running it by hand

Worth doing when a board does not appear in Wireshark. These are the calls
Wireshark itself makes; `btle-extcap(1)` describes each one and what it
answers.

    btle-extcap.py --extcap-interfaces              # one line per attached board
    btle-extcap.py --extcap-interface=ubertooth<serial> --extcap-dlts
    btle-extcap.py --extcap-interface=ubertooth<serial> --capture --fifo=- > adv.pcap

`-V` prints the version and `-h` the usage line, as in every other tool.

## See also

`ubertooth-btle` and `ubertooth-util` (`host/doc/ubertooth-btle.md`,
`host/doc/ubertooth-util.md`), and `extcap(4)` for the interface Wireshark
expects.
