#!/usr/bin/env python3

# Copyright 2013, 2023 Mike Ryan
#
# This file is part of Project Ubertooth.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.

import getopt
import re
import signal
import sys
from subprocess import Popen, PIPE

child = None

def sig_handler(sig, frame):
    if child is not None:
        child.terminate()

def main():
    signal.signal(signal.SIGTERM, sig_handler)

    try:
        opts, args = getopt.getopt(
            sys.argv[1:], "h",
            [
                "help",
                "extcap-interfaces",
                "extcap-dlts",
                "extcap-config",
                "capture",
                "extcap-interface=",
                "fifo=",
                "channel=",
            ])
    except getopt.GetoptError as err:
        # A usage error: the message and the usage line go to standard error
        # and the exit status is 1 (.kiro/steering/cli-output.md).
        print("error: %s" % err, file=sys.stderr)
        usage(sys.stderr)
        sys.exit(1)

    interface = ''
    fifo = None
    channel = "37"
    do_list_dlts = False
    do_config = False
    do_capture = False

    for o, a in opts:
        if o in ("-h", "--help"):
            usage()
            sys.exit()
        elif o == "--extcap-interfaces":
            list_interfaces()
            exit(0)
        elif o == "--extcap-dlts":
            do_list_dlts = True
        elif o == "--extcap-config":
            do_config = True
        elif o == "--capture":
            do_capture = True
        elif o == "--extcap-interface":
            interface = a
        elif o == "--fifo":
            fifo = a
        elif o == "--channel":
            channel = a
        else:
            assert False, "unhandled option"

    # every operation down here depends on having an interface. Its value is
    # "ubertooth<serial>", as --extcap-interfaces lists it; an older
    # "ubertooth<index>" still works, because ubertooth-btle -U reads an
    # all-digit value as an index.
    m = re.match(r'ubertooth([0-9a-fA-F]+)$', interface)
    if not m:
        print("error: --extcap-interface must be ubertooth<serial>, as "
              "--extcap-interfaces lists it", file=sys.stderr)
        exit(1)
    interface = m.group(1)

    if do_list_dlts:
        list_dlts()
    elif do_config:
        config()
    elif do_capture:
        if fifo is None:
            print("error: --capture needs --fifo=PATH", file=sys.stderr)
            exit(1)
        capture(interface, fifo, channel)


def usage(out=sys.stdout):
    print("Usage: %s --extcap-interfaces |"
          " --extcap-interface=ubertooth<serial>"
          " (--extcap-dlts | --extcap-config |"
          " --capture --fifo=PATH [--channel=37|38|39])" % sys.argv[0],
          file=out)


# One "<index>: <serial>" line per attached board, which is what
# "ubertooth-util -U all -s" prints for any number of boards.
INTERFACE_LINE = re.compile(r'(\d+): ([0-9a-fA-F]+)$')


def list_interfaces():
    # -U all asks for the indexed listing explicitly, so one output form
    # covers one board and several. Each interface is named by the board's
    # serial, which --capture passes to "ubertooth-btle -U <serial>": an index
    # can change when boards are re-plugged between listing and capture, a
    # serial cannot. A board whose serial could not be read is shown by
    # ubertooth-util as "<index>: <unavailable>" and has no serial to offer,
    # so it is left out.
    proc = Popen(['ubertooth-util', '-U', 'all', '-s'],
                 stdout=PIPE, stderr=PIPE, universal_newlines=True)
    out = proc.communicate()[0]

    for line in out.split('\n'):
        m = INTERFACE_LINE.match(line.strip())
        if m:
            print("interface {value=ubertooth%s}{display=Ubertooth One %s}"
                  % (m.group(2), m.group(2)))


def list_dlts():
    print("dlt {number=256}{name=DLT_BLUETOOTH_LE_LL_WITH_PHDR}{display=Bluetooth LE}\n")


def config():
    args = []
    args.append((0, '--channel', 'Advertising Channel', 'selector'))

    values = []
    values.append((0, "37", "37", "true"))
    values.append((0, "38", "38", "false"))
    values.append((0, "39", "39", "false"))

    for arg in args:
        print("arg {number=%d}{call=%s}{display=%s}{type=%s}" % arg)

    for value in values:
        print("value {arg=%d}{value=%s}{display=%s}{default=%s}" % value)


def capture(interface, fifo, channel):
    global child
    p = Popen([
        "ubertooth-btle", "-f",
        "-U", interface,
        "-q", fifo,
        "-A", channel,
    ])
    child = p
    p.wait()

if __name__ == "__main__":
    main()
